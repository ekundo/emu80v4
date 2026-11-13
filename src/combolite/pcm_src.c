/* Источники проигрывателя PCM: файл по расширению -> 16-разрядные отсчёты.
 *
 *   .wav   PCM 8 или 16 бит, моно или стерео, любая частота (пересчитывает
 *          pcm_play.c)
 *   .mp3   minimp3 (pcm_mp3.c)
 *   .mod   micromod (pcm_mod.c): ProTracker и многоканальные до 32 каналов
 *
 * Переносимо: stdio, без ESP-IDF -- гоняется на маке (fw/test_pcm.c). */
#include "pcm_src.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ WAV */
typedef struct {
    FILE *f;
    long  data, len, pos;               /* начало, длина данных и позиция, байты */
    int   bits;                         /* 8 или 16 */
    int   ch;
} wav_t;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int wav_read(pcm_src_t *s, int16_t *dst, int n)
{
    wav_t *w = s->st;
    uint8_t buf[1024];
    int fb = w->ch * w->bits / 8, got = 0;
    while (got < n) {
        long left = (w->len - w->pos) / fb;
        int k = n - got, i, r;
        if (left <= 0) break;
        if (k > (int)(sizeof buf / fb)) k = (int)(sizeof buf / fb);
        if (k > left) k = (int)left;
        r = (int)fread(buf, (size_t)fb, (size_t)k, w->f);
        if (r <= 0) break;
        for (i = 0; i < r * w->ch; i++)
            dst[got * w->ch + i] = w->bits == 8 ? (int16_t)((buf[i] - 128) << 8)
                                                : (int16_t)le16(buf + 2 * i);
        got += r;
        w->pos += (long)r * fb;
    }
    return got;
}

static int wav_rewind(pcm_src_t *s)
{
    wav_t *w = s->st;
    w->pos = 0;
    return fseek(w->f, w->data, SEEK_SET);
}

static void wav_close(pcm_src_t *s)
{
    wav_t *w = s->st;
    if (!w) return;
    fclose(w->f);
    free(w);
    s->st = 0;
}

static int wav_open(FILE *f, pcm_src_t *s)
{
    uint8_t h[12], c[8], fmt[16];
    int have_fmt = 0;
    wav_t *w;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    w = calloc(1, sizeof *w);
    if (!w) return -1;
    while (fread(c, 1, 8, f) == 8) {
        long sz = (long)le32(c + 4);
        if (!memcmp(c, "fmt ", 4) && sz >= 16) {
            if (fread(fmt, 1, 16, f) != 16) break;
            if (le16(fmt) != 1) break;                      /* только PCM */
            w->ch = le16(fmt + 2);
            s->rate = (int)le32(fmt + 4);
            w->bits = le16(fmt + 14);
            have_fmt = 1;
            fseek(f, sz - 16 + (sz & 1), SEEK_CUR);
        }
        else if (!memcmp(c, "data", 4) && have_fmt) {
            if ((w->bits != 8 && w->bits != 16) || (w->ch != 1 && w->ch != 2)) break;
            w->f = f; w->data = ftell(f); w->len = sz;
            s->ch = w->ch;
            s->read = wav_read; s->rewind = wav_rewind; s->close = wav_close;
            s->st = w;
            return 0;
        }
        else fseek(f, sz + (sz & 1), SEEK_CUR);
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

/* Ячейка проигрывателя: формат -- по содержимому. WAV и MP3 читаются из
 * памяти как из файла (fmemopen), MOD -- прямо из неё. */
int pcm_src_open_mem(const uint8_t *d, long len, pcm_src_t *s)
{
    FILE *f;
    memset(s, 0, sizeof *s);
    if (len >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) {
        if (!(f = fmemopen((void *)d, (size_t)len, "rb"))) return -1;
        if (wav_open(f, s) == 0) return 0;
        fclose(f);
        return -1;
    }
    if (pcm_mod_open_mem(d, len, s) == 0) return 0;
    if (!(f = fmemopen((void *)d, (size_t)len, "rb"))) return -1;
    if (pcm_mp3_open(f, s) == 0) return 0;
    fclose(f);
    return -1;
}

int pcm_src_open(const char *path, pcm_src_t *s)
{
    FILE *f;
    memset(s, 0, sizeof *s);
    if (!pcm_src_known(path)) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    if (ext_is(path, ".wav") && wav_open(f, s) == 0) return 0;
    if (ext_is(path, ".mp3") && pcm_mp3_open(f, s) == 0) return 0;
    if (ext_is(path, ".mod") && pcm_mod_open(f, s) == 0) return 0;
    fclose(f);
    return -1;
}
