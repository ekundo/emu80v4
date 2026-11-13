/* Проигрыватель PCM платы ComboLite rev5 в эмуляторе (pcm_host.h). */
#include "pcm_host.h"
#include "pcm_play.h"
#include "pcm_src.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---------------------------------------------------- ПЛИС (hdl/pcm.v) */
static uint8_t  ring[PCM_NBLK * PCM_BLK];       /* кольцо в DD6B, страницы 4..7 */
static uint32_t rp;                             /* указатель чтения, байты */
static uint16_t lim;                            /* первый незаписанный блок */
static uint8_t  ctl;                            /* от ESP: разряд 0 -- играть, 7..1 -- «Вектору» */
static uint8_t  mb, full;                       /* почта «Вектора» */
static uint8_t  out_l = 0x80, out_r = 0x80;     /* выход держит последний отсчёт; с 80h -- без щелчка при включении */
static int      on;

static char            music[1024];
static pcm_host_mem_fn mem_fn;
static void           *mem_ctx;
static long            until_tick;              /* отсчётов до следующего опроса ESP */
static long            period = PCM_HOST_RATE / 50;

uint8_t pcm_host_in(void)
{
    if (!on) return 0xFF;
    return (uint8_t)((ctl & 0xFE) | full);
}

void pcm_host_out(uint8_t v)
{
    if (!on) return;
    mb = v; full = 1;
}

static int x_xchg(void *c, uint8_t newctl, uint16_t l, uint16_t *r, int *mail)
{
    (void)c;
    *r = (uint16_t)(rp / PCM_BLK);
    *mail = full ? mb : -1;
    full = 0;
    ctl = newctl; lim = l & (PCM_NBLK - 1);
    if (!(ctl & 1)) rp = 0;
    return 0;
}

static int x_ring(void *c, int b, const uint8_t *d)
{
    (void)c;
    memcpy(ring + (size_t)(b & (PCM_NBLK - 1)) * PCM_BLK, d, PCM_BLK);
    return 0;
}

/* ------------------------------------------ задача ESP (fw/hdd_esp.c) */
static int cmp_ci(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* Дорожка n -- n-й по алфавиту файл папки из тех, что умеем. */
static int x_open(void *c, int n, pcm_src_t *src)
{
    char *names[128], path[2048];
    struct dirent *e;
    struct stat sb;
    int count = 0, k, rc = -1;
    DIR *d;
    (void)c;
    if (!music[0] || !(d = opendir(music))) return -1;
    while ((e = readdir(d)) && count < 128) {
        snprintf(path, sizeof path, "%s/%s", music, e->d_name);
        if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode) && pcm_src_known(e->d_name)
            && (names[count] = strdup(e->d_name)))
            count++;
    }
    closedir(d);
    qsort(names, (size_t)count, sizeof *names, cmp_ci);
    if (n >= 1 && n <= count) {
        snprintf(path, sizeof path, "%s/%s", music, names[n - 1]);
        rc = pcm_src_open(path, src);
    }
    for (k = 0; k < count; k++) free(names[k]);
    return rc;
}

static int x_open_mem(void *c, uint8_t *const *d, const long *have, long size, pcm_src_t *src)
{
    (void)c;
    return pcm_src_open_mem(d, have, size, src);
}

static int x_mem_read(void *c, uint16_t a, uint8_t *dst, int n)
{
    (void)c;
    if (!mem_fn || (long)a + n > 0x10000L) return -1;
    return mem_fn(mem_ctx, a, dst, n);
}

void pcm_host_init(const char *music_dir, pcm_host_mem_fn m, void *c)
{
    static const pcm_ops_t ops = { x_xchg, x_ring, x_open, x_open_mem, x_mem_read, 0 };
    snprintf(music, sizeof music, "%s", music_dir ? music_dir : "");
    mem_fn = m; mem_ctx = c;
    on = 1;
    pcm_host_reset();
    pcm_init(&ops);
}

void pcm_host_reset(void)
{
    if (!on) return;
    if (pcm_state() & PCM_ST_PLAY) pcm_command(0x00);
    mb = full = 0;
    until_tick = 0;
}

/* разделительный конденсатор: y = x - x_прошлый + a*y_прошлый, a = 1 - 2^-11 */
static int32_t hp_x[2], hp_y[2];
static int16_t dc_block(int k, int x)
{
    int32_t y = x - hp_x[k] + hp_y[k] - (hp_y[k] >> 11);
    hp_x[k] = x; hp_y[k] = y;
    return (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
}

void pcm_host_step(int16_t *l, int16_t *r)
{
    if (!on) { *l = *r = 0; return; }
    if (--until_tick <= 0) {
        /* как pcm_task: идёт почта или режим игры -- раз в 1 мс, играет -- 5 мс,
         * тишина -- 20 мс */
        int s = pcm_tick(8);
        period = s == 2 ? PCM_HOST_RATE / 1000 : s == 1 ? PCM_HOST_RATE / 200 : PCM_HOST_RATE / 50;
        until_tick = period;
    }
    /* ПЛИС: играть, пока указатель не дошёл до границы; иначе держать последний */
    if ((ctl & 1) && rp / PCM_BLK != lim) {
        out_l = ring[rp]; out_r = ring[rp + 1];
        rp = (rp + 2) % sizeof ring;
    }
    *l = dc_block(0, (out_l - 128) * 256);
    *r = dc_block(1, (out_r - 128) * 256);
}
