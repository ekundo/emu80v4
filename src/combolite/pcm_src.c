/* Источники проигрывателя PCM: файл по расширению -> 16-разрядные отсчёты.
 *
 *   .wav   PCM 8 или 16 бит, моно или стерео, любая частота (пересчитывает
 *          pcm_play.c)
 *   .mp3   minimp3 (pcm_mp3.c)
 *   .mod   micromod (pcm_mod.c): ProTracker и многоканальные до 32 каналов
 *
 * Переносимо: stdio, без ESP-IDF -- гоняется на маке (fw/test_pcm.c).
 * WAV и MP3 читают через pcm_io_t: файл или ячейку, которую игра ещё дописывает
 * (тогда «пока нет» -- PCM_SRC_WAIT, а не конец). */
#include "pcm_src.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------- ввод */
/* Кэш чтения ячейки-файла (14.11.2026). Файл ячейки открыт без буфера stdio
 * (дописывание -- одной записью 4 КБ, pcm_play.c), и каждое чтение по 1 КБ
 * шло через переход по файлу и невыровненные сектора: на плате №1 через мост
 * SD это 138 блоков кольца в секунду при нужных 172 -- разрывы звука. Здесь
 * читается кусок PCM_IO_CACHE байт по его границе, дальше -- из памяти. До
 * записанной границы (*have) файл не меняется, так что кэш верен; источник
 * у проигрывателя один. */
#ifndef PCM_IO_CACHE
#define PCM_IO_CACHE 2048
#endif
static uint8_t     ioc[PCM_IO_CACHE];
static const FILE *ioc_f;
static long        ioc_at = -1, ioc_n;

unsigned long pcm_io_reads, pcm_io_fread_n;      /* отладка: вызовов, fread с карты */
static long slot_file_read(pcm_io_t *io, uint8_t *dst, long n)
{
    pcm_io_reads++;
    long got = 0;
    while (got < n) {
        long p = io->pos + got, k;
        if (!(ioc_f == io->f && p >= ioc_at && p < ioc_at + ioc_n)) {
            long base = p & ~(long)(PCM_IO_CACHE - 1), want = *io->have - base;
            size_t r;
            if (want > PCM_IO_CACHE) want = PCM_IO_CACHE;
            if (want <= p - base || fseek(io->f, base, SEEK_SET) != 0) break;
            r = fread(ioc, 1, (size_t)want, io->f);
            pcm_io_fread_n++;
            ioc_f = io->f; ioc_at = base; ioc_n = (long)r;
            if (p >= base + (long)r) break;
        }
        k = ioc_at + ioc_n - p;
        if (k > n - got) k = n - got;
        memcpy(dst + got, ioc + (p - ioc_at), (size_t)k);
        got += k;
    }
    return got;
}

long pcm_io_read(pcm_io_t *io, void *dst, long n)
{
    long left;
    if (io->f && !io->have) {
        size_t r = fread(dst, 1, (size_t)n, io->f);
        io->pos += (long)r;
        return (long)r;
    }
    left = *io->have - io->pos;
    if (left <= 0) return pcm_io_whole(io) ? 0 : PCM_SRC_WAIT;
    if (n > left) n = left;
    if (io->f) {
        /* ячейка-файл: тот же FILE дописывает pcm_play.c, позиция у него своя */
        long r = slot_file_read(io, dst, n);
        if (!r) return -1;
        io->pos += r;
        return r;
    }
    memcpy(dst, *io->d + io->pos, (size_t)n);
    io->pos += n;
    return n;
}

int pcm_io_seek(pcm_io_t *io, long pos)
{
    if (io->f && !io->have && fseek(io->f, pos, SEEK_SET) != 0) return -1;
    io->pos = pos;
    return 0;
}

int pcm_io_whole(const pcm_io_t *io)
{
    return (io->f && !io->have) || !io->size || *io->have >= io->size;
}

void pcm_io_close(pcm_io_t *io)
{
    if (ioc_f == io->f) { ioc_f = 0; ioc_n = 0; }   /* тот же FILE* может достаться другому файлу */
    if (io->f && !io->have) fclose(io->f);
    io->f = 0;
}

/* ------------------------------------------------------------------ WAV */
typedef struct {
    pcm_io_t io;
    long  data, len;                    /* начало и длина данных, байты */
    int   bits;                         /* 8 или 16 */
    int   ch;
} wav_t;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Ровно n байт заголовка; дозаписываемая ячейка без них -- не WAV (пока). */
static int io_get(pcm_io_t *io, void *dst, long n)
{
    return pcm_io_read(io, dst, n) == n ? 0 : -1;
}

static int wav_read(pcm_src_t *s, int16_t *dst, int n)
{
    wav_t *w = s->st;
    uint8_t buf[1024];
    int fb = w->ch * w->bits / 8, got = 0;
    while (got < n) {
        long left = (w->data + w->len - w->io.pos) / fb, r;
        int k = n - got, i;
        if (left <= 0) break;
        if (k > (int)(sizeof buf / fb)) k = (int)(sizeof buf / fb);
        if (k > left) k = (int)left;
        r = pcm_io_read(&w->io, buf, (long)k * fb);
        if (r == PCM_SRC_WAIT && !got) return PCM_SRC_WAIT;
        if (r <= 0) break;
        if (r % fb) pcm_io_seek(&w->io, w->io.pos - r % fb);   /* полкадра -- в следующий раз */
        r /= fb;
        if (!r) break;
        for (i = 0; i < r * w->ch; i++)
            dst[got * w->ch + i] = w->bits == 8 ? (int16_t)((buf[i] - 128) << 8)
                                                : (int16_t)le16(buf + 2 * i);
        got += (int)r;
    }
    return got;
}

static int wav_rewind(pcm_src_t *s)
{
    wav_t *w = s->st;
    return pcm_io_seek(&w->io, w->data);
}

static void wav_close(pcm_src_t *s)
{
    wav_t *w = s->st;
    if (!w) return;
    pcm_io_close(&w->io);
    free(w);
    s->st = 0;
}

/* 0 -- источник готов и владеет io */
static int wav_open(const pcm_io_t *io0, pcm_src_t *s)
{
    uint8_t h[12], c[8], fmt[16];
    int have_fmt = 0;
    wav_t *w = calloc(1, sizeof *w);
    if (!w) return -1;
    w->io = *io0;
    if (io_get(&w->io, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { free(w); return -1; }
    while (io_get(&w->io, c, 8) == 0) {
        long sz = (long)le32(c + 4);
        if (!memcmp(c, "fmt ", 4) && sz >= 16) {
            if (io_get(&w->io, fmt, 16)) break;
            if (le16(fmt) != 1) break;                      /* только PCM */
            w->ch = le16(fmt + 2);
            s->rate = (int)le32(fmt + 4);
            w->bits = le16(fmt + 14);
            have_fmt = 1;
            pcm_io_seek(&w->io, w->io.pos + sz - 16 + (sz & 1));
        }
        else if (!memcmp(c, "data", 4) && have_fmt) {
            if ((w->bits != 8 && w->bits != 16) || (w->ch != 1 && w->ch != 2)) break;
            w->data = w->io.pos; w->len = sz;
            s->ch = w->ch;
            s->read = wav_read; s->rewind = wav_rewind; s->close = wav_close;
            s->st = w;
            return 0;
        }
        else pcm_io_seek(&w->io, w->io.pos + sz + (sz & 1));
    }
    free(w);
    return -1;
}

/* -------------------------------------------------------------- по имени */
static int ext_is(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

int pcm_src_known(const char *name)
{
    return ext_is(name, ".wav") || ext_is(name, ".mp3") || ext_is(name, ".mod");
}

/* Ячейка проигрывателя: формат -- по содержимому. MOD читается прямо из неё
 * и только целый; WAV и MP3 -- через pcm_io_t, пока её дописывают. */
int pcm_src_open_mem(uint8_t *const *d, const long *have, long size, pcm_src_t *s)
{
    pcm_io_t io = { 0, d, have, size, 0 };
    memset(s, 0, sizeof *s);
    if (*have >= 12 && !memcmp(*d, "RIFF", 4) && !memcmp(*d + 8, "WAVE", 4))
        return wav_open(&io, s);
    if (pcm_io_whole(&io) && pcm_mod_open_mem(*d, *have, s) == 0) {
        s->fixed = 1;
        return 0;
    }
    return pcm_mp3_open(&io, s);
}

int pcm_src_open_slot_file(FILE *f, const long *have, long size, pcm_src_t *s)
{
    pcm_io_t io = { f, 0, have, size, 0 };
    uint8_t h[12];
    int wav;
    memset(s, 0, sizeof *s);
    wav = *have >= 12 && pcm_io_read(&io, h, 12) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4);
    io.pos = 0;
    return wav ? wav_open(&io, s) : pcm_mp3_open(&io, s);
}

int pcm_src_open(const char *path, pcm_src_t *s)
{
    FILE *f;
    memset(s, 0, sizeof *s);
    if (!pcm_src_known(path)) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    {
        pcm_io_t io = { f, 0, 0, 0, 0 };
        if (ext_is(path, ".wav") && wav_open(&io, s) == 0) return 0;
        if (ext_is(path, ".mp3") && pcm_mp3_open(&io, s) == 0) return 0;
    }
    if (ext_is(path, ".mod") && pcm_mod_open(f, s) == 0) return 0;
    fclose(f);
    return -1;
}
