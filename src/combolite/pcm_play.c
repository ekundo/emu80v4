/* Проигрыватель PCM rev5 -- без платы (pcm_play.h). */
#include "pcm_play.h"

#include <stdlib.h>
#include <string.h>

#define RAMP  512                       /* кадров плавного входа и выхода, 11,6 мс */
#define INBUF 2304                      /* отсчётов источника за раз (кадр MP3 стерео) */

enum { S_IDLE, S_START, S_PLAY, S_DRAIN };

static pcm_ops_t ops;
static int       st = S_IDLE, run, loop, err, ready;
static int       wblk;                  /* следующий блок кольца, он же граница */
static uint16_t  rp;                    /* блок, который играет ПЛИС */
static int       vol = 16;

static pcm_src_t src;
static int       src_on;                /* источник открыт и не кончился */
static int16_t   inb[INBUF];
static int       in_n, in_i;            /* кадров в inb, следующий кадр */
static uint32_t  frac, step;            /* позиция между кадрами a и b, 16.16 */
static int       a_l, a_r, b_l, b_r;
static int       ramp_in, ramp_out;     /* кадров осталось */
static uint8_t   last_l, last_r;        /* последний выданный отсчёт */
static int       done;                  /* всё выдано, дальше только тишина */

/* Ячейки, которые игра загрузила сама (длинные команды, pcm_play.h). */
static struct { uint8_t *d; long len, cap; } slot[PCM_SLOTS];
static int       playing_slot;          /* чья память под источником, 0 -- ничья */
static int       seq;                   /* разряд PCM_ST_SEQ */
static int       ext_n;                 /* байт адреса блока ещё ждём (длинная команда) */
static uint16_t  ext_a;
static int       hurry;                 /* шагов с частым опросом: идёт длинная команда */

void pcm_init(const pcm_ops_t *o)
{
    ops = *o;
    st = S_IDLE; run = 0; err = 0; ready = 1; wblk = 0; rp = 0;
    src_on = 0; done = 1; last_l = last_r = 0;
    ext_n = 0; playing_slot = 0;
}

uint8_t pcm_state(void)
{
    return (uint8_t)((ready ? PCM_ST_READY : 0) | (err ? PCM_ST_ERR : 0)
                     | (st != S_IDLE ? PCM_ST_PLAY : 0) | (seq ? PCM_ST_SEQ : 0));
}

void pcm_volume(int v) { vol = v < 0 ? 0 : v > 16 ? 16 : v; }

static void src_close(void)
{
    if (src_on && src.close) src.close(&src);
    src_on = 0;
    playing_slot = 0;
}

/* следующий кадр источника в b; 0 -- источник кончился */
static int next_frame(void)
{
    if (in_i >= in_n) {
        int n = src.read(&src, inb, INBUF / 2);
        if (n <= 0 && loop && src.rewind && src.rewind(&src) == 0)
            n = src.read(&src, inb, INBUF / 2);
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

/* Один кадр выхода. 0 -- выдавать нечего (done). */
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
            frac -= 0x10000;
            if (!next_frame()) { src_close(); ramp_out = RAMP; break; }
        }
        *l = last_l = to_u8(vl); *r = last_r = to_u8(vr);
        return 1;
    }
    if (ramp_out) {
        *l = (uint8_t)((last_l * (ramp_out - 1)) / RAMP);
        *r = (uint8_t)((last_r * (ramp_out - 1)) / RAMP);
        if (--ramp_out == 0) done = 1;
        return 1;
    }
    return 0;
}

/* Блок кольца; 0 -- нечего класть. Хвост последнего блока -- нули. */
static int make_block(uint8_t *blk)
{
    int k;
    if (done) return 0;
    for (k = 0; k < PCM_BLK / 2; k++) {
        uint8_t l = 0, r = 0;
        if (done || !out_frame(&l, &r)) l = r = 0;
        blk[2 * k] = l; blk[2 * k + 1] = r;
    }
    return 1;
}

static void fill(int max)
{
    static uint8_t blk[PCM_BLK];
    int n = 0;
    while (n < max && ((wblk - rp) & (PCM_NBLK - 1)) < PCM_AHEAD) {
        if (!make_block(blk)) break;
        if (ops.ring_write(ops.ctx, wblk, blk) < 0) break;
        wblk = (wblk + 1) & (PCM_NBLK - 1);
        n++;
    }
    if (done && st == S_PLAY) st = S_DRAIN;
}

static void start(int rc, int lp);
static void ext_command(void);

void pcm_command(int cmd)
{
    cmd &= 0xFF;
    if (ext_n) {
        /* байт адреса блока параметров длинной команды: младший, старший */
        if (ext_n == 2) ext_a = (uint16_t)cmd;
        else            ext_a |= (uint16_t)(cmd << 8);
        if (--ext_n == 0) ext_command();
        return;
    }
    if (cmd == 0x00) {
        /* Стоп: дальше того блока, что играет, -- плавный выход от середины
         * и тишина. Что уже лежит впереди, не играется. */
        if (st == S_IDLE) { err = 0; return; }
        src_close();
        if (st == S_START) { st = S_IDLE; run = 0; done = 1; return; }
        ramp_in = 0; ramp_out = RAMP; last_l = last_r = 0x80; done = 0;
        wblk = (rp + 1) & (PCM_NBLK - 1);
        st = S_PLAY;
        fill(1);
        err = 0;
        return;
    }
    if (cmd == 0x80) { ext_n = 2; hurry = 200; return; }
    src_close();
    memset(&src, 0, sizeof src);
    start(ops.open(ops.ctx, cmd & 0x7F, &src), (cmd & 0x80) != 0);
}

/* Пустить открытый источник (rc -- как открылся). */
static void start(int rc, int lp)
{
    if (rc != 0 || src.rate <= 0 || (src.ch != 1 && src.ch != 2)) {
        if (src.close) src.close(&src);
        if (st != S_IDLE) pcm_command(0x00);
        err = 1;
        return;
    }
    err = 0; loop = lp;
    src_on = 1; in_n = in_i = 0;
    step = (uint32_t)(((uint64_t)src.rate << 16) / PCM_RATE);
    b_l = b_r = 0;
    if (!next_frame()) {
        src_close();
        if (st != S_IDLE) pcm_command(0x00);
        err = 1;
        return;
    }
    if (!next_frame()) { a_l = b_l; a_r = b_r; }
    frac = 0;
    ramp_in = RAMP; ramp_out = 0; done = 0;
    st = S_START; run = 0;              /* следующий обмен вернёт кольцо в начало */
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
    if (playing_slot == k) pcm_command(0x00);     /* его память под источником */
    free(slot[k].d);
    slot[k].d = 0; slot[k].len = slot[k].cap = 0;
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
        if (playing_slot == k) pcm_command(0x00);   /* realloc сдвинет память */
        d = realloc(slot[k].d, (size_t)cap);
        if (!d) return -1;
        slot[k].d = d; slot[k].cap = cap;
    }
    if (n && ops.mem_read(ops.ctx, a, slot[k].d + slot[k].len, n) < 0) return -1;
    slot[k].len = need;
    return 0;
}

static int le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* Блок параметров -- в ОЗУ «Вектора» по ext_a (зеркало в статике). */
static void ext_command(void)
{
    uint8_t b[6];
    int ok = 0, k;
    if (ops.mem_read(ops.ctx, ext_a, b, sizeof b) == 0) {
        k = b[1];
        switch (b[0]) {
        case PCM_OP_BEGIN:
            if (k >= 1 && k < PCM_SLOTS) { slot_free(k); ok = 1; }
            break;
        case PCM_OP_APPEND:
            ok = k >= 1 && k < PCM_SLOTS && slot_append(k, (uint16_t)le16(b + 2), (uint16_t)le16(b + 4)) == 0;
            break;
        case PCM_OP_PLAY:
            if (k >= 1 && k < PCM_SLOTS && slot[k].len > 0) {
                src_close();
                memset(&src, 0, sizeof src);
                start(ops.open_mem(ops.ctx, slot[k].d, slot[k].len, &src), b[2] & 1);
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
    else if (st == S_PLAY) fill(max);
    else if (st == S_DRAIN && rp == wblk) { st = S_IDLE; run = 0; }
    if (mail >= 0) { pcm_command(mail); hurry = 200; }
    if (hurry) { hurry--; return 2; }
    return st != S_IDLE;
}
