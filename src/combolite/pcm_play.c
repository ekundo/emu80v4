/* Проигрыватель PCM rev5 -- без платы (pcm_play.h). */
#include "pcm_play.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#define NOW_US() esp_timer_get_time()
#else
#define NOW_US() 0LL
#endif
static long long z_t80 = -1, z_end = -1, z_mail, z_exe, z_fl, z_gap;
static unsigned long z_n, z_bytes;
/* Срабатывание сторожа -- в журнал (14.11.2026, владелец: срок 0,5 с,
 * «ловишь, где таймаут, порт чинит»): последние байты почты с временем,
 * последняя длинная команда, что звучало. */
#define WD_LOG 12
static uint8_t  wl_b[WD_LOG];
static long long wl_t[WD_LOG];
static int      wl_i;
static uint8_t  wl_op = 0xFF;
static long long wl_op_t;
static char     wd_txt[400];
static int      wd_new;
static long long wd_prev_us, wd_acc_us;    /* сторож: время, пока звучит */

#include <stdlib.h>
#include <string.h>

#define RAMP  512                       /* кадров плавного входа и выхода, 11,6 мс */
#define INBUF 2304                      /* отсчётов источника за раз (кадр MP3 стерео) */
#define FRAMES PCM_FRAMES               /* кадров в блоке кольца (первый -- сдвиги) */
#define LOW   (-32768)                  /* низ шкалы: выход «в ноль» */
/* Покой выхода PCM. Прежний смеситель (rev4-fb-70 и раньше, rev5-39) считал
 * отсчёт без знака: покой -- низ шкалы, и вход в работу и выход из неё --
 * плавные 11,6 мс от низа к середине. На плате №1 с весом x4 это щелчок при
 * включении режима игры (14.11.2026). Смеситель mixer.v (rev4-fb-71) берёт
 * отсчёт со знаком вокруг 80h, и pcm.v после включения стоит на 80h: покой --
 * середина, перепада нет (PCM_REST_MID, CMakeLists.txt). */
#ifdef PCM_REST_MID
#define REST  0
#else
#define REST  LOW
#endif
#ifndef PCM_MIR
#define PCM_MIR (PCM_GAME_AHEAD + 8)
#endif
#define MIR   PCM_MIR                   /* блоков музыки без эффекта: больше PCM_GAME_AHEAD */
#define FX_EDGE 32                      /* кадров плавного края эффекта, 0,7 мс */

enum { S_IDLE, S_START, S_PLAY, S_DRAIN };

static pcm_ops_t ops;
static int       st = S_IDLE, run, loop, err, ready;
static int       wblk;                  /* следующий блок кольца, он же граница */
static long      wabs;                  /* он же от запуска: wabs % PCM_NBLK == wblk */
static uint16_t  rp;                    /* блок, который играет ПЛИС */
static int       vol = 16, fx_vol = 16;

static pcm_src_t src;
static int       src_on;                /* источник открыт и не кончился */
/* inb и mir -- из кучи и только пока проигрыватель занят (играет или режим
 * игры; bufs_get / bufs_put): на rev4 (без PSRAM) 12,6 КБ внутренней памяти
 * в простое нужны Wi-Fi и веб-серверу (14.11.2026: плата падала без памяти).
 * Прежде: из кучи при pcm_init, не статикой: на rev4 (без PSRAM) 12 КБ
 * внутренней памяти занимали бы и там, где проигрывателя нет (fw/pcm4.c) */
static int16_t  *inb;
static int       in_n, in_i;            /* кадров в inb, следующий кадр */
static uint32_t  frac, step;            /* позиция между кадрами a и b, 16.16 */
static int       a_l, a_r, b_l, b_r;
static int       ramp_in, ramp_out;     /* кадров осталось */
static int       last_l, last_r;        /* последний выданный отсчёт, 16 бит со знаком */
static int       done;                  /* всё выдано, дальше только тишина */
static unsigned long under;             /* недоборов */
static int       starved;               /* ПЛИС стоит на границе (считан один раз) */

/* Ячейки, которые игра загрузила сама (длинные команды, pcm_play.h). */
/* d -- в куче, f -- ячейка-файл, xm -- в статике платы (xoff, xcap; x0 -- её
 * первый байт: на стыке повтора эффекта нужен вместе с последним) */
static struct { uint8_t *d; long len, cap, size; FILE *f; int xm; long xoff, xcap; uint8_t x0; long old; } slot[PCM_SLOTS];
static int slot_has(int k) { return slot[k].d || slot[k].xm; }
static void stage_drop(int k);
static int       playing_slot;          /* чья память под источником, 0 -- ничья */
static int       seq;                   /* разряд PCM_ST_SEQ */
static int       ext_n;                 /* байт адреса блока ещё ждём (длинная команда) */
static uint16_t  ext_a;
static int       hurry;                 /* шагов с частым опросом: идёт длинная команда */

/* ------------------------------------------------------ режим игры
 * Второй голос -- эффект поверх музыки. Кольцо в режиме игры держит впереди
 * PCM_GAME_AHEAD блоков, а музыка без эффекта для них лежит здесь (mir): по
 * OUT эффект подмешивается в блоки с rp + PCM_FX_LEAD, они переписываются, и
 * дальше эффект идёт вместе с новыми. Голос -- функция номера кадра от
 * запуска (fx_at), так что переписать блок можно сколько угодно раз. Новый
 * эффект обрывает прежний: тот гаснет за FX_EDGE кадров (old). */
static int       gm;                    /* режим игры */
static struct { uint8_t k, reps; uint16_t rate; } fx_tab[PCM_FX_MAX];
static int       fx_n;
typedef struct {
    int       k;                        /* ячейка; 0 -- голоса нет */
    int       reps;
    uint32_t  step;                     /* 16.16: отсчётов ячейки на кадр выхода */
    long long start;                    /* кадр выхода, с которого звучит */
    long long cut;                      /* с какого кадра гаснет; -1 -- не гаснет */
} voice_t;
static voice_t   fx, fx_old;

/* Сторож режима игры (13.11.2026): игра пинает проигрыватель (любая почта, FFh
 * -- пинок без дела). Нет пинков дольше wd_t десятых секунды -- игра
 * повисла или «Вектор» сброшен: музыка и эффект гаснут, режим игры
 * выключается. Время -- по выходу ПЛИС (play_frame): в режиме игры он идёт
 * всегда. После срабатывания короткая почта не значит ничего (иначе эффект N
 * от ожившей игры пустил бы дорожку N с карты), пока не придёт 07h. FFh
 * вне режима игры -- тоже ничего: загрузчик пинает и до 07h. */
/* Срок сторожа по умолчанию, десятые секунды. rev4: 5 (CMakeLists.txt) --
 * владелец 14.11.2026: «3 секунды не устраивают, не больше 1, лучше
 * половину». Сброс машины гасит музыку и без сторожа (pcm_machine_reset). */
#ifndef PCM_WD_DEFAULT
#define PCM_WD_DEFAULT 20
#endif
#define WD_DEFAULT PCM_WD_DEFAULT
static int       wd_t = WD_DEFAULT, wd_dead;
static int       wd_kick;               /* пинок пришёл, отметку ещё не взяли */
static long long wd_last;
static unsigned long wd_fired;
static uint8_t (*mir)[PCM_BLK];
static long      mir_abs[MIR];

void pcm_init(const pcm_ops_t *o)
{
    int k;
    /* открытый источник -- закрыть (стенд зовёт pcm_init много раз; MP3 на
     * rev4 -- одно состояние на всех, PCM_MP3_STATIC) */
    if (src_on && src.close) src.close(&src);
    src_on = 0;
    ops = *o;
    st = S_IDLE; run = 0; err = 0; ready = 1; wblk = 0; wabs = 0; rp = 0;
    src_on = 0; done = 1; last_l = last_r = REST;
    ext_n = 0; playing_slot = 0;
    gm = 0; fx_n = 0; fx.k = fx_old.k = 0; fx_vol = 16;
    wd_t = WD_DEFAULT; wd_dead = 0; wd_kick = 0; wd_last = 0; wd_fired = 0; wd_prev_us = 0; wd_acc_us = 0;
    under = 0; starved = 0;
    for (k = 0; k < MIR; k++) mir_abs[k] = -1;
}

/* Буферы источника и музыки режима игры: 0 -- есть. */
static int bufs_get(void)
{
    int k;
    if (inb && mir) return 0;
    if (!inb) inb = malloc(INBUF * sizeof *inb);
    if (!mir) {
        mir = malloc(MIR * sizeof *mir);
        for (k = 0; k < MIR; k++) mir_abs[k] = -1;
    }
    return inb && mir ? 0 : -1;
}

static void bufs_put(void)
{
    free(inb); inb = 0;
    free(mir); mir = 0;
}

/* кадр выхода, который ПЛИС играет сейчас (с точностью до блока) */
static long long play_frame(void)
{
    long rabs = wabs - ((wblk - rp) & (PCM_NBLK - 1));
    return (long long)rabs * FRAMES;
}

/* последний кадр голоса (не включая) */
static long long fx_end(const voice_t *v)
{
    long long n;
    if (!v->k || !slot_has(v->k) || !v->step) return v->start;
    n = ((long long)slot[v->k].len * v->reps << 16) / v->step;
    if (v->cut >= 0 && v->cut + FX_EDGE < v->start + n) return v->cut + FX_EDGE;
    return v->start + n;
}

uint8_t pcm_state(void)
{
    int playing = gm ? (src_on || ramp_out) : st != S_IDLE;
    int sounding = gm && fx.k && fx_end(&fx) > play_frame();
    return (uint8_t)((ready ? PCM_ST_READY : 0) | (err ? PCM_ST_ERR : 0)
                     | (playing ? PCM_ST_PLAY : 0) | (seq ? PCM_ST_SEQ : 0)
                     | (sounding ? PCM_ST_FX : 0));
}

unsigned long pcm_underruns(void) { return under; }
unsigned long pcm_watchdog_fired(void) { return wd_fired; }
static unsigned long xc_miss, rp_bad;
unsigned long pcm_xmem_misses(void) { return xc_miss; }
unsigned long pcm_rp_bad(void) { return rp_bad; }

/* Отладка заикания режима игры (14.11.2026): почта по видам, перезаписи
 * блоков и опоздавшие из них -- ПЛИС вошла в блок rw_first раньше, чем ESP
 * его переписала. */
static unsigned long d_kick, d_fx, d_stop, d_ext, d_other, d_rw, d_rw_late, d_ahead = 999, d_kgap, d_resets;
static long rw_first = -1;
/* v[7] -- наименьший запас кольца (блоков) на обмене с ПЛИС, пока играет
 * музыка; с прошлого вызова. 1 -- ПЛИС на последнем записанном блоке: на
 * границе она держит отсчёт, а rp остаётся на этом блоке, поэтому rp == wblk
 * (недобор) так не увидеть. */
/* v[8] -- самый длинный промежуток между пинками в режиме игры с прошлого
 * вызова, мс (по выходу ПЛИС, с точностью до блока 5,8 мс); v[9] -- сбросов
 * машины, погасивших музыку. */
void pcm_debug2(unsigned long v[10])
{
    v[0] = d_kick; v[1] = d_fx; v[2] = d_stop; v[3] = d_ext; v[4] = d_other; v[5] = d_rw; v[6] = d_rw_late;
    v[7] = d_ahead; d_ahead = 999;
    v[8] = d_kgap; d_kgap = 0;
    v[9] = d_resets;
}
void pcm_debug(int *rp_, int *wblk_, int *st_, int *gm_, int *src_)
{
    *rp_ = rp; *wblk_ = wblk; *st_ = st; *gm_ = gm; *src_ = src_on;
}

void pcm_volume(int v) { vol = v < 0 ? 0 : v > 16 ? 16 : v; }

static void src_close(void)
{
    if (src_on && src.close) src.close(&src);
    src_on = 0;
    playing_slot = 0;
}

/* следующий кадр источника в b; 1 -- есть, 0 -- источник кончился,
 * PCM_SRC_WAIT -- ячейку ещё дописывают */
static int next_frame(void)
{
    if (in_i >= in_n) {
        int n = src.read(&src, inb, INBUF / 2);
        if (n == 0 && loop && src.rewind && src.rewind(&src) == 0)
            n = src.read(&src, inb, INBUF / 2);
        if (n == PCM_SRC_WAIT) return PCM_SRC_WAIT;
        if (n <= 0) return 0;
        in_n = n; in_i = 0;
    }
    a_l = b_l; a_r = b_r;
    if (src.ch == 2) { b_l = inb[2 * in_i]; b_r = inb[2 * in_i + 1]; }
    else             { b_l = b_r = inb[in_i]; }
    in_i++;
    return 1;
}

static int to_s16(int v)
{
    v = (v * vol) >> 4;
    return v > 32767 ? 32767 : v < LOW ? LOW : v;
}

/* Блок кольца из кадров (16 бит со знаком): кадр 0 -- сдвиги s левого и
 * правого, дальше FRAMES отсчётов q по 8 бит (80h -- середина), значение
 * отсчёта -- q * 2^(8-s). Сдвиг -- наибольший 0..PCM_SHIFT_MAX, при котором
 * пик канала в блоке влезает в 8 бит: тихий блок идёт с точностью до 12 бит,
 * громкий -- как раньше, 8 (13.11.2026: шум квантования на тихой музыке). */
static void encode(const int16_t *fr, uint8_t *blk)
{
    int c, k;
    for (c = 0; c < 2; c++) {
        int pk = 0, s = 0, sh;
        for (k = 0; k < FRAMES; k++) {
            int a = fr[2 * k + c];
            if (a < 0) a = -a;
            if (a > pk) pk = a;
        }
        while (s < PCM_SHIFT_MAX && (pk << (s + 1)) < 32768 - 256) s++;
        sh = 8 - s;
        blk[c] = (uint8_t)s;
        for (k = 0; k < FRAMES; k++) {
            int q = (fr[2 * k + c] + (1 << (sh - 1))) >> sh;
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            blk[2 + 2 * k + c] = (uint8_t)(q + 128);
        }
    }
}

static void decode(const uint8_t *blk, int16_t *fr)
{
    int c, k;
    for (c = 0; c < 2; c++)
        for (k = 0; k < FRAMES; k++)
            fr[2 * k + c] = (int16_t)((blk[2 + 2 * k + c] - 128) * (1 << (8 - (blk[c] & 7))));
}

/* Один кадр музыки. 0 -- выдавать нечего (done). В режиме игры без музыки --
 * середина (80h): выход не стоит, эффекту есть на чём звучать. */
static int out_frame(int *l, int *r)
{
    if (ramp_in) {
        *l = *r = last_l = last_r = (REST * ramp_in) / RAMP;
        ramp_in--;
        return 1;
    }
    if (src_on) {
        int vl = a_l + (int)(((int64_t)(b_l - a_l) * frac) >> 16);
        int vr = a_r + (int)(((int64_t)(b_r - a_r) * frac) >> 16);
        frac += step;
        while (frac >= 0x10000) {
            int k = next_frame();
            if (k == PCM_SRC_WAIT) { frac = 0x10000; break; }   /* ждём: держим b */
            frac -= 0x10000;
            if (!k) { src_close(); ramp_out = RAMP; break; }
        }
        *l = last_l = to_s16(vl); *r = last_r = to_s16(vr);
        return 1;
    }
    if (ramp_out) {
        /* к нулю, а в режиме игры -- к середине: выход живёт дальше */
        int t = gm ? 0 : REST;
        *l = t + (int)(((long)(last_l - t) * (ramp_out - 1)) / RAMP);
        *r = t + (int)(((long)(last_r - t) * (ramp_out - 1)) / RAMP);
        if (--ramp_out == 0) {
            if (gm) last_l = last_r = 0;
            else done = 1;
        }
        return 1;
    }
    if (gm) { *l = *r = 0; return 1; }
    return 0;
}

/* Блок музыки; 0 -- нечего класть. Хвост последнего блока -- низ шкалы. */
static int make_block(uint8_t *blk)
{
    static int16_t fr[2 * FRAMES];
    int k;
    if (done) return 0;
    for (k = 0; k < FRAMES; k++) {
        int l = REST, r = REST;
        if (done || !out_frame(&l, &r)) l = r = REST;
        fr[2 * k] = (int16_t)l; fr[2 * k + 1] = (int16_t)r;
    }
    encode(fr, blk);
    return 1;
}

/* ---------------------------------------------------------- эффект */
/* Отсчёт голоса на кадре f (от запуска плеера), 16 бит; 0 -- молчит. */
/* Окна ячеек в статике: 2 блока с блока, где лежит j, -- j + 1 тоже в окне.
 * Два окна: голосов два (fx, fx_old), замещается то, что брали раньше. */
static struct { int k; long base; uint8_t d[2 * PCM_BLK]; } xc[2];
static int xc_last;

static void xc_drop(int k)
{
    if (xc[0].k == k) xc[0].k = 0;
    if (xc[1].k == k) xc[1].k = 0;
}

/* Отсчёт j ячейки k (8 бит со знаком) */
static int sample(int k, long j)
{
    long base;
    int e, n;
    if (slot[k].d) return ((const int8_t *)slot[k].d)[j];
    if (!j) return (int8_t)slot[k].x0;
    base = j & ~(long)(PCM_BLK - 1);
    for (e = 0; e < 2; e++)
        if (xc[e].k == k && xc[e].base == base) { xc_last = e; return (int8_t)xc[e].d[j - base]; }
    /* окно на блок вперёд: его второй блок -- новый первый, дочитать один */
    for (e = 0; e < 2; e++)
        if (xc[e].k == k && xc[e].base + PCM_BLK == base) {
            memmove(xc[e].d, xc[e].d + PCM_BLK, PCM_BLK);
            xc_miss++;
            if (slot[k].xcap - base >= 2 * PCM_BLK
                && ops.xmem_read(ops.ctx, slot[k].xoff + base + PCM_BLK, xc[e].d + PCM_BLK, PCM_BLK) < 0) {
                xc[e].k = 0;
                return 0;
            }
            xc[e].base = base; xc_last = e;
            return (int8_t)xc[e].d[j - base];
        }
    e = 1 - xc_last;
    n = slot[k].xcap - base >= 2 * PCM_BLK ? 2 * PCM_BLK : PCM_BLK;
    xc_miss++;
    if (ops.xmem_read(ops.ctx, slot[k].xoff + base, xc[e].d, n) < 0) { xc[e].k = 0; return 0; }
    xc[e].k = k; xc[e].base = base; xc_last = e;
    return (int8_t)xc[e].d[j - base];
}

static int fx_at(const voice_t *v, long long f)
{
    long len, total, i, j;
    long long p, rel = f - v->start, end;
    int s0, s1, x, edge;
    if (!v->k || rel < 0 || !slot_has(v->k) || !(len = slot[v->k].len))
        return 0;
    total = len * v->reps;
    p = rel * v->step;
    i = (long)(p >> 16);
    if (i >= total) return 0;
    j = i % len;
    s0 = sample(v->k, j);
    s1 = i + 1 < total ? sample(v->k, (j + 1) % len) : 0;
    x = (s0 << 8) + (int)(((long long)(s1 - s0) * (p & 0xFFFF)) >> 8);
    /* края -- плавные: сэмпл может начинаться и кончаться не с нуля */
    end = (((long long)total << 16) - p) / v->step;         /* кадров до конца */
    edge = FX_EDGE;
    if (rel < edge) edge = (int)rel;
    if (end < edge) edge = (int)end;
    if (v->cut >= 0 && f >= v->cut) {
        if (f >= v->cut + FX_EDGE) return 0;
        if (v->cut + FX_EDGE - f < edge) edge = (int)(v->cut + FX_EDGE - f);
    }
    if (edge < FX_EDGE) x = x * edge / FX_EDGE;
    return x * fx_vol / 16;
}

/* Мягкий потолок: до 3/4 шкалы -- как есть, дальше сжатие к краю. */
static int soft(int x)
{
    const int k = 24576, room = 32767 - k;
    int a = x < 0 ? -x : x;
    if (a > k) a = k + (int)((long long)(a - k) * room / (a - k + room));
    return x < 0 ? -a : a;
}

/* Эффекты в блок abs поверх музыки (blk -- музыка без эффекта): блок
 * раскрывается в 16 бит, сводится и кодируется заново -- сдвиг блока может
 * стать меньше, если эффект громче музыки. */
static void fx_mix(long abs, uint8_t *blk)
{
    static int16_t fr[2 * FRAMES];
    long long f0 = (long long)abs * FRAMES, e0 = fx_end(&fx), e1 = fx_end(&fx_old);
    int k;
    if ((!fx.k || e0 <= f0 || fx.start >= f0 + FRAMES) && (!fx_old.k || e1 <= f0 || fx_old.start >= f0 + FRAMES))
        return;
    decode(blk, fr);
    for (k = 0; k < FRAMES; k++) {
        int e = fx_at(&fx, f0 + k) + fx_at(&fx_old, f0 + k);
        if (!e) continue;
        fr[2 * k] = (int16_t)soft(fr[2 * k] + e);
        fr[2 * k + 1] = (int16_t)soft(fr[2 * k + 1] + e);
    }
    encode(fr, blk);
}

/* Блоки [from, wabs) -- заново: музыка из mir плюс эффекты. Блок, которого нет
 * в mir (записан до режима игры), остаётся как был. */
static void rewrite(long from)
{
    static uint8_t blk[PCM_BLK];
    long b;
    for (b = from; b < wabs; b++) {
        int m = (int)(b % MIR);
        if (mir_abs[m] != b) continue;
        memcpy(blk, mir[m], PCM_BLK);
        fx_mix(b, blk);
        if (ops.ring_write(ops.ctx, (int)(b % PCM_NBLK), blk) < 0) break;
    }
}

/* Первый блок, который ещё можно переписать: ПЛИС до него не дойдёт, пока
 * пишем. */
static long fx_first(void)
{
    long rabs = wabs - ((wblk - rp) & (PCM_NBLK - 1));
    long b = rabs + PCM_FX_LEAD;
    return b < wabs ? b : wabs;
}

/* Прежний голос гаснет с кадра cut; новый (k = 0 -- никакого) звучит с него. */
static void fx_switch(int k, int reps, unsigned rate)
{
    long first = fx_first();
    long long cut = (long long)first * FRAMES;
    d_rw++; rw_first = first;
    fx_old = fx;
    if (fx_old.k && (fx_old.cut < 0 || fx_old.cut > cut)) fx_old.cut = cut;
    fx.k = k; fx.reps = reps; fx.cut = -1; fx.start = cut;
    fx.step = rate ? (uint32_t)(((uint64_t)rate << 16) / PCM_RATE) : 0;
    rewrite(first);
}

static void fx_play(int n)
{
    int k;
    if (n < 1 || n > fx_n || !(k = fx_tab[n - 1].k) || !slot[k].len || !slot_has(k)) { err = 1; return; }
    err = 0;
    fx_switch(k, fx_tab[n - 1].reps, fx_tab[n - 1].rate);
}

static void fx_kill_slot(int k)
{
    if (fx.k == k) fx.k = 0;
    if (fx_old.k == k) fx_old.k = 0;
}

/* --------------------------------------------------------------- кольцо */
/* PCM_GAME_STARVE (rev4, CMakeLists.txt): в режиме игры без музыки и
 * эффекта тишину в кольцо не писать. ПЛИС на границе держит последний отсчёт
 * -- середину шкалы, -- а запись блока в статику rev4 под нагрузкой шины стоит
 * до 9 мс: 172 блока тишины в секунду отнимали у загрузки Gobliiins почти всё
 * время ESP (14.11.2026). Перед остановкой -- два блока тишины, чтобы на выходе
 * точно стояла середина. Сторож при этом стоит (выход не движется). */
#ifdef PCM_GAME_STARVE
static int quiet;                       /* блоков тишины подряд в режиме игры */
static int fx_live(const voice_t *v, long long f0)
{
    return v->k && fx_end(v) > f0 && v->start < f0 + FRAMES;
}
#endif

static void fill(int max)
{
    static uint8_t blk[PCM_BLK];
    int n = 0, ahead = gm ? PCM_GAME_AHEAD : PCM_AHEAD;
    while (n < max && ((wblk - rp) & (PCM_NBLK - 1)) < ahead) {
        int m = (int)(wabs % MIR);
#ifdef PCM_GAME_STARVE
        long long f0 = (long long)wabs * FRAMES;
        int idle = gm && !src_on && !ramp_in && !ramp_out && !fx_live(&fx, f0) && !fx_live(&fx_old, f0);
        if (idle && quiet >= 2) break;
        quiet = idle ? quiet + 1 : 0;
#endif
        if (!make_block(blk)) break;
        memcpy(mir[m], blk, PCM_BLK); mir_abs[m] = wabs;
        fx_mix(wabs, blk);
        if (ops.ring_write(ops.ctx, wblk, blk) < 0) { mir_abs[m] = -1; break; }
        wblk = (wblk + 1) & (PCM_NBLK - 1);
        wabs++;
        n++;
    }
    if (done && st == S_PLAY) st = S_DRAIN;
}

/* Кольцо с начала (run = 0 в следующем обмене): блок 0 -- новый номер. */
static void ring_restart(void)
{
    int k;
    st = S_START; run = 0;
    wabs = (wabs + PCM_NBLK) & ~(long)(PCM_NBLK - 1);
    fx.k = fx_old.k = 0;
    for (k = 0; k < MIR; k++) mir_abs[k] = -1;
}

static void start(int rc, int lp);
static void ext_command(void);
static void stop_music(void);

/* Музыку -- стоп. В режиме игры выход идёт дальше: музыка гаснет к середине
 * со следующего блока, эффект звучит. */
static void music_off(void)
{
    if (!gm) { stop_music(); return; }
    if (src_on) { src_close(); ramp_out = RAMP; }
}

static void stop_music(void)
{
    /* Стоп: дальше того блока, что играет, -- плавный выход от середины
     * и тишина. Что уже лежит впереди, не играется. */
    if (st == S_IDLE) { err = 0; return; }
    src_close();
    if (st == S_START) { st = S_IDLE; run = 0; done = 1; return; }
    ramp_in = 0; ramp_out = RAMP; last_l = last_r = 0; done = 0;
    wabs = wabs - ((wblk - rp) & (PCM_NBLK - 1)) + 1;
    wblk = (rp + 1) & (PCM_NBLK - 1);
    st = S_PLAY;
    fill(1);
    err = 0;
}

void pcm_command(int cmd)
{
    cmd &= 0xFF;
    wl_b[wl_i] = (uint8_t)cmd; wl_t[wl_i] = NOW_US(); wl_i = (wl_i + 1) % WD_LOG;
    if (gm) wd_kick = 1;                    /* любая почта -- пинок */
    if (ext_n) {
        /* байт адреса блока параметров длинной команды: младший, старший */
        if (ext_n == 2) ext_a = (uint16_t)cmd;
        else            ext_a |= (uint16_t)(cmd << 8);
        if (--ext_n == 0) ext_command();
        return;
    }
    if (cmd == 0x80) {
        long long t = NOW_US();
        if (z_end >= 0) z_gap += t - z_end;
        z_t80 = t; ext_n = 2; hurry = 200; d_ext++; return;
    }
    if (cmd == 0xFF) d_kick++;
    else if (cmd == 0x00) d_stop++;
    else if (cmd < 0x80) d_fx++;
    else d_other++;
    if (gm) {
        if (cmd == 0x00) { err = 0; if (fx.k) fx_switch(0, 0, 0); }
        else if (cmd < 0x80) fx_play(cmd);
        else if (cmd != 0xFF) err = 1;      /* FFh -- пинок: разряд 2 не трогает */
        return;
    }
    if (cmd == 0xFF) return;                /* пинок и вне режима игры: загрузчик пинает до 07h */
    if (wd_dead) { err = 1; return; }       /* сторож сработал: ждём 07h */
    if (cmd == 0x00) { stop_music(); return; }
    src_close();
    memset(&src, 0, sizeof src);
    start(ops.open(ops.ctx, cmd & 0x7F, &src), (cmd & 0x80) != 0);
}

/* Пустить открытый источник (rc -- как открылся). */
static void start(int rc, int lp)
{
    if (rc != 0 || src.rate <= 0 || (src.ch != 1 && src.ch != 2) || bufs_get() < 0) {
        if (src.close) src.close(&src);
        if (st != S_IDLE && !gm) stop_music();
        err = 1;
        return;
    }
    err = 0; loop = lp;
    src_on = 1; in_n = in_i = 0;
    step = (uint32_t)(((uint64_t)src.rate << 16) / PCM_RATE);
    b_l = b_r = 0;
    if (next_frame() == 0) {
        src_close();
        if (st != S_IDLE && !gm) stop_music();
        err = 1;
        return;
    }
    if (next_frame() == 0) { a_l = b_l; a_r = b_r; }
    frac = 0;
    ramp_out = 0; done = 0;
    if (gm && st != S_IDLE && st != S_START) {
        /* выход уже на середине и работает: музыка вступает со следующего
         * блока, кольцо и эффект не трогаем */
        ramp_in = 0;
        st = S_PLAY;
        return;
    }
    ramp_in = RAMP;
    ring_restart();                     /* следующий обмен вернёт кольцо в начало */
}

/* ------------------------------------------------------- режим игры */
static void game(int on)
{
    if (on == gm) return;
    if (on) {
        gm = 1;
        wd_kick = 1;
        if (st == S_IDLE || st == S_DRAIN) {
            /* выход к середине и дальше -- без перерыва */
            done = 0; ramp_out = 0; ramp_in = RAMP;
            if (st == S_IDLE) ring_restart();
            else st = S_PLAY;
        }
        return;
    }
    if (fx.k) fx_switch(0, 0, 0);
    gm = 0;
    if (!src_on && st != S_IDLE) {      /* без музыки -- от середины к нулю */
        if (ramp_in) {                  /* ещё не дошли до середины */
            last_l = last_r = (REST * ramp_in) / RAMP;
            ramp_in = 0;
        }
        else if (ramp_out) {            /* гасла к середине: оттуда, где сейчас */
            last_l = (int)(((long)last_l * ramp_out) / RAMP);
            last_r = (int)(((long)last_r * ramp_out) / RAMP);
        }
        else last_l = last_r = 0;
        ramp_out = RAMP;
    }
}

/* ------------------------------------------------------- длинные команды */
static long slots_total(void)
{
    long t = 0;
    int k;
    for (k = 1; k < PCM_SLOTS; k++) t += slot[k].cap;
    return t;
}

static void slot_free(int k)
{
    if (playing_slot == k) music_off();             /* его память под источником */
    fx_kill_slot(k);
    free(slot[k].d);
    stage_drop(k);
    if (slot[k].f) fclose(slot[k].f);
    xc_drop(k);
    slot[k].d = 0; slot[k].f = 0; slot[k].len = slot[k].cap = slot[k].size = 0;
    slot[k].xm = 0; slot[k].xoff = slot[k].xcap = 0; slot[k].old = 0;
}

/* Место в статике на size байт: первый промежуток между ячейками, по блокам;
 * -1 -- нет. */
static long xmem_find(long size)
{
    long need = (size + PCM_BLK - 1) & ~(long)(PCM_BLK - 1), at = 0;
    int k, moved;
    do {
        moved = 0;
        for (k = 1; k < PCM_SLOTS; k++)
            if (slot[k].xm && at < slot[k].xoff + slot[k].xcap && slot[k].xoff < at + need) {
                at = slot[k].xoff + slot[k].xcap;
                moved = 1;
            }
    } while (moved);
    return at + need <= ops.xmem_size ? at : -1;
}

/* Дописать в ячейку в статике. Статика пишется блоками: неполный блок --
 * прочитать, дополнить, записать (хвост блока за длиной -- мусор, он свой). */
static int xmem_append(int k, uint16_t a, uint16_t n)
{
    static uint8_t blk[PCM_BLK];
    if (slot[k].len + n > slot[k].xcap) return -1;
    xc_drop(k);
    while (n) {
        long at = slot[k].len, base = at & ~(long)(PCM_BLK - 1);
        int o = (int)(at - base), m = PCM_BLK - o;
        if (m > n) m = n;
        if (o && ops.xmem_read(ops.ctx, slot[k].xoff + base, blk, PCM_BLK) < 0) return -1;
        if (ops.mem_read(ops.ctx, a, blk + o, m) < 0) return -1;
        if (ops.xmem_write(ops.ctx, slot[k].xoff + base, blk, PCM_BLK) < 0) return -1;
        if (!at) slot[k].x0 = blk[0];
        slot[k].len += m; a = (uint16_t)(a + m); n = (uint16_t)(n - m);
    }
    return 0;
}

/* Начать ячейку; size > 0 -- память сразу на всю длину (или файл). */
static int slot_begin(int k, long size)
{
    slot_free(k);
    if (!size) return 0;
    if (ops.slot_file && size >= PCM_FILE_MIN) {
        if (!(slot[k].f = ops.slot_file(ops.ctx, k, size, &slot[k].old))) return -1;
        slot[k].size = size;
        return 0;
    }
    if (ops.xmem_size && size < PCM_FILE_MIN) {
        long off = xmem_find(size);
        if (off < 0) return -1;
        slot[k].xm = 1; slot[k].xoff = off;
        slot[k].xcap = (size + PCM_BLK - 1) & ~(long)(PCM_BLK - 1);
        slot[k].size = size;
        return 0;
    }
    if (slots_total() + size > PCM_MEM_MAX) return -1;
    if (!(slot[k].d = malloc((size_t)size))) return -1;
    slot[k].cap = slot[k].size = size;
    return 0;
}

/* Дописать в ячейку-файл. Длина растёт по кускам: источник, который её
 * играет, видит только записанное. */
/* Дописывание копится в fst и уходит в файл кусками до границы FSTAGE
 * (14.11.2026): по 512 байт вразнобой карта rev4 на каждый кусок читала и
 * писала сектор, а под такими мелкими записями уходила в уборку дольше 5 с.
 * Целые 4 КБ по границе -- одна запись восьми секторов. slot[k].len -- только
 * то, что уже в файле: источник видит записанное. Остаток уходит, когда
 * ячейка дописана до длины 0Ah, перед её пуском, перед дописыванием другой
 * ячейки и после паузы (stage_tick). fst -- из кучи, пока есть что писать. */
#define FSTAGE 4096
static uint8_t *fst;
static int      fst_k, fst_n, fst_idle;

static unsigned long st_same, st_wrote;  /* байт: совпало со старым файлом, записано */

/* Кусок уже лежит в старом файле (slot_file, *old)? Сверка по 512 байт. */
static int stage_same(int k, int n)
{
    static uint8_t cmp[PCM_BLK];
    int at = 0;
    if (slot[k].len + n > slot[k].old || fseek(slot[k].f, slot[k].len, SEEK_SET) != 0) return 0;
    while (at < n) {
        int m = n - at > PCM_BLK ? PCM_BLK : n - at;
        if (fread(cmp, 1, (size_t)m, slot[k].f) != (size_t)m || memcmp(cmp, fst + at, (size_t)m)) return 0;
        at += m;
    }
    return 1;
}

static int stage_flush_do(void);
/* Замер загрузки ячеек (14.11.2026, форк сессии: темы Gobliiins грузятся
 * ~4 КБ/с): мкс от 80h до разбора, разбор целиком, сверка/запись файла,
 * пауза «Вектора» между командами; байт дописано. */
void pcm_debug3(long long v[6], unsigned long *n, unsigned long *bytes)
{
    v[0] = z_mail; v[1] = z_exe; v[2] = z_fl; v[3] = z_gap; *n = z_n; *bytes = z_bytes;
    z_mail = z_exe = z_fl = z_gap = 0; z_n = z_bytes = 0;
}
static int stage_flush(void)
{
    long long t = NOW_US();
    int rc = stage_flush_do();
    z_fl += NOW_US() - t;
    return rc;
}
static int stage_flush_do(void)
{
    int k = fst_k, n = fst_n;
    fst_n = 0;
    if (!n) return 0;
    if (!slot[k].f) return -1;
    if (stage_same(k, n)) st_same += (unsigned long)n;
    else {
        if (fseek(slot[k].f, slot[k].len, SEEK_SET) != 0
            || fwrite(fst, 1, (size_t)n, slot[k].f) != (size_t)n || fflush(slot[k].f) != 0)
            return -1;
        st_wrote += (unsigned long)n;
    }
    slot[k].len += n;
    return 0;
}

void pcm_file_stats(unsigned long *same, unsigned long *wrote) { *same = st_same; *wrote = st_wrote; }

static void stage_drop(int k)
{
    if (fst_n && fst_k == k) fst_n = 0;
}

/* Шаг проигрывателя: дописывания нет -- остаток в файл, буфер в кучу. */
static void stage_tick(void)
{
    if (!fst || ++fst_idle < 20) return;
    if (fst_n && stage_flush() < 0) err = 1;
    free(fst); fst = 0;
}

static int file_append(int k, uint16_t a, uint16_t n)
{
    if (fst_n && fst_k != k && stage_flush() < 0) return -1;
    if (!fst && !(fst = malloc(FSTAGE))) return -1;
    fst_k = k; fst_idle = 0;
    while (n) {
        int target = FSTAGE - (int)(slot[k].len % FSTAGE);   /* до границы */
        int m = target - fst_n;
        if (m > n) m = n;
        if (ops.mem_read(ops.ctx, a, fst + fst_n, m) < 0) return -1;
        fst_n += m; a = (uint16_t)(a + m); n = (uint16_t)(n - m);
        if (fst_n == target && stage_flush() < 0) return -1;
    }
    if (slot[k].size && slot[k].len + fst_n >= slot[k].size && stage_flush() < 0) return -1;
    return 0;
}

/* Ячейка 01h доросла до PCM_FILE_MIN -- в файл. Играющую останавливаем:
 * источник читает её память, а её больше нет. */
static int slot_to_file(int k)
{
    long old;
    FILE *f = ops.slot_file(ops.ctx, k, 0, &old);
    if (!f) return -1;
    if (slot[k].len && (fwrite(slot[k].d, 1, (size_t)slot[k].len, f) != (size_t)slot[k].len || fflush(f) != 0)) {
        fclose(f);
        return -1;
    }
    if (playing_slot == k) music_off();
    fx_kill_slot(k);
    free(slot[k].d);
    slot[k].d = 0; slot[k].cap = 0; slot[k].f = f;
    return 0;
}

static int slot_append(int k, uint16_t a, uint16_t n)
{
    z_bytes += n;
    long need = slot[k].len + n;
    if ((long)a + n > 0x10000L) return -1;
    if (!slot[k].f && !slot[k].xm && ops.slot_file && need >= PCM_FILE_MIN && slot_to_file(k) < 0) return -1;
    if (slot[k].f) return file_append(k, a, n);
    if (slot[k].xm) return xmem_append(k, a, n);
    if (need > slot[k].cap) {
        long cap = slot[k].cap ? slot[k].cap : 4096;
        uint8_t *d;
        while (cap < need) cap *= 2;
        if (slots_total() - slot[k].cap + cap > PCM_MEM_MAX) {
            cap = need;                             /* впритык -- может и влезть */
            if (slots_total() - slot[k].cap + cap > PCM_MEM_MAX) return -1;
        }
        /* realloc сдвинет память: WAV и MP3 читают ячейку через ссылку на её
         * начало (pcm_io_t) и это переживут, MOD держит адрес -- его стоп */
        if (playing_slot == k && src.fixed) music_off();
        d = realloc(slot[k].d, (size_t)cap);
        if (!d) return -1;
        slot[k].d = d; slot[k].cap = cap;
    }
    if (n && ops.mem_read(ops.ctx, a, slot[k].d + slot[k].len, n) < 0) return -1;
    slot[k].len = need;
    return 0;
}

static int le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* Таблица эффектов: n записей по 4 байта с адреса a. */
static int fx_table(int n, uint16_t a)
{
    uint8_t t[PCM_FX_MAX * 4];
    int i;
    if (n > PCM_FX_MAX || (long)a + 4 * n > 0x10000L) return -1;
    if (n && ops.mem_read(ops.ctx, a, t, 4 * n) < 0) return -1;
    for (i = 0; i < n; i++) {
        unsigned rate = (unsigned)le16(t + 4 * i + 2);
        if (t[4 * i] >= PCM_SLOTS || !t[4 * i + 1] || rate < 1000 || rate > 48000) return -1;
    }
    for (i = 0; i < n; i++) {
        fx_tab[i].k = t[4 * i]; fx_tab[i].reps = t[4 * i + 1];
        fx_tab[i].rate = (uint16_t)le16(t + 4 * i + 2);
    }
    fx_n = n;
    return 0;
}

/* Блок параметров -- в ОЗУ «Вектора» по ext_a (зеркало в статике). */
static void ext_command_do(void);
static void ext_command(void)
{
    long long t = NOW_US();
    if (z_t80 >= 0) z_mail += t - z_t80;
    ext_command_do();
    z_end = NOW_US(); z_exe += z_end - t; z_n++;
}
static void ext_command_do(void)
{
    uint8_t b[6];
    int ok = 0, k;
    if (ops.mem_read(ops.ctx, ext_a, b, sizeof b) == 0) {
        wl_op = b[0]; wl_op_t = NOW_US();
        k = b[1];
        switch (b[0]) {
        case PCM_OP_BEGIN:
            ok = k >= 1 && k < PCM_SLOTS && slot_begin(k, 0) == 0;
            break;
        case PCM_OP_BEGIN_SIZE:
            ok = k >= 1 && k < PCM_SLOTS && slot_begin(k, b[2] | (long)b[3] << 8 | (long)b[4] << 16) == 0;
            break;
        case PCM_OP_APPEND:
            ok = k >= 1 && k < PCM_SLOTS && slot_append(k, (uint16_t)le16(b + 2), (uint16_t)le16(b + 4)) == 0;
            break;
        case PCM_OP_PLAY:
            if (fst_n && fst_k == k && stage_flush() < 0) break;
            if (k >= 1 && k < PCM_SLOTS && slot[k].len > 0 && !slot[k].xm) {  /* в статике -- только эффект */
                src_close();
                memset(&src, 0, sizeof src);
                start(!slot[k].f ? ops.open_mem(ops.ctx, &slot[k].d, &slot[k].len, slot[k].size, &src)
                      : ops.open_file ? ops.open_file(ops.ctx, slot[k].f, &slot[k].len, slot[k].size, &src)
                      : -1, b[2] & 1);
                if (src_on) playing_slot = k;
                ok = !err;
            }
            break;
        case PCM_OP_VOLUME:
            if (b[1] <= 16) { pcm_volume(b[1]); ok = 1; }
            break;
        case PCM_OP_FREE:
            if (k == 0) { for (k = 1; k < PCM_SLOTS; k++) slot_free(k); ok = 1; }
            else if (k < PCM_SLOTS) { slot_free(k); ok = 1; }
            break;
        case PCM_OP_FX_TABLE:
            ok = fx_table(k, (uint16_t)le16(b + 2)) == 0;
            break;
        case PCM_OP_GAME:
            if (k <= 1 && (!k || bufs_get() == 0)) { game(k); wd_dead = 0; ok = 1; }
            break;
        case PCM_OP_STOP:
            if (k & 1) music_off();
            if ((k & 2) && fx.k) fx_switch(0, 0, 0);
            ok = 1;
            break;
        case PCM_OP_FX_VOLUME:
            if (b[1] <= 16) { fx_vol = b[1]; ok = 1; }
            break;
        case PCM_OP_WATCHDOG:
            wd_t = k ? k : WD_DEFAULT;
            ok = 1;
            break;
        }
    }
    err = !ok;
    seq ^= 1;                                       /* команда исполнена */
    if (gm) wd_kick = 1;    /* игра ждала ответа -- жива (rev4: ответ бывает и через секунды) */
}

/* Отладка (pcm4.c, /api/pcm?slotplay=1): готовый файл f длиной len --
 * ячейка-файл k, и сразу пуск, как 03h k 0. */
void pcm_debug_slot_play(int k, FILE *f, long len)
{
    if (k < 1 || k >= PCM_SLOTS || !f) return;
    slot_free(k);
    slot[k].f = f; slot[k].len = slot[k].size = slot[k].old = len;
    src_close();
    memset(&src, 0, sizeof src);
    start(ops.open_file ? ops.open_file(ops.ctx, f, &slot[k].len, slot[k].size, &src) : -1, 0);
    if (src_on) playing_slot = k;
}

/* Сброс машины (rev4: флаг сброса эмулятора винта, pcm4.c): музыку и
 * эффект -- сразу, не дожидаясь сторожа; режим игры -- выключить, как
 * сработавший сторож (короткая почта ничего не значит до 07h). */
/* Часы сторожа: на ESP -- настоящие; в эмуляторах (модель ПЛИС, pcm_host.c)
 * -- по выходу ПЛИС, приращение за шаг не больше 50 мс (см. pcm_tick). */
static long long wd_clock_us(void)
{
#ifdef ESP_PLATFORM
    return NOW_US();
#else
    return play_frame() * 1000000LL / PCM_RATE;
#endif
}

static void wd_note(long ms)
{
    long long tn = NOW_US();
    int i, n;
    n = snprintf(wd_txt, sizeof wd_txt,
                 "watchdog: no kick for %ld ms (limit %d00 ms); music %s, effect %s; mail (newest first):",
                 ms, wd_t > PCM_WD_MIN ? wd_t : PCM_WD_MIN, src_on ? "on" : "off", fx.k ? "on" : "off");
    for (i = 1; i <= WD_LOG && n < (int)sizeof wd_txt - 16; i++) {
        int j = (wl_i - i + WD_LOG) % WD_LOG;
        if (!wl_t[j]) break;
        n += snprintf(wd_txt + n, sizeof wd_txt - (size_t)n, " %02X@-%ld", wl_b[j], (long)((tn - wl_t[j]) / 1000));
    }
    if (n < (int)sizeof wd_txt - 40)
        snprintf(wd_txt + n, sizeof wd_txt - (size_t)n, "; last long op %02X %ld ms ago",
                 wl_op, wl_op_t ? (long)((tn - wl_op_t) / 1000) : -1L);
    wd_new = 1;
}

const char *pcm_wd_report(void)
{
    if (!wd_new) return 0;
    wd_new = 0;
    return wd_txt;
}

void pcm_machine_reset(void)
{
    int was = gm || st != S_IDLE;
    if (gm) {
        music_off();
        game(0);
        wd_dead = 1;
    }
    /* и отвести границу кольца к rp + 1: в режиме игры впереди записано до
     * PCM_GAME_AHEAD блоков (rev4 -- 116 мс), без этого они доигрывали бы
     * после сброса (плата №1 14.11.2026: тишина через 0,4-0,7 с) */
    stop_music();
    if (was) d_resets++;
}

/* Признак жизни «Вектора» со стороны (rev4: команда винта, pcm4.c). */
void pcm_kick(void)
{
    if (gm) wd_kick = 1;
}

int pcm_tick(int max)
{
    int mail = -1;
    uint8_t ctl;
    ctl = (uint8_t)((pcm_state() & 0xFE) | (run ? 0x01 : 0));
    {
        /* ПЛИС не уходит вперёд записанного (граница -- wblk): указатель
         * дальше PCM_AHEAD блоков впереди -- испорчен обменом. Один такой
         * сдвигал отсчёт сторожа на ~3 с (подозрение, плата №1 14.11.2026):
         * отбросить и сосчитать. */
        uint16_t nrp;
        if (ops.xchg(ops.ctx, ctl, (uint16_t)wblk, &nrp, &mail) < 0) return -1;
        /* только в режиме игры: вне его стоп (stop_music) отводит wblk назад,
         * к rp + 1, и ПЛИС честно оказывается впереди (PCMTEST на плате №1:
         * указатель отбрасывался на каждом обмене, кольцо не доливалось) */
        if (gm && ((wblk - nrp) & (PCM_NBLK - 1)) > PCM_AHEAD && st != S_START) rp_bad++;
        else rp = nrp;
        if (st == S_PLAY && src_on && (unsigned long)((wblk - rp) & (PCM_NBLK - 1)) < d_ahead)
            d_ahead = (unsigned long)((wblk - rp) & (PCM_NBLK - 1));
        if (rw_first >= 0) {
            if (wabs - ((wblk - rp) & (PCM_NBLK - 1)) > rw_first) d_rw_late++;
            rw_first = -1;
        }
    }
    if (st == S_START && !run) {
        /* ПЛИС только что получила run = 0 -- указатель в начале */
        rp = 0; wblk = 0;
        fill(max);
        st = done ? S_DRAIN : S_PLAY;
        run = 1;
    }
    else if (st == S_PLAY) {
#ifdef PCM_GAME_STARVE
        if (rp == wblk && !starved && quiet < 2) { under++; starved = 1; }
#else
        if (rp == wblk && !starved) { under++; starved = 1; }
#endif
        fill(max);
        if (rp != wblk) starved = 0;
    }
    else if (st == S_DRAIN && rp == wblk) { st = S_IDLE; run = 0; }
    if (mail >= 0) { pcm_command(mail); if (!gm) hurry = 200; }
    /* СТОРОЖ ПО ЧАСАМ ESP (14.11.2026). Прежде время шло по позиции выхода
     * ПЛИС (play_frame: wabs - (wblk - rp)), а она прыгает, когда граница
     * кольца отводится назад: на плате №1 сторож насчитал 1,48 с без пинка
     * при пинках каждые 20 мс. Теперь время копится по часам, только пока в
     * режиме игры что-то звучит (музыка, её затухание или эффект), и
     * обнуляется пинком. Почта разобрана выше, до проверки: пинок, пришедший
     * за долгий шаг задачи, лежит в ящике ПЛИС и успевает. */
    {
        long long t = wd_clock_us();
        long long dt = wd_prev_us ? t - wd_prev_us : 0;
        if (dt < 0 || dt > 50000) dt = dt < 0 ? 0 : 50000;   /* модель: позиция выхода прыгает */
        /* эффект звучит, пока не кончился по времени: номер ячейки (fx.k,
         * fx_old.k) остаётся и после конца -- по нему сторож считал и в
         * тишине (проверочный набор Gobliiins на плате №1: 08h, пинки,
         * загрузка без пинков -- срабатывание через 0,5 с) */
        long long pf = play_frame();
        int sounding = (st == S_PLAY || st == S_DRAIN)
                    && (src_on || ramp_out || (fx.k && fx_end(&fx) > pf) || (fx_old.k && fx_end(&fx_old) > pf));
        wd_prev_us = t;
        if (!gm) wd_acc_us = 0;
        else if (wd_kick) {
            if ((unsigned long)(wd_acc_us / 1000) > d_kgap) d_kgap = (unsigned long)(wd_acc_us / 1000);
            wd_acc_us = 0; wd_kick = 0;
        }
        else if (sounding) {
            wd_acc_us += dt;
            if (!ext_n && wd_acc_us > (long long)(wd_t > PCM_WD_MIN ? wd_t : PCM_WD_MIN) * 100000LL) {
                wd_note((long)(wd_acc_us / 1000));
                wd_acc_us = 0;
                music_off();
                game(0);
                wd_dead = 1;
                wd_fired++;
            }
        }
    }
    stage_tick();
    if (st == S_IDLE && !gm && !src_on && (inb || mir)) bufs_put();   /* простой -- память отдать */
    if (hurry) { hurry--; return 2; }
    if (gm) return 2;
    return st != S_IDLE;
}
