/* Источник MP3 для проигрывателя PCM: minimp3 (fw/vendor/minimp3, CC0).
 *
 * Файл или ячейка читается кусками по 16 КБ, кадр за кадром; памяти -- около
 * 23 КБ на дорожку. Частота и число каналов -- по первому кадру; ID3v2 в
 * начале пропускается. MPEG-1/2 layer 1 и 2 не нужны -- MINIMP3_ONLY_MP3.
 * Заголовок LAME (Xing/Info) даёт петлю без зазора: задержку кодера в начале
 * и хвост в конце не играем. */
#include "pcm_src.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "vendor/minimp3/minimp3.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define CHUNK 16384
#define LIVE_MIN 4096                   /* дописываемая ячейка: меньше -- ждать */
#define DEC_DELAY 529                   /* задержка minimp3 сверх кодера (замер 13.11.2026) */

typedef struct {
    pcm_io_t  io;
    long      data0;                    /* после ID3v2 и кадра Xing/Info */
    mp3dec_t  dec;
    uint8_t   buf[CHUNK];
    int       len, eof, wait;
    /* петля без зазора: заголовок LAME говорит, сколько отсчётов в начале --
     * задержка кодера, и сколько всего настоящих; без него -- 0 и 0 */
    long      skip0, total;
    long      skip, done;               /* осталось пропустить; выдано */
    int16_t   pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];   /* кадр, как его дал декодер */
} mp3_t;

static void refill(mp3_t *m)
{
    long r;
    m->wait = 0;
    if (m->eof || m->len >= CHUNK) return;
    r = pcm_io_read(&m->io, m->buf + m->len, CHUNK - m->len);
    if (r == PCM_SRC_WAIT) { m->wait = 1; return; }
    if (r <= 0) { m->eof = 1; return; }
    m->len += (int)r;
}

/* Один кадр в dst (до MINIMP3_MAX_SAMPLES_PER_FRAME отсчётов); кадров на
 * канал, 0 -- конец, PCM_SRC_WAIT -- ячейку ещё дописывают. info -- его
 * частота и каналы. */
static int frame(mp3_t *m, int16_t *dst, mp3dec_frame_info_t *info)
{
    for (;;) {
        int n;
        refill(m);
        /* Пока ячейку дописывают, хвост буфера -- может быть, полкадра:
         * декодеру его не давать, пока байт мало (он ищет синхронизацию
         * дальше и выкинул бы начало кадра). */
        if (m->wait && m->len < LIVE_MIN) return PCM_SRC_WAIT;
        if (m->len == 0) return 0;
        n = mp3dec_decode_frame(&m->dec, m->buf, m->len, dst, info);
        if (info->frame_bytes > 0) {
            m->len -= info->frame_bytes;
            memmove(m->buf, m->buf + info->frame_bytes, (size_t)m->len);
        }
        if (n > 0) return n;
        if (info->frame_bytes == 0) {
            /* синхронизации в буфере нет: хвост без кадра -- конец */
            if (m->wait) return PCM_SRC_WAIT;
            if (m->eof) return 0;
            if (m->len >= CHUNK) m->len = 0;        /* мусор во весь буфер */
        }
    }
}

/* Кадр MP3 -- до 1152 кадров на канал: n меньше -- не влезет. */
static int mp3_read(pcm_src_t *s, int16_t *dst, int n)
{
    mp3_t *m = s->st;
    mp3dec_frame_info_t info;
    int k, i, from;
    if (n < MINIMP3_MAX_SAMPLES_PER_FRAME / 2) return -1;
    for (;;) {
        if (m->total && m->done >= m->total) return 0;
        k = frame(m, m->pcm, &info);
        if (k <= 0) return k;
        from = 0;
        if (m->skip) {                              /* задержка кодера */
            from = m->skip < k ? (int)m->skip : k;
            m->skip -= from;
        }
        if (m->total && m->done + (k - from) > m->total) k = from + (int)(m->total - m->done);
        if (k > from) break;
    }
    k -= from;
    m->done += k;
    if (info.channels == s->ch) memcpy(dst, m->pcm + from * s->ch, (size_t)k * s->ch * sizeof *dst);
    else if (s->ch == 2)                            /* моно-кадр в стерео */
        for (i = 0; i < k; i++) dst[2 * i] = dst[2 * i + 1] = m->pcm[from + i];
    else
        for (i = 0; i < k; i++) dst[i] = (int16_t)((m->pcm[2 * (from + i)] + m->pcm[2 * (from + i) + 1]) / 2);
    return k;
}

static int mp3_rewind(pcm_src_t *s)
{
    mp3_t *m = s->st;
    m->len = 0; m->eof = 0; m->wait = 0;
    m->skip = m->skip0; m->done = 0;
    mp3dec_init(&m->dec);
    return pcm_io_seek(&m->io, m->data0);
}

static void mp3_close(pcm_src_t *s)
{
    mp3_t *m = s->st;
    if (!m) return;
    if (m->io.f) fclose(m->io.f);
    free(m);
    s->st = 0;
}

static long be32(const uint8_t *p) { return (long)((uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]); }

/* Первый кадр -- Xing/Info с заголовком LAME? Тогда он не звук: data0 за ним,
 * а задержка кодера и хвост дают петлю без зазора. Заголовок пишется только
 * если влезает в кадр: у CBR 24-32 кбит/с на 22 кГц не влезает, у VBR
 * (lame -V) -- да. */
static void lame_tag(mp3_t *m, const mp3dec_frame_info_t *info, int spf)
{
    const uint8_t *f = m->buf + info->frame_offset, *p;
    int fb = info->frame_bytes - info->frame_offset, i;
    long frames = 0, flags, delay, pad;
    for (i = 4; i + 8 <= fb && i < 48; i++)
        if (!memcmp(f + i, "Xing", 4) || !memcmp(f + i, "Info", 4)) break;
    if (i + 8 > fb || i >= 48) return;
    p = f + i + 4;
    flags = be32(p); p += 4;
    if (flags & 1) { frames = be32(p); p += 4; }
    if (flags & 2) p += 4;
    if (flags & 4) p += 100;
    if (flags & 8) p += 4;
    m->data0 += info->frame_offset + info->frame_bytes;    /* кадр с тегом -- не звук */
    if (!frames || p + 24 > f + fb) return;
    delay = p[21] << 4 | p[22] >> 4;
    pad = (p[22] & 15) << 8 | p[23];
    if (frames * spf <= delay + pad) return;
    m->skip0 = delay + DEC_DELAY;
    m->total = frames * spf - delay - pad;
}

int pcm_mp3_open(const pcm_io_t *io, pcm_src_t *s)
{
    mp3dec_frame_info_t info;
    uint8_t h[10];
    int n;
    mp3_t *m = calloc(1, sizeof *m);
    if (!m) return -1;
    m->io = *io;
    pcm_io_seek(&m->io, 0);
    if (pcm_io_read(&m->io, h, 10) == 10 && !memcmp(h, "ID3", 3))
        m->data0 = 10 + ((long)(h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F))
                 + ((h[5] & 0x10) ? 10 : 0);
    s->st = m;
    mp3_rewind(s);
    /* первый кадр -- прямо из буфера: нужны его байты, а frame() их сдвигает */
    refill(m);
    if (m->wait && m->len < LIVE_MIN) goto fail;
    n = mp3dec_decode_frame(&m->dec, m->buf, m->len, m->pcm, &info);
    if (n <= 0 || info.hz <= 0 || (info.channels != 1 && info.channels != 2)) goto fail;
    lame_tag(m, &info, n);
    s->rate = info.hz; s->ch = info.channels;
    s->read = mp3_read; s->rewind = mp3_rewind; s->close = mp3_close;
    mp3_rewind(s);
    return 0;
fail:
    free(m); s->st = 0;
    return -1;
}
