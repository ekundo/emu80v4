/* Источник MOD для проигрывателя PCM: micromod (fw/vendor/micromod, BSD-3).
 *
 * ProTracker MOD и многоканальные (6CHN, 8CHN, ..., до 32 каналов). Модуль
 * целиком в памяти (на ESP -- в PSRAM, malloc больше 16 КБ идёт туда), не
 * больше MOD_MAX; из ячейки проигрывателя -- прямо из её памяти. Раскладывает сразу на частоте ПЛИС (PCM_RATE), стерео, так
 * что пересчёта нет. micromod -- один на всю программу: дорожка одна. */
#include "pcm_src.h"
#include "vendor/micromod/micromod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD_MAX (1024L * 1024)

typedef struct {
    signed char *data;
    int          own;                   /* data -- наша (из файла), а не ячейки */
    long         len, pos;              /* длина песни и позиция, кадры */
} mod_t;

static int mod_read(pcm_src_t *s, int16_t *dst, int n)
{
    mod_t *m = s->st;
    long k = m->len - m->pos;
    if (k <= 0) return 0;
    if (k > n) k = n;
    memset(dst, 0, (size_t)k * 2 * sizeof *dst);
    micromod_get_audio(dst, k);
    m->pos += k;
    return (int)k;
}

static int mod_rewind(pcm_src_t *s)
{
    mod_t *m = s->st;
    micromod_set_position(0);
    m->pos = 0;
    return 0;
}

static void mod_close(pcm_src_t *s)
{
    mod_t *m = s->st;
    if (!m) return;
    if (m->own) free(m->data);
    free(m);
    s->st = 0;
}

static int mod_start(mod_t *m, pcm_src_t *s)
{
    if (micromod_initialise(m->data, PCM_RATE) < 0) return -1;
    /* 4 канала -- 64 без искажений, больше -- тише (micromod.h) */
    micromod_set_gain(micromod_mute_channel(-1) <= 4 ? 64 : 32);
    m->len = micromod_calculate_song_duration();
    micromod_set_position(0);
    s->rate = PCM_RATE; s->ch = 2;
    s->read = mod_read; s->rewind = mod_rewind; s->close = mod_close;
    s->st = m;
    return 0;
}

int pcm_mod_open(FILE *f, pcm_src_t *s)
{
    signed char hdr[1084];
    long len;
    mod_t *m;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) return -1;
    len = micromod_calculate_mod_file_len(hdr);
    if (len < (long)sizeof hdr || len > MOD_MAX) return -1;
    m = calloc(1, sizeof *m);
    if (!m || !(m->data = malloc((size_t)len))) { free(m); return -1; }
    memcpy(m->data, hdr, sizeof hdr);
    /* короткий файл (обрезанные сэмплы) -- дополнить тишиной */
    memset(m->data + sizeof hdr, 0, (size_t)(len - (long)sizeof hdr));
    if (fread(m->data + sizeof hdr, 1, (size_t)(len - (long)sizeof hdr), f) == 0 && len > (long)sizeof hdr) {
        free(m->data); free(m); return -1;
    }
    m->own = 1;
    if (mod_start(m, s) < 0) { free(m->data); free(m); return -1; }
    fclose(f);                          /* модуль в памяти -- файл больше не нужен */
    return 0;
}

/* Модуль, который уже лежит в памяти (ячейка проигрывателя): без копии, но
 * только целый -- micromod читает сэмплы до конца по заголовку. */
int pcm_mod_open_mem(const uint8_t *data, long len, pcm_src_t *s)
{
    mod_t *m;
    long need;
    if (len < 1084) return -1;
    need = micromod_calculate_mod_file_len((signed char *)data);
    if (need < 1084 || need > len) return -1;
    m = calloc(1, sizeof *m);
    if (!m) return -1;
    m->data = (signed char *)data;
    if (mod_start(m, s) < 0) { free(m); return -1; }
    return 0;
}
