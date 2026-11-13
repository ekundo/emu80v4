/* Проигрыватель PCM rev5 -- без платы (pcm_play.h). */
#include "pcm_play.h"

#include <stdlib.h>
#include <string.h>

#define RAMP  512                       /* кадров плавного входа и выхода, 11,6 мс */
#define INBUF 2304                      /* отсчётов источника за раз (кадр MP3 стерео) */
#define FRAMES (PCM_BLK / 2)            /* кадров в блоке кольца */
#define MIR   16                        /* блоков музыки без эффекта: больше PCM_GAME_AHEAD */
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
static int16_t   inb[INBUF];
static int       in_n, in_i;            /* кадров в inb, следующий кадр */
static uint32_t  frac, step;            /* позиция между кадрами a и b, 16.16 */
static int       a_l, a_r, b_l, b_r;
static int       ramp_in, ramp_out;     /* кадров осталось */
static uint8_t   last_l, last_r;        /* последний выданный отсчёт */
static int       done;                  /* всё выдано, дальше только тишина */
static unsigned long under;             /* недоборов */
static int       starved;               /* ПЛИС стоит на границе (считан один раз) */

/* Ячейки, которые игра загрузила сама (длинные команды, pcm_play.h). */
static struct { uint8_t *d; long len, cap, size; } slot[PCM_SLOTS];
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
#define WD_DEFAULT 20                   /* 2 с */
static int       wd_t = WD_DEFAULT, wd_dead;
static int       wd_kick;               /* пинок пришёл, отметку ещё не взяли */
static long long wd_last;
static unsigned long wd_fired;
static uint8_t   mir[MIR][PCM_BLK];
static long      mir_abs[MIR];

void pcm_init(const pcm_ops_t *o)
{
    int k;
    ops = *o;
    st = S_IDLE; run = 0; err = 0; ready = 1; wblk = 0; wabs = 0; rp = 0;
    src_on = 0; done = 1; last_l = last_r = 0;
    ext_n = 0; playing_slot = 0;
    gm = 0; fx_n = 0; fx.k = fx_old.k = 0; fx_vol = 16;
    wd_t = WD_DEFAULT; wd_dead = 0; wd_kick = 0; wd_last = 0; wd_fired = 0;
    under = 0; starved = 0;
    for (k = 0; k < MIR; k++) mir_abs[k] = -1;
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
    if (!v->k || !slot[v->k].d || !v->step) return v->start;
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

static uint8_t to_u8(int v)
{
    v = (v * vol) >> 4;
    v = (v + 128) >> 8;
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (uint8_t)(v + 128);
}

/* Один кадр музыки. 0 -- выдавать нечего (done). В режиме игры без музыки --
 * середина (80h): выход не стоит, эффекту есть на чём звучать. */
static int out_frame(uint8_t *l, uint8_t *r)
{
    if (ramp_in) {
        *l = *r = last_l = last_r = (uint8_t)(0x80 - (0x80 * ramp_in) / RAMP);
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
        *l = last_l = to_u8(vl); *r = last_r = to_u8(vr);
        return 1;
    }
    if (ramp_out) {
        /* к нулю, а в режиме игры -- к середине: выход живёт дальше */
        int t = gm ? 0x80 : 0;
        *l = (uint8_t)(t + ((last_l - t) * (ramp_out - 1)) / RAMP);
        *r = (uint8_t)(t + ((last_r - t) * (ramp_out - 1)) / RAMP);
        if (--ramp_out == 0) {
            if (gm) last_l = last_r = 0x80;
            else done = 1;
        }
        return 1;
    }
    if (gm) { *l = *r = 0x80; return 1; }
    return 0;
}

/* Блок музыки; 0 -- нечего класть. Хвост последнего блока -- нули. */
static int make_block(uint8_t *blk)
{
    int k;
    if (done) return 0;
    for (k = 0; k < FRAMES; k++) {
        uint8_t l = 0, r = 0;
        if (done || !out_frame(&l, &r)) l = r = 0;
        blk[2 * k] = l; blk[2 * k + 1] = r;
    }
    return 1;
}

/* ---------------------------------------------------------- эффект */
/* Отсчёт голоса на кадре f (от запуска плеера), 16 бит; 0 -- молчит. */
static int fx_at(const voice_t *v, long long f)
{
    const int8_t *d;
    long len, total, i, j;
    long long p, rel = f - v->start, end;
    int s0, s1, x, edge;
    if (!v->k || rel < 0 || !(d = (const int8_t *)slot[v->k].d) || !(len = slot[v->k].len))
        return 0;
    total = len * v->reps;
    p = rel * v->step;
    i = (long)(p >> 16);
    if (i >= total) return 0;
    j = i % len;
    s0 = d[j];
    s1 = i + 1 < total ? d[(j + 1) % len] : 0;
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

static uint8_t mix8(uint8_t m, int e)
{
    int v = soft(((m - 128) << 8) + e);
    v = (v + 128) >> 8;
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (uint8_t)(v + 128);
}

/* Эффекты в блок abs поверх музыки (blk -- музыка без эффекта). */
static void fx_mix(long abs, uint8_t *blk)
{
    long long f0 = (long long)abs * FRAMES, e0 = fx_end(&fx), e1 = fx_end(&fx_old);
    int k;
    if ((!fx.k || e0 <= f0 || fx.start >= f0 + FRAMES) && (!fx_old.k || e1 <= f0 || fx_old.start >= f0 + FRAMES))
        return;
    for (k = 0; k < FRAMES; k++) {
        int e = fx_at(&fx, f0 + k) + fx_at(&fx_old, f0 + k);
        if (!e) continue;
        blk[2 * k] = mix8(blk[2 * k], e);
        blk[2 * k + 1] = mix8(blk[2 * k + 1], e);
    }
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
    fx_old = fx;
    if (fx_old.k && (fx_old.cut < 0 || fx_old.cut > cut)) fx_old.cut = cut;
    fx.k = k; fx.reps = reps; fx.cut = -1; fx.start = cut;
    fx.step = rate ? (uint32_t)(((uint64_t)rate << 16) / PCM_RATE) : 0;
    rewrite(first);
}

static void fx_play(int n)
{
    int k;
    if (n < 1 || n > fx_n || !(k = fx_tab[n - 1].k) || !slot[k].len) { err = 1; return; }
    err = 0;
    fx_switch(k, fx_tab[n - 1].reps, fx_tab[n - 1].rate);
}

static void fx_kill_slot(int k)
{
    if (fx.k == k) fx.k = 0;
    if (fx_old.k == k) fx_old.k = 0;
}

/* --------------------------------------------------------------- кольцо */
static void fill(int max)
{
    static uint8_t blk[PCM_BLK];
    int n = 0, ahead = gm ? PCM_GAME_AHEAD : PCM_AHEAD;
    while (n < max && ((wblk - rp) & (PCM_NBLK - 1)) < ahead) {
        int m = (int)(wabs % MIR);
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
    ramp_in = 0; ramp_out = RAMP; last_l = last_r = 0x80; done = 0;
    wabs = wabs - ((wblk - rp) & (PCM_NBLK - 1)) + 1;
    wblk = (rp + 1) & (PCM_NBLK - 1);
    st = S_PLAY;
    fill(1);
    err = 0;
}

void pcm_command(int cmd)
{
    cmd &= 0xFF;
    if (gm) wd_kick = 1;                    /* любая почта -- пинок */
    if (ext_n) {
        /* байт адреса блока параметров длинной команды: младший, старший */
        if (ext_n == 2) ext_a = (uint16_t)cmd;
        else            ext_a |= (uint16_t)(cmd << 8);
        if (--ext_n == 0) ext_command();
        return;
    }
    if (cmd == 0x80) { ext_n = 2; hurry = 200; return; }
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
    if (rc != 0 || src.rate <= 0 || (src.ch != 1 && src.ch != 2)) {
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
            last_l = last_r = (uint8_t)(0x80 - (0x80 * ramp_in) / RAMP);
            ramp_in = 0;
        }
        else if (ramp_out) {            /* гасла к середине: оттуда, где сейчас */
            last_l = (uint8_t)(0x80 + ((last_l - 0x80) * ramp_out) / RAMP);
            last_r = (uint8_t)(0x80 + ((last_r - 0x80) * ramp_out) / RAMP);
        }
        else last_l = last_r = 0x80;
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
    slot[k].d = 0; slot[k].len = slot[k].cap = slot[k].size = 0;
}

/* Начать ячейку; size > 0 -- память сразу на всю длину. */
static int slot_begin(int k, long size)
{
    slot_free(k);
    if (!size) return 0;
    if (slots_total() + size > PCM_MEM_MAX) return -1;
    if (!(slot[k].d = malloc((size_t)size))) return -1;
    slot[k].cap = slot[k].size = size;
    return 0;
}

static int slot_append(int k, uint16_t a, uint16_t n)
{
    long need = slot[k].len + n;
    if ((long)a + n > 0x10000L) return -1;
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
static void ext_command(void)
{
    uint8_t b[6];
    int ok = 0, k;
    if (ops.mem_read(ops.ctx, ext_a, b, sizeof b) == 0) {
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
            if (k >= 1 && k < PCM_SLOTS && slot[k].len > 0) {
                src_close();
                memset(&src, 0, sizeof src);
                start(ops.open_mem(ops.ctx, &slot[k].d, &slot[k].len, slot[k].size, &src), b[2] & 1);
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
            if (k <= 1) { game(k); wd_dead = 0; ok = 1; }
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
}

int pcm_tick(int max)
{
    int mail = -1;
    uint8_t ctl = (uint8_t)((pcm_state() & 0xFE) | (run ? 0x01 : 0));
    if (ops.xchg(ops.ctx, ctl, (uint16_t)wblk, &rp, &mail) < 0) return -1;
    if (st == S_START && !run) {
        /* ПЛИС только что получила run = 0 -- указатель в начале */
        rp = 0; wblk = 0;
        fill(max);
        st = done ? S_DRAIN : S_PLAY;
        run = 1;
    }
    else if (st == S_PLAY) {
        if (rp == wblk && !starved) { under++; starved = 1; }
        fill(max);
        if (rp != wblk) starved = 0;
    }
    else if (st == S_DRAIN && rp == wblk) { st = S_IDLE; run = 0; }
    if (mail >= 0) { pcm_command(mail); if (!gm) hurry = 200; }
    /* Отметку берём, только пока выход идёт: при перезапуске кольца (S_START)
     * play_frame -- мусор. Пинок до того -- отметка с первого шага выхода. */
    if (gm && (st == S_PLAY || st == S_DRAIN)) {
        long long now = play_frame();
        if (wd_kick || now < wd_last) { wd_last = now; wd_kick = 0; }
        else if (!ext_n && now - wd_last > (long long)wd_t * PCM_RATE / 10) {
            music_off();
            game(0);
            wd_dead = 1;
            wd_fired++;
        }
    }
    if (hurry) { hurry--; return 2; }
    if (gm) return 2;
    return st != S_IDLE;
}
