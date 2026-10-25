/* Модель канала для проверки программ «Вектора» (tools/test_cl.py,
 * tools/test_term.py) и для эмулятора: настоящий vchan.c и modem.c, а вместо
 * ПЛИС -- две очереди по 16 байт, как в hdl/vchan.v, и плата в памяти.
 * Собирается в разделяемую библиотеку и зовётся из питона. Эмулятор 8080
 * дёргает sim_in/sim_out на IN/OUT портов 28h-2Dh, а sim_esp -- опрос ESP. */

#include "vchan.h"
#include "board.h"
#include "combo.h"
#include "modem.h"

/* Палитра машины и регистр квазидиска 10h -- для ответа на DIAG. Стенды
 * берут заглушки, эмулятор (v06x, ветка combolite) ставит свои крюки. */
static int stub_palette(uint8_t pal[SCR_PAL_BYTES])
{
    int i;
    for (i = 0; i < SCR_PAL_BYTES; i++) pal[i] = (uint8_t)(0x10 + i);
    return 0;
}
static uint8_t stub_kvaz(void) { return 0x23; }
int     (*sim_palette)(uint8_t pal[SCR_PAL_BYTES]) = stub_palette;
uint8_t (*sim_kvaz)(void) = stub_kvaz;

int     board_palette(uint8_t pal[SCR_PAL_BYTES]) { return sim_palette(pal); }
uint8_t combo_diag(uint8_t sel) { (void)sel; return sim_kvaz(); }

#include <string.h>

/* ---------------------------------------------------------- плата */
static board_settings_t cur = BOARD_SETTINGS_DEFAULT;
int sim_saves;

int board_settings_valid(const board_settings_t *s)
{
    return !(s->dev_off & ~DEV_MASK) && s->fdc_rdy <= 2 && s->fdc_pc <= 0x1F
        && s->vga60 <= 1 && s->vi53_off <= 1 && s->boot <= BOOT_VVOD
        && s->fdd_cable <= 1 && s->autostart <= 1;
}
void    board_get(board_settings_t *out) { *out = cur; }
int     board_set(const board_settings_t *s)
{
    if (!board_settings_valid(s)) return -1;
    cur = *s; sim_saves++;
    return 0;
}
int     board_present(void)   { return 1; }
uint8_t board_features(void)  { return 0xBF; }
uint8_t board_features2(void) { return 0x0B; }

/* Поле настроек по номеру vchan.h, для проверок из питона. */
int sim_field(int id)
{
    switch (id) {
    case VF_DEV_OFF:   return cur.dev_off;
    case VF_FDC_RDY:   return cur.fdc_rdy;
    case VF_FDC_PC:    return cur.fdc_pc;
    case VF_VGA60:     return cur.vga60;
    case VF_VI53_OFF:  return cur.vi53_off;
    case VF_BOOT:      return cur.boot;
    case VF_FDD_CABLE: return cur.fdd_cable;
    default:           return cur.autostart;
    }
}

/* ---------------------------------------------------------- ПЛИС */
/* Как hdl/vchan.v: подмножество UART 16550 с базой 28h, очереди по CHAN_DEPTH
 * байт, ESP видит только «пусто». */
#define Q CHAN_DEPTH
static uint8_t v2e[16], e2v[16];
static int v2e_r, v2e_n, e2v_r, e2v_n, flushed, polled, dlab;
int sim_esp_on = 1;                 /* 0 -- ESP молчит, LSR = 00h */

static void flush(void) { v2e_r = v2e_n = e2v_r = e2v_n = 0; flushed = 1; }

int sim_in(int port)
{
    uint8_t b;
    if (port == 0x2D)               /* LSR: DR, THRE, TEMT */
        return polled ? ((v2e_n == 0 ? 0x40 : 0) | (v2e_n < Q ? 0x20 : 0) | (e2v_n ? 1 : 0)) : 0x00;
    if (port == 0x28 && !dlab) {
        if (!e2v_n) return 0;
        b = e2v[e2v_r]; e2v_r = (e2v_r + 1) & 15; e2v_n--;
        return b;
    }
    return 0xFF;                    /* прочее не отвечает: пустая шина */
}

void sim_out(int port, int val)
{
    if (port == 0x28 && !dlab && v2e_n < Q) { v2e[(v2e_r + v2e_n) & 15] = (uint8_t)val; v2e_n++; }
    if (port == 0x2A && (val & 6)) flush();
    if (port == 0x2B) dlab = (val >> 7) & 1;
}

int combo_chan_read(uint8_t *buf, int max, int *free, int *fl)
{
    int n = v2e_n, i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) { buf[i] = v2e[v2e_r]; v2e_r = (v2e_r + 1) & 15; v2e_n--; }
    *free = e2v_n ? 0 : Q; *fl = flushed; flushed = 0;
    return n;
}

void combo_chan_write(const uint8_t *buf, int n)
{
    int i;
    for (i = 0; i < n && e2v_n < Q; i++) { e2v[(e2v_r + e2v_n) & 15] = buf[i]; e2v_n++; }
}

void sim_esp(unsigned now_ms)
{
    if (!sim_esp_on) return;
    polled = 1;
    modem_poll(now_ms);
    vchan_poll(now_ms);
}

/* Модем -- до первого опроса. */
void sim_init(void) { modem_init(); }
