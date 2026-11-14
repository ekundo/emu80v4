/* Источники проигрывателя PCM: файл -> отсчёты (pcm_src.c). */
#ifndef COMBO_PCM_SRC_H
#define COMBO_PCM_SRC_H

#include "pcm_play.h"

#include <stdio.h>

/* Откуда источник берёт байты: файл на карте, ячейка в памяти или ячейка-файл.
 * Ячейку игра дописывает, пока она играет (pcm_play.h, 0Ah), поэтому здесь не
 * копия указателя и длины, а ссылки на них: realloc меняет *d, каждая
 * дозапись -- *have.
 *   f, have = 0   файл дорожки: источник им владеет и закроет
 *   f = 0         ячейка в памяти: *d, *have
 *   f, have       ячейка-файл (rev4 без PSRAM): файлом владеет pcm_play.c, он
 *                 же в него дописывает; читать -- с fseek, до *have */
typedef struct {
    FILE           *f;                  /* файл, или 0 -- память */
    uint8_t *const *d;                  /* начало ячейки в памяти */
    const long     *have;               /* сколько уже лежит */
    long            size;               /* сколько будет всего; 0 -- сколько лежит */
    long            pos;
} pcm_io_t;

/* Прочитать до n байт: сколько прочитано, 0 -- конец, PCM_SRC_WAIT -- в ячейке
 * пока нет, но будет. */
long pcm_io_read(pcm_io_t *io, void *dst, long n);
int  pcm_io_seek(pcm_io_t *io, long pos);
/* Всё ли уже лежит (файл -- всегда). */
int  pcm_io_whole(const pcm_io_t *io);
/* Закрыть: файл дорожки -- fclose, ячейку не трогает. */
void pcm_io_close(pcm_io_t *io);

/* 1 -- расширение из тех, что проигрыватель умеет. */
int pcm_src_known(const char *name);
/* Открыть файл; 0 -- открыт, src готов. */
int pcm_src_open(const char *path, pcm_src_t *src);
/* Из ячейки, формат -- по содержимому: WAV, MOD, MP3. *d и *have живут, пока
 * источник открыт; size -- сколько будет всего (0 -- столько, сколько есть).
 * WAV и MP3 играют, пока ячейку дописывают; MOD -- только целую. */
int pcm_src_open_mem(uint8_t *const *d, const long *have, long size, pcm_src_t *src);
/* Из ячейки-файла: то же для WAV и MP3; MOD -- нет (micromod держит модуль в
 * памяти, а ячейка-файл для того и есть, что в память не помещается). */
int pcm_src_open_slot_file(FILE *f, const long *have, long size, pcm_src_t *src);

/* По формату: io открыт и стоит в начале. 0 -- источник готов и владеет io
 * (файл закроет он); иначе io не тронут, закрывает вызвавший. */
int pcm_mp3_open(const pcm_io_t *io, pcm_src_t *src);   /* pcm_mp3.c */
int pcm_mod_open(FILE *f, pcm_src_t *src);              /* pcm_mod.c */
int pcm_mod_open_mem(const uint8_t *data, long len, pcm_src_t *src);

#endif
