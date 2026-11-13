/* Источники проигрывателя PCM: файл -> отсчёты (pcm_src.c). */
#ifndef COMBO_PCM_SRC_H
#define COMBO_PCM_SRC_H

#include "pcm_play.h"

#include <stdio.h>

/* 1 -- расширение из тех, что проигрыватель умеет. */
int pcm_src_known(const char *name);
/* Открыть файл; 0 -- открыт, src готов. */
int pcm_src_open(const char *path, pcm_src_t *src);
/* Из памяти (ячейка проигрывателя), формат -- по содержимому: WAV, MOD, MP3.
 * Память живёт, пока источник открыт. */
int pcm_src_open_mem(const uint8_t *data, long len, pcm_src_t *src);

/* По формату: файл открыт и стоит в начале. 0 -- источник готов и владеет
 * файлом; иначе файл не тронут, закрывает вызвавший. */
int pcm_mp3_open(FILE *f, pcm_src_t *src);         /* pcm_mp3.c */
int pcm_mod_open(FILE *f, pcm_src_t *src);         /* pcm_mod.c */
int pcm_mod_open_mem(const uint8_t *data, long len, pcm_src_t *src);

#endif
