/* Источник MP3 для проигрывателя PCM: minimp3 (fw/vendor/minimp3, CC0).
 *
 * Файл читается кусками по 16 КБ, кадр за кадром; памяти -- около 23 КБ на
 * дорожку. Частота и число каналов -- по первому кадру; ID3v2 в начале
 * пропускается. MPEG-1/2 layer 1 и 2 не нужны -- MINIMP3_ONLY_MP3. */
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

typedef struct {
    FILE     *f;
    long      data0;                    /* после ID3v2 */
    mp3dec_t  dec;
    uint8_t   buf[CHUNK];
    int       len, eof;
    int16_t   pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];   /* кадр, как его дал декодер */
} mp3_t;

static void refill(mp3_t *m)
{
    size_t r;
    if (m->eof || m->len >= CHUNK) return;
    r = fread(m->buf + m->len, 1, (size_t)(CHUNK - m->len), m->f);
    if (r == 0) m->eof = 1;
    m->len += (int)r;
}

/* Один кадр в dst (до MINIMP3_MAX_SAMPLES_PER_FRAME отсчётов); кадров на
 * канал, 0 -- конец. info -- его частота и каналы. */
static int frame(mp3_t *m, int16_t *dst, mp3dec_frame_info_t *info)
{
    for (;;) {
        int n;
        refill(m);
        if (m->len == 0) return 0;
        n = mp3dec_decode_frame(&m->dec, m->buf, m->len, dst, info);
        if (info->frame_bytes > 0) {
            m->len -= info->frame_bytes;
            memmove(m->buf, m->buf + info->frame_bytes, (size_t)m->len);
        }
        if (n > 0) return n;
        if (info->frame_bytes == 0) {
            /* синхронизации в буфере нет: хвост без кадра -- конец */
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
    int k, i;
    if (n < MINIMP3_MAX_SAMPLES_PER_FRAME / 2) return -1;
    k = frame(m, m->pcm, &info);
    if (k <= 0) return 0;
    if (info.channels == s->ch) memcpy(dst, m->pcm, (size_t)k * s->ch * sizeof *dst);
    else if (s->ch == 2)                            /* моно-кадр в стерео */
        for (i = 0; i < k; i++) dst[2 * i] = dst[2 * i + 1] = m->pcm[i];
    else
        for (i = 0; i < k; i++) dst[i] = (int16_t)((m->pcm[2 * i] + m->pcm[2 * i + 1]) / 2);
    return k;
}

static int mp3_rewind(pcm_src_t *s)
{
    mp3_t *m = s->st;
    m->len = 0; m->eof = 0;
    mp3dec_init(&m->dec);
    return fseek(m->f, m->data0, SEEK_SET);
}

static void mp3_close(pcm_src_t *s)
{
    mp3_t *m = s->st;
    if (!m) return;
    fclose(m->f);
    free(m);
    s->st = 0;
}

int pcm_mp3_open(FILE *f, pcm_src_t *s)
{
    mp3dec_frame_info_t info;
    uint8_t h[10];
    mp3_t *m = calloc(1, sizeof *m);
    if (!m) return -1;
    m->f = f;
    if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3))
        m->data0 = 10 + ((long)(h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F))
                 + ((h[5] & 0x10) ? 10 : 0);
    s->st = m;
    mp3_rewind(s);
    if (frame(m, m->pcm, &info) <= 0 || info.hz <= 0 || (info.channels != 1 && info.channels != 2)) {
        free(m); s->st = 0;
        return -1;
    }
    s->rate = info.hz; s->ch = info.channels;
    s->read = mp3_read; s->rewind = mp3_rewind; s->close = mp3_close;
    mp3_rewind(s);
    return 0;
}
