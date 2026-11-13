/* Проигрыватель PCM платы ComboLite rev5 в эмуляторе «Вектора».
 *
 * Код ESP -- тот же, что на плате (pcm_play.c, pcm_src.c, pcm_mp3.c,
 * pcm_mod.c, копией из fw/ репозитория платы через sync.sh). Здесь -- то,
 * что на плате делает ПЛИС (hdl/pcm.v): почтовый порт 3Ch, кольцо отсчётов
 * 256 КБ и указатель чтения на 44 092 Гц, -- и то, что делает задача ESP
 * (fw/hdd_esp.c): опрос раз в 1, 5 или 20 мс, дорожки из папки music.
 *
 * Протокол порта -- в pcm_play.h. Файл общий для Emu80 и v06x: правится в
 * одном, копируется в другой как есть.
 */
#ifndef PCM_HOST_H
#define PCM_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCM_HOST_PORT 0x3C
#define PCM_HOST_RATE 44092                     /* отсчётов в секунду, как PCM_RATE */

/* ОЗУ «Вектора» (то, что на плате лежит в зеркале): n байт с адреса a в dst;
 * 0 -- прочитано. */
typedef int (*pcm_host_mem_fn)(void *ctx, uint16_t a, uint8_t *dst, int n);

/* Включить проигрыватель. music_dir -- папка с дорожками (как music на карте
 * платы: дорожка N -- N-й по алфавиту файл WAV, MP3 или MOD), NULL -- дорожек
 * нет, работают только ячейки (длинные команды). */
void    pcm_host_init(const char *music_dir, pcm_host_mem_fn mem, void *ctx);
void    pcm_host_reset(void);                   /* сброс машины: стоп, почта пуста */

uint8_t pcm_host_in(void);                      /* чтение порта 3Ch */
void    pcm_host_out(uint8_t v);                /* запись в порт 3Ch */

/* Один отсчёт выхода: 1/44092 с времени «Вектора». Выход ПЛИС -- беззнаковый,
 * тишина 80h, после плавного выхода -- 0; блок кольца -- байт со сдвигом на
 * блок (pcm_play.h), ПЛИС делит на 2^s во времени, здесь -- сразу; на плате
 * постоянную составляющую снимает разделительный конденсатор; здесь -- фильтр
 * первого порядка (срез ~3 Гц). Знаковые, полная шкала -- +-32767. */
void    pcm_host_step(int16_t *l, int16_t *r);

#ifdef __cplusplus
}
#endif

#endif
