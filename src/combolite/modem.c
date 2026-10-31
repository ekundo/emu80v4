/* Wi-Fi модем: AT-команды и telnet. Описание -- modem.h. */

#include "modem.h"
#include "bbslist.h"
#include "fetch.h"
#include "netcompat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DIAL_TIMEOUT_MS   20000u
#define RING_MS           2000u        /* RING раз в две секунды, как у Hayes */
#define RING_GIVEUP_MS    30000u       /* не ответили за полминуты -- звонок сброшен */
#define LISTEN_RETRY_MS   5000u
#define GUARD_MS          1000u        /* тишина вокруг +++ */

/* ---------------------------------------------------------- трубы */
/* Каждая труба -- кольцо с одним писателем и одним читателем: на плате
 * modem_put и modem_get зовёт задача канала, а modem_poll -- задача модема.
 * Индексы растут без конца, размер -- степень двойки. */
#define RING 4096                       /* справка и список BBS -- больше 1 КБ разом */
typedef struct {
    uint8_t           b[RING];
    volatile uint32_t w, r;
} ring_t;

static ring_t from_vec;                 /* машина -> модем */
static ring_t to_vec;                   /* модем -> машина */
static uint8_t  net_out[512];           /* модем -> сеть, ещё не ушло */
static int      net_out_n;

static int ring_put(ring_t *q, uint8_t b)
{
    if (q->w - q->r >= RING) return 0;
    q->b[q->w % RING] = b;
    __sync_synchronize();
    q->w++;
    return 1;
}

static int ring_get(ring_t *q, uint8_t *b)
{
    if (q->w == q->r) return 0;
    *b = q->b[q->r % RING];
    __sync_synchronize();
    q->r++;
    return 1;
}

static int ring_room(const ring_t *q) { return RING - (int)(q->w - q->r); }

void modem_put(uint8_t b) { ring_put(&from_vec, b); }
int  modem_get(uint8_t *b) { return ring_get(&to_vec, b); }

static void say(const char *s)
{
    while (*s) ring_put(&to_vec, (uint8_t)*s++);
}

static void result(const char *word)
{
    say("\r\n"); say(word); say("\r\n");
}

/* ---------------------------------------------------------- состояние */
enum { M_CMD, M_DIAL, M_ONLINE };

static int      mode = M_CMD;
static int      online_cmd;             /* соединение есть, но мы в командах (+++) */
static int      echo = 1;
static int      sock = -1;
static uint32_t dial_ms;
static char     line[80];               /* строка команды */
static int      line_n;
static int      cmd_esc;                /* ESC-последовательность в строке команды: мимо */
static char     last_cmd[80];           /* для A/ */
static char     last_dial[80];          /* для ATDL */
/* Вид соединения -- по первому байту от сервера: telnet начинает с IAC, IRC и
 * прочий сырой TCP -- с текста. Сервер молчит CONN_WAIT_MS -- сырой: IRC ждёт
 * NICK и USER, telnet-BBS торгуется сразу. Пока вид не ясен, байты от машины
 * ждут: telnet и сырой TCP шлют их по-разному (IAC IAC, CR NUL). */
#define CONN_WAIT_MS 1000u
enum { C_UNKNOWN, C_TELNET, C_RAW, C_GOPHER, C_TLS };
static int      conn;
static uint32_t conn_ms;

/* +++ в соединении */
static uint32_t last_in_ms;             /* последний байт от машины */
static int      plus_n;
static uint32_t plus_ms;

/* Мост «Gopher поверх HTTPS»: соединение без сокета, байты -- из fetch.h. */
static int      gopher;                 /* 0 -- нет, 1 -- копим строку выбора, 2 -- качаем */
static char     g_host[64];
static char     g_sel[256];
static int      g_sel_n;
static int      g_gem;                  /* мост Gemini (порт 1965), а не Gopher */

/* TLS-туннель (порт 443): соединение без сокета, байты -- через fetch.h. */
static int      tls_on;                 /* 0 -- нет, 1 -- соединяемся, 2 -- на линии */

/* Входящие (doc/MODEM.md): слушаем порт AT$SP, звонок -- RING раз в две
 * секунды, ATA отвечает, ATS0=n -- ответить самим на n-м звонке. По умолчанию
 * порт закрыт: иначе «Вектор» виден всей сети. Занята линия -- входящему
 * отказ. */
static unsigned listen_port;
static int      lsock = -1;
static uint32_t lsock_try_ms;
static int      rsock = -1;             /* звонит, ещё не ответили */
static int      ring_n;
static uint32_t ring_ms, ring_start_ms;
static int      s0;                     /* ATS0: на каком звонке отвечать, 0 -- не самим */

static int line_up(void) { return sock >= 0 || gopher || tls_on; }

int modem_online(void) { return line_up() && mode == M_ONLINE; }
int modem_line(void) { return line_up(); }

static void hang_up(void)
{
    if (gopher) fetch_close();
    gopher = 0;
    if (tls_on) tls_close();
    tls_on = 0;
    if (sock >= 0) net_close(sock);
    sock = -1;
    net_out_n = 0;
    online_cmd = 0;
    mode = M_CMD;
}

static void listen_off(void)
{
    if (lsock >= 0) net_close(lsock);
    lsock = -1;
    if (rsock >= 0) net_close(rsock);
    rsock = -1;
}

void modem_init(void)
{
    hang_up();
    listen_off();
    listen_port = 0;
    s0 = 0;
    echo = 1;
    line_n = 0;
    cmd_esc = 0;
    plus_n = 0;
}

/* ---------------------------------------------------------- telnet */
#define T_IAC   255
#define T_DONT  254
#define T_DO    253
#define T_WONT  252
#define T_WILL  251
#define T_SB    250
#define T_SE    240
#define O_BINARY 0
#define O_ECHO   1
#define O_SGA    3
#define O_TTYPE  24
#define O_NAWS   31

static uint8_t us_on[32], him_on[32];    /* по разряду на опцию */
static int echo_esc;                    /* местное эхо: 0 -- текст, 1 -- после ESC, 2 -- внутри ESC [ */
static int  bit(const uint8_t *m, int o) { return (m[o >> 3] >> (o & 7)) & 1; }
static void setb(uint8_t *m, int o, int v)
{
    if (v) m[o >> 3] |= (uint8_t)(1 << (o & 7));
    else   m[o >> 3] &= (uint8_t)~(1 << (o & 7));
}

static void net_raw(uint8_t b)
{
    if (net_out_n < (int)sizeof net_out) net_out[net_out_n++] = b;
}

static void cmd3(uint8_t verb, uint8_t opt)
{
    net_raw(T_IAC); net_raw(verb); net_raw(opt);
}

static void send_naws(void)
{
    static const uint8_t sb[] = { T_IAC, T_SB, O_NAWS, 0, MODEM_COLS, 0, MODEM_ROWS,
                                  T_IAC, T_SE };
    size_t i;
    for (i = 0; i < sizeof sb; i++) net_raw(sb[i]);
}

static void send_ttype(void)
{
    static const char name[] = "ANSI";
    size_t i;
    net_raw(T_IAC); net_raw(T_SB); net_raw(O_TTYPE); net_raw(0);   /* IS */
    for (i = 0; i < sizeof name - 1; i++) net_raw((uint8_t)name[i]);
    net_raw(T_IAC); net_raw(T_SE);
}

/* BINARY -- в обе стороны: без него сервер может портить CR в файлах (XMODEM). */
static int we_can(int o)  { return o == O_SGA || o == O_TTYPE || o == O_NAWS || o == O_BINARY; }
static int he_may(int o)  { return o == O_ECHO || o == O_SGA || o == O_BINARY; }

/* Ответ меняет состояние и уходит только при смене: так вежливые стороны
 * не зацикливаются на подтверждениях (RFC 854, раздел о согласовании). */
static void tn_verb(uint8_t verb, uint8_t o)
{
    switch (verb) {
    case T_DO:
        if (we_can(o)) {
            if (!bit(us_on, o)) { setb(us_on, o, 1); cmd3(T_WILL, o); }
            if (o == O_NAWS) send_naws();
        } else cmd3(T_WONT, o);
        break;
    case T_DONT:
        if (bit(us_on, o)) { setb(us_on, o, 0); cmd3(T_WONT, o); }
        break;
    case T_WILL:
        if (he_may(o)) {
            if (!bit(him_on, o)) { setb(him_on, o, 1); cmd3(T_DO, o); }
        } else cmd3(T_DONT, o);
        break;
    default:   /* WONT */
        if (bit(him_on, o)) { setb(him_on, o, 0); cmd3(T_DONT, o); }
        break;
    }
}

static void binary(void);

enum { TS_DATA, TS_IAC, TS_VERB, TS_SB, TS_SB_IAC, TS_CR };
static int     ts = TS_DATA;
static uint8_t ts_verb, sb_opt;
static int     sb_n, sb_send;

static void tn_reset(void)
{
    conn = C_UNKNOWN;
    echo_esc = 0;
    memset(us_on, 0, sizeof us_on);
    memset(him_on, 0, sizeof him_on);
    ts = TS_DATA;
}

/* Байт из сети. Команды telnet вырезаются, CR NUL становится CR. */
static void tn_in(uint8_t b)
{
    if (conn == C_UNKNOWN) {
        conn = b == T_IAC ? C_TELNET : C_RAW;
        if (conn == C_TELNET) binary();
    }
    if (conn == C_RAW) { ring_put(&to_vec, b); return; }
    switch (ts) {
    case TS_DATA:
        if (b == T_IAC) ts = TS_IAC;
        else { ring_put(&to_vec, b); if (b == '\r' && !bit(him_on, O_BINARY)) ts = TS_CR; }
        break;
    case TS_CR:
        ts = TS_DATA;
        if (b == 0 && !bit(him_on, O_BINARY)) break;     /* в BINARY NUL после CR -- данные */
        tn_in(b);
        break;
    case TS_IAC:
        if (b == T_IAC) { ring_put(&to_vec, b); ts = TS_DATA; }
        else if (b >= T_WILL) { ts_verb = b; ts = TS_VERB; }
        else if (b == T_SB) { ts = TS_SB; sb_n = 0; sb_send = 0; }
        else ts = TS_DATA;            /* NOP, GA и прочие одиночные */
        break;
    case TS_VERB:
        tn_verb(ts_verb, b);
        ts = TS_DATA;
        break;
    case TS_SB:
        if (b == T_IAC) { ts = TS_SB_IAC; break; }
        if (sb_n == 0) sb_opt = b;
        else if (sb_n == 1 && sb_opt == O_TTYPE && b == 1) sb_send = 1;   /* SEND */
        sb_n++;
        break;
    default:   /* TS_SB_IAC */
        if (b == T_SE) {
            if (sb_send) send_ttype();
            ts = TS_DATA;
        } else ts = TS_SB;            /* IAC IAC внутри SB -- данные */
        break;
    }
}

/* Попросить BINARY в обе стороны, как только видно, что это telnet: файлы
 * (XMODEM) идут через модем как есть, а модем о них не знает. Биты ставятся
 * сразу: ответ согласия тогда не повторяет просьбу, а отказ их снимет. */
static void binary(void)
{
    if (!bit(us_on, O_BINARY)) { setb(us_on, O_BINARY, 1); cmd3(T_WILL, O_BINARY); }
    if (!bit(him_on, O_BINARY)) { setb(him_on, O_BINARY, 1); cmd3(T_DO, O_BINARY); }
}

/* Местное эхо, как у telnet-клиента: пока сервер не обещал эхо (WILL ECHO),
 * набранное машине показывает сам модем -- иначе на MUD вроде bylins.su,
 * которые эха не шлют, не видно, что набираешь. Сервер включил своё (перед
 * паролем, чтобы его скрыть) -- модем молчит. ВК -- переводом строки, ЗБ --
 * стиранием знака; последовательности ESC (стрелки, F1-F5) не показываются. */
static void local_echo(uint8_t b)
{
    if (echo_esc == 1) { echo_esc = (b == '[' || b == 'O') ? 2 : 0; return; }
    if (echo_esc == 2) { if (b >= 0x40 && b <= 0x7E) echo_esc = 0; return; }
    if (b == 27) { echo_esc = 1; return; }
    if (b == '\r') { ring_put(&to_vec, '\r'); ring_put(&to_vec, '\n'); return; }
    if (b == 8 || b == 127) { ring_put(&to_vec, 8); ring_put(&to_vec, ' '); ring_put(&to_vec, 8); return; }
    if (b >= 32) ring_put(&to_vec, b);
}

/* Байт от машины в сеть. */
static void gopher_char(uint8_t b);

static void tn_out(uint8_t b)
{
    if (conn == C_GOPHER) { gopher_char(b); return; }
    if (conn == C_RAW || conn == C_TLS) { net_raw(b); return; }
    if (conn == C_TELNET && !bit(him_on, O_ECHO)) local_echo(b);
    if (b == T_IAC) { net_raw(T_IAC); net_raw(T_IAC); return; }
    net_raw(b);
    if (b == '\r' && !bit(us_on, O_BINARY)) net_raw(0);
}

/* ---------------------------------------------------------- мост Gopher
 * GitHub Pages отдаёт только HTTPS, а GOPHER.COM ходит сырым TCP на порт 70.
 * Набор хоста *.github.io на порт 70 соединяет не с сетью, а с мостом:
 * CONNECT сразу, строка выбора -- до CR или LF (запрос поиска после ТАБ --
 * мимо: там статика), файл -- https://хост/путь (пустой путь или с «/» на
 * конце -- index.gph), его байты -- как ответ сервера Gopher, конец --
 * NO CARRIER, как при закрытии TCP. Не вышло -- меню из одной строки-ошибки.
 * Скачивание -- fetch.h: на плате esp_http_client, на маке curl. */
static int gopher_host(const char *host, const char *port)
{
    static const char tail[] = ".github.io";
    size_t hl = strlen(host), tl = sizeof tail - 1;
    return !strcmp(port, "70") && hl > tl && !strcasecmp(host + hl - tl, tail);
}

static void gopher_end(const char *error)
{
    if (error) {
        say("3"); say(error); say("\t\terror.host\t1\r\n.\r\n");
    }
    hang_up();
    result("NO CARRIER");
}

/* Строка выбора пришла: путь -> адрес. В адресе остаются только буквы, цифры
 * и -._~/%, прочее -- процентами: так он годится и в оболочку (curl). */
static void gopher_start(void)
{
    static const char hex[] = "0123456789ABCDEF";
    char url[512];
    size_t n;
    int i;

    n = (size_t)snprintf(url, sizeof url, "https://%s", g_host);
    if (g_sel[0] != '/') url[n++] = '/';
    for (i = 0; i < g_sel_n && g_sel[i] != '\t' && n < sizeof url - 16; i++) {
        uint8_t c = (uint8_t)g_sel[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            strchr("-._~/%", c)) url[n++] = (char)c;
        else { url[n++] = '%'; url[n++] = hex[c >> 4]; url[n++] = hex[c & 15]; }
    }
    if (url[n - 1] == '/') { memcpy(url + n, "index.gph", 9); n += 9; }
    url[n] = 0;
    if (fetch_open(url) != 0) { gopher_end("Bridge busy, try again"); return; }
    gopher = 2;
}

static void gem_start(void);

static void gopher_char(uint8_t b)
{
    if (gopher != 1) return;            /* строка выбора уже ушла: остальное мимо */
    if (b == '\r' || b == '\n') { if (g_gem) gem_start(); else gopher_start(); return; }
    if (g_sel_n < (int)sizeof g_sel - 1) g_sel[g_sel_n++] = (char)b;
}

/* Скачанное -- машине, сколько влезет. */
static void gopher_pump(void)
{
    uint8_t buf[256];
    int n = ring_room(&to_vec) - 16, i;

    if (n > (int)sizeof buf) n = (int)sizeof buf;
    if (n <= 0) return;
    n = fetch_read(buf, n);
    if (n > 0) { for (i = 0; i < n; i++) ring_put(&to_vec, buf[i]); return; }
    if (n == FETCH_DONE) gopher_end(0);
    else if (n == FETCH_FAIL) gopher_end(fetch_status() ? "Not found" : "Cannot reach the site");
}

/* ---------------------------------------------------------- мост Gemini
 * Gemini (geminiprotocol.net) -- TLS на порт 1965, запрос -- адрес строкой,
 * ответ -- строка состояния «NN мета» и тело. GOPHER.COM говорит по-гоферски,
 * поэтому набор любого хоста на порт 1965 соединяет с мостом, а он переводит
 * страницу Gemini (text/gemini) в меню Gopher: текст -- строки «i», ссылки --
 * пункты, просьба ввести строку (состояние 1x) -- пункт поиска «7». Абзацы
 * переносятся по словам на GEM_W колонок: в меню GOPHER.COM строка меню --
 * одна строка экрана, и 8 колонок слева заняты номером и меткой пункта.
 *
 * Строка выбора -- путь (адрес -- gemini://хост/путь) или целый адрес
 * gemini://..., когда порт не 1965; после ТАБ -- запрос поиска, он заменяет
 * «?...» адреса. Впереди «!» -- отдать тело как есть (файлы и текст для
 * пунктов TXT), «+» -- принять новый сертификат капсулы. За перенаправлениями
 * (3x) мост идёт сам. */
#define GEM_W     72
#define GEM_LINE  1024                  /* строка страницы длиннее -- обрезается */
#define GEM_URL   1024                  /* адрес Gemini не длиннее (спецификация) */
#define GEM_ROOM  1600                  /* место в трубе под строку меню со всеми переносами */
#define GEM_HOPS  5
#define SEL_MAX   160                   /* строка выбора у GOPHER.COM (SELMAX) */
#define HOST_MAX  47                    /* хост у GOPHER.COM (HOSTMAX без нуля) */

enum { GS_HEAD, GS_PAGE, GS_TEXT, GS_RAW, GS_STOP, GS_REDIR };
static char    gm_url[GEM_URL + 1];     /* адрес страницы: от него -- ссылки */
static int     gm_raw, gm_hops, gm_st, gm_pre;
static char    gm_ln[GEM_LINE + 1];     /* строка ответа */
static int     gm_n;
static uint8_t gm_buf[256];
static int     gm_bn, gm_bi;

static int is_alnum(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

static int u8cols(const char *s, int n)
{
    int c = 0, i;
    for (i = 0; i < n; i++) if (((uint8_t)s[i] & 0xC0) != 0x80) c++;
    return c;
}

static void put_n(const char *s, int n)
{
    while (n-- > 0) ring_put(&to_vec, (uint8_t)*s++);
}

/* gemini://хост[:порт]/путь -> хост и порт; path -- путь с запросом. */
static int gem_split(const char *url, char *host, size_t hmax, int *port, const char **path)
{
    const char *h, *e, *c;
    size_t hl;

    if (strncasecmp(url, "gemini://", 9)) return -1;
    h = url + 9;
    e = h + strcspn(h, "/?#");
    c = memchr(h, ':', (size_t)(e - h));
    hl = (size_t)((c ? c : e) - h);
    if (hl == 0 || hl >= hmax) return -1;
    memcpy(host, h, hl);
    host[hl] = 0;
    *port = c ? atoi(c + 1) : 1965;
    if (*port <= 0 || *port > 65535) return -1;
    *path = e;
    return 0;
}

/* Адрес -- в запрос: без «#...», прочее, кроме букв, цифр и знаков адреса,
 * -- процентами (UTF-8 тоже). Кавычек и пробелов не остаётся: адрес идёт и в
 * командную строку (fetch_curl.c). Пустой путь -- «/». */
static void gem_safe(const char *in, char *out, size_t max)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    int slashes = 0;

    for (; *in && *in != '#' && n + 4 < max; in++) {
        uint8_t c = (uint8_t)*in;
        if (slashes == 2 && (c == '?' || !c)) { out[n++] = '/'; slashes = 3; }  /* gemini://хост?... */
        if (c == '/' && slashes < 3) slashes++;
        if (is_alnum(c) || strchr("-._~/%?=&:@+,;!$*()[]", c)) out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    if (slashes == 2 && n + 1 < max) out[n++] = '/';
    out[n] = 0;
}

/* Убрать «.» и «..» из пути (RFC 3986, 5.2.4). Буферы -- не на стеке:
 * модем один, а стек его задачи -- 8 КБ. */
static void gem_dots(char *url)
{
    static char in[GEM_URL + 1], rest[GEM_URL + 1];
    char *p = strstr(url, "://"), *o, *i;
    size_t pl;

    if (!p) return;
    p += 3;
    p += strcspn(p, "/?#");
    if (*p != '/') return;
    pl = strcspn(p, "?#");
    snprintf(in, sizeof in, "%.*s", (int)pl, p);
    snprintf(rest, sizeof rest, "%s", p + pl);
    o = p;
    i = in;
    while (*i) {
        if (!strncmp(i, "/./", 3)) i += 2;
        else if (!strcmp(i, "/.")) i[1] = 0;
        else if (!strncmp(i, "/../", 4) || !strcmp(i, "/..")) {
            if (i[3]) i += 3; else i[1] = 0;
            while (o > p && *--o != '/') ;      /* последний кусок -- прочь вместе с «/» */
        } else {
            do *o++ = *i++; while (*i && *i != '/');
        }
    }
    strcpy(o, rest);
}

/* Ссылка ref со страницы base -> адрес (RFC 3986, 5.2). */
static void gem_join(const char *base, const char *ref, char *out, size_t max)
{
    const char *p = ref, *auth, *a_end, *q;

    while (is_alnum(*p) || *p == '+' || *p == '.' || *p == '-') p++;
    if (p > ref && *p == ':' && !(ref[0] >= '0' && ref[0] <= '9')) {
        snprintf(out, max, "%s", ref);          /* своя схема */
        gem_dots(out);
        return;
    }
    auth = strstr(base, "://");
    auth = auth ? auth + 3 : base;
    a_end = auth + strcspn(auth, "/?#");
    if (ref[0] == '/' && ref[1] == '/') snprintf(out, max, "%.*s%s", (int)(auth - 2 - base), base, ref);
    else if (ref[0] == '/') snprintf(out, max, "%.*s%s", (int)(a_end - base), base, ref);
    else if (ref[0] == '?') snprintf(out, max, "%.*s%s", (int)strcspn(base, "?#"), base, ref);
    else if (!ref[0] || ref[0] == '#') snprintf(out, max, "%.*s", (int)strcspn(base, "#"), base);
    else {
        q = base + strcspn(base, "?#");
        while (q > a_end && q[-1] != '/') q--;
        if (q == a_end) snprintf(out, max, "%.*s/%s", (int)(a_end - base), base, ref);
        else snprintf(out, max, "%.*s%s", (int)(q - base), base, ref);
    }
    gem_dots(out);
}

/* Вид пункта по расширению: файлы -- на диск, текст -- как есть, прочее --
 * страница. */
static char gem_kind(const char *path)
{
    static const char *const kinds[] = {
        "0txt", "0md", "0asm", "0bas", "0c", "0h", "0inc", "0log", "0csv", "0diz", "0nfo",
        "Ipng", "Ijpg", "Ijpeg", "Igif", "Ibmp", "Iwebp",
        "smp3", "swav", "sogg", "sflac", "smod", "sxm", "spt3", "say",
        "9zip", "9rom", "9fdd", "9com", "9bin", "97z", "9gz", "9tar", "9pdf", "9exe",
        "9img", "9dsk", "9edd", "9r0m", "9vec", "9cas", "9lzh", "9arj", "9rar", 0 };
    size_t n = strcspn(path, "?"), i;
    const char *e = 0, *p;
    char ext[8];

    for (p = path; p < path + n; p++) {
        if (*p == '/') e = 0;
        else if (*p == '.') e = p + 1;
    }
    if (!e || path + n - e >= (int)sizeof ext || path + n == e) return '1';
    for (i = 0; e + i < path + n; i++) ext[i] = (char)((e[i] >= 'A' && e[i] <= 'Z') ? e[i] + 32 : e[i]);
    ext[i] = 0;
    for (i = 0; kinds[i]; i++) if (!strcmp(kinds[i] + 1, ext)) return kinds[i][0];
    return '1';
}

/* Текст s (n байт UTF-8) -- строками меню по GEM_W колонок, по словам.
 * Первая -- вида type с приставкой pre1 и хвостом tail (ТАБ, путь, хост,
 * порт у пункта), остальные -- «i» с pre2. */
static void gem_wrap(char type, const char *pre1, const char *pre2, const char *s, int n, const char *tail)
{
    const char *pre = pre1;

    for (;;) {
        int w = GEM_W - u8cols(pre, (int)strlen(pre)), cols = 0, i = 0, brk = 0, cut;

        while (i < n && cols < w) {
            if (s[i] == ' ') brk = i;
            i++;
            while (i < n && ((uint8_t)s[i] & 0xC0) == 0x80) i++;
            cols++;
        }
        cut = (i >= n || s[i] == ' ' || brk == 0) ? i : brk;
        ring_put(&to_vec, (uint8_t)type);
        say(pre);
        put_n(s, cut);
        if (tail) say(tail);
        say("\r\n");
        type = 'i';
        tail = 0;
        s += cut;
        n -= cut;
        while (n > 0 && *s == ' ') { s++; n--; }
        if (n <= 0) return;
        pre = pre2;
    }
}

/* Строка ошибки в меню; дальше ответ не читаем. */
static void gem_err(const char *a, const char *b)
{
    say("3"); say(a); if (b) say(b); say("\t\terror.host\t1\r\n");
    gm_st = GS_STOP;
}

/* Путь пункта для этой же страницы: путь или, если порт не 1965, адрес. */
static int gem_self(const char *url, const char *pfx, char *sel, size_t smax, char *host, size_t hmax)
{
    const char *path;
    int port;

    if (gem_split(url, host, hmax, &port, &path)) return -1;
    return snprintf(sel, smax, "%s%s", pfx, port == 1965 ? path : url) >= (int)smax || !sel[0] ? -1 : 0;
}

static int hexv(int c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; }

/* Ссылка «=> адрес [подпись]». */
static void gem_link(char *s)
{
    static char url[GEM_URL + 1], safe[GEM_URL + 1];
    char host[64], sel[SEL_MAX + 2], tail[SEL_MAX + 80];
    const char *path;
    char *ref, *label, type;
    int port;

    while (*s == ' ') s++;
    ref = s;
    s += strcspn(s, " ");
    if (*s) { *s++ = 0; while (*s == ' ') s++; }
    label = *s ? s : ref;
    if (!*ref) return;
    gem_join(gm_url, ref, url, sizeof url);
    if (!strncasecmp(url, "gemini://", 9)) {
        gem_safe(url, safe, sizeof safe);
        if (!gem_split(safe, host, sizeof host, &port, &path) && strlen(host) <= HOST_MAX) {
            type = gem_kind(path);
            if (snprintf(sel, sizeof sel, "%s%s", type == '1' ? "" : "!", port == 1965 ? path : safe) <= SEL_MAX) {
                snprintf(tail, sizeof tail, "\t%s\t%s\t1965", sel, host);
                gem_wrap(type, "", "", label, (int)strlen(label), tail);
                return;
            }
        }
    } else if (!strncasecmp(url, "gopher://", 9)) {
        /* gopher://хост[:порт][/вид[путь]], путь -- без процентов */
        char *h = url + 9, *e = h + strcspn(h, "/:"), *p;
        size_t n = 0;
        port = 70;
        if (*e == ':') { *e++ = 0; port = atoi(e); e += strcspn(e, "/"); }
        p = e;
        if (*p == '/') *p++ = 0; else *p = 0;
        type = *p ? *p++ : '1';
        if (strlen(h) <= HOST_MAX && *h) {
            for (; *p && n < SEL_MAX; p++) {
                if (*p == '%' && p[1] && p[2]) { sel[n++] = (char)(hexv(p[1]) * 16 + hexv(p[2])); p += 2; }
                else sel[n++] = *p;
            }
            sel[n] = 0;
            snprintf(tail, sizeof tail, "\t%s\t%.*s\t%d", sel, HOST_MAX, h, port);
            gem_wrap(type, "", "", label, (int)strlen(label), tail);
            return;
        }
    } else {
        snprintf(tail, sizeof tail, "\tURL:%.*s\terror.host\t1", SEL_MAX - 4, url);
        gem_wrap('h', "", "", label, (int)strlen(label), tail);
        return;
    }
    gem_wrap('i', "", "", label, (int)strlen(label), 0);     /* в GOPHER.COM не уместить */
}

/* Строка страницы gm_ln (gm_n). */
static void gem_line(void)
{
    char *s = gm_ln;
    int n = gm_n;

    if (n == GEM_LINE) {                /* обрезана: недописанный знак UTF-8 -- прочь */
        int k = n;
        while (k > 0 && ((uint8_t)s[k - 1] & 0xC0) == 0x80) k--;
        if (k > 0 && ((uint8_t)s[k - 1] & 0xC0) == 0xC0) n = k - 1;
    }
    s[n] = 0;
    if (gm_st == GS_TEXT) { gem_wrap('i', "", "", s, n, 0); return; }
    if (!strncmp(s, "```", 3)) { gm_pre = !gm_pre; return; }
    if (gm_pre) {                                               /* как есть, лишнее справа -- прочь */
        int i = 0, cols = 0;
        while (i < n && cols < GEM_W) { i++; while (i < n && ((uint8_t)s[i] & 0xC0) == 0x80) i++; cols++; }
        say("i"); put_n(s, i); say("\r\n");
        return;
    }
    if (s[0] == '=' && s[1] == '>') { gem_link(s + 2); return; }
    if (s[0] == '*' && s[1] == ' ') { gem_wrap('i', "\xE2\x80\xA2 ", "  ", s + 2, n - 2, 0); return; }
    if (s[0] == '>') { s++; n--; if (*s == ' ') { s++; n--; } gem_wrap('i', "> ", "> ", s, n, 0); return; }
    gem_wrap('i', "", "", s, n, 0);
}

/* Строка состояния «NN мета». */
static void gem_head(char *h)
{
    char sel[SEL_MAX + 2], host[64], tail[SEL_MAX + 80], *meta = h + 2;

    while (*meta == ' ') meta++;
    if (h[0] < '1' || h[0] > '6' || h[1] < '0' || h[1] > '9') { gem_err("Not a Gemini answer", 0); return; }
    switch (h[0]) {
    case '1':                           /* просит строку: пункт поиска */
        if (gem_self(gm_url, "", sel, sizeof sel, host, sizeof host)) { gem_err("Address too long", 0); return; }
        sel[strcspn(sel, "?")] = 0;
        snprintf(tail, sizeof tail, "\t%s\t%s\t1965", sel, host);
        gem_wrap('7', "", "", *meta ? meta : "Input", (int)strlen(*meta ? meta : "Input"), tail);
        gm_st = GS_STOP;
        return;
    case '2':
        if (gm_raw) gm_st = GS_RAW;
        else if (!*meta || !strncasecmp(meta, "text/gemini", 11)) gm_st = GS_PAGE;
        else if (!strncasecmp(meta, "text/", 5)) gm_st = GS_TEXT;
        else {                          /* файл: пункт «сохранить» */
            char *slash;
            if (gem_self(gm_url, "!", sel, sizeof sel, host, sizeof host)) { gem_err("Address too long", 0); return; }
            say("i"); say(meta); say("\r\n");
            slash = strrchr(sel, '/');
            snprintf(tail, sizeof tail, "\t%s\t%s\t1965", sel, host);
            say("9"); say(slash && slash[1] ? slash + 1 : "file"); say(tail); say("\r\n");
            gm_st = GS_STOP;
        }
        return;
    case '3':
        snprintf(gm_ln + GEM_LINE / 2, GEM_LINE / 2, "%s", meta);      /* куда -- до перехода */
        gm_st = GS_REDIR;
        return;
    case '6':
        gem_err("Capsule wants a client certificate: ", meta);
        return;
    default:
        gem_err(h, 0);
        return;
    }
}

static void gem_byte(uint8_t b)
{
    if (b == '\r') return;
    if (b == '\n') {
        gm_ln[gm_n] = 0;
        if (gm_st == GS_HEAD) gem_head(gm_ln); else gem_line();
        gm_n = 0;
        return;
    }
    if (gm_st == GS_HEAD && gm_n >= GEM_LINE / 2 - 1) { gem_err("Not a Gemini answer", 0); return; }
    if (b == '\t') b = ' ';
    if (gm_n < GEM_LINE) gm_ln[gm_n++] = (char)b;
}

static void gem_go(const char *url, int trust_new)
{
    char host[64];
    const char *path;
    int port;

    gem_safe(url, gm_url, sizeof gm_url);
    gm_st = GS_HEAD;
    gm_n = gm_pre = gm_bn = gm_bi = 0;
    gopher = 2;
    if (gem_split(gm_url, host, sizeof host, &port, &path)) { gem_err("Bad Gemini address", 0); return; }
    if (fetch_gemini(host, port, gm_url, trust_new) != 0) gem_err("Gemini bridge unavailable, try again", 0);
}

/* Строка выбора пришла: путь или адрес, запрос после ТАБ. */
static void gem_start(void)
{
    static char url[GEM_URL + 1];
    char *s = g_sel, *q;
    int trust = 0;

    g_sel[g_sel_n] = 0;
    gm_raw = 0;
    gm_hops = 0;
    for (;; s++) {
        if (*s == '!') gm_raw = 1;
        else if (*s == '+') trust = 1;
        else break;
    }
    q = strchr(s, '\t');
    if (q) *q++ = 0;
    if (!strncasecmp(s, "gemini://", 9)) snprintf(url, sizeof url, "%s", s);
    else snprintf(url, sizeof url, "gemini://%s%s%s", g_host, *s == '/' ? "" : "/", s);
    if (q) {                            /* запрос -- процентами весь */
        static const char hex[] = "0123456789ABCDEF";
        size_t n = strcspn(url, "?");
        url[n++] = '?';
        for (; *q && n + 4 < sizeof url; q++) {
            uint8_t c = (uint8_t)*q;
            if (is_alnum(c) || strchr("-._~", c)) url[n++] = (char)c;
            else { url[n++] = '%'; url[n++] = hex[c >> 4]; url[n++] = hex[c & 15]; }
        }
        url[n] = 0;
    }
    gem_go(url, trust);
}

static void gem_end(void)
{
    if ((gm_st == GS_PAGE || gm_st == GS_TEXT) && gm_n) gem_line();
    if (gm_st == GS_HEAD) gem_err("Empty answer", 0);
    if (gm_st != GS_RAW) say(".\r\n");
    hang_up();
    result("NO CARRIER");
}

static void gem_failed(void)
{
    char sel[SEL_MAX + 2], host[64], tail[SEL_MAX + 80];

    if (fetch_status() != FETCH_CERT) { gem_err("Cannot reach the capsule", 0); gem_end(); return; }
    if (gem_self(gm_url, "+", sel, sizeof sel, host, sizeof host)) { gem_err("Certificate changed", 0); gem_end(); return; }
    gem_err("Certificate of the capsule changed", 0);
    say("iThe capsule may have a new one, or someone is in between.\r\n");
    snprintf(tail, sizeof tail, "\t%s\t%s\t1965", sel, host);
    say("1Trust the new certificate"); say(tail); say("\r\n");
    gem_end();
}

static void gem_redirect(void)
{
    static char url[GEM_URL + 1];

    fetch_close();
    gem_join(gm_url, gm_ln + GEM_LINE / 2, url, sizeof url);
    if (++gm_hops > GEM_HOPS) { gem_err("Too many redirects", 0); gem_end(); return; }
    if (strncasecmp(url, "gemini://", 9)) { gem_err("Redirect out of Gemini: ", url); gem_end(); return; }
    gem_go(url, 0);
    if (gm_st == GS_STOP) gem_end();
}

/* Ответ капсулы -- машине: в меню, пока в трубе есть место под строку
 * меню со всеми переносами, как есть -- сколько влезет. */
static void gem_pump(void)
{
    for (;;) {
        if (gm_st == GS_STOP) { gem_end(); return; }
        if (gm_st == GS_REDIR) { gem_redirect(); return; }
        if (gm_bi == gm_bn) {
            int n;
            if (ring_room(&to_vec) < GEM_ROOM) return;
            n = fetch_read(gm_buf, sizeof gm_buf);
            if (n == 0) return;
            if (n == FETCH_DONE) { gem_end(); return; }
            if (n == FETCH_FAIL) { gem_failed(); return; }
            gm_bn = n;
            gm_bi = 0;
        }
        while (gm_bi < gm_bn && gm_st == GS_RAW) {
            if (ring_room(&to_vec) <= 16) return;
            ring_put(&to_vec, gm_buf[gm_bi++]);
        }
        while (gm_bi < gm_bn && gm_st != GS_RAW && gm_st != GS_STOP && gm_st != GS_REDIR) {
            if (ring_room(&to_vec) < GEM_ROOM) return;
            gem_byte(gm_buf[gm_bi++]);
        }
    }
}

/* ---------------------------------------------------------- TLS-туннель
 * Набор на порт 443 -- TLS к этому хосту, CONNECT -- когда соединились.
 * Дальше байты как у сырого TCP: от машины -- через net_out, к машине -- сколько
 * влезет. Сервер закрыл -- NO CARRIER. */
static void tls_pump(uint32_t now_ms)
{
    uint8_t buf[256];
    int n, i;

    if (tls_on == 1) {
        n = tls_ready();
        if (n < 0 || (n == 0 && now_ms - dial_ms >= DIAL_TIMEOUT_MS)) { hang_up(); result("NO ANSWER"); return; }
        if (n == 0) return;
        tls_on = 2;
        mode = M_ONLINE;
        last_in_ms = now_ms;
        result("CONNECT");
    }
    if (net_out_n) {
        n = tls_write(net_out, net_out_n);
        if (n > 0) {
            memmove(net_out, net_out + n, (size_t)(net_out_n - n));
            net_out_n -= n;
        }
    }
    n = ring_room(&to_vec) - 16;
    if (n > (int)sizeof buf) n = (int)sizeof buf;
    if (n <= 0) return;
    n = tls_read(buf, n);
    if (n > 0) { for (i = 0; i < n; i++) ring_put(&to_vec, buf[i]); return; }
    if (n == FETCH_DONE || n == FETCH_FAIL) { hang_up(); result("NO CARRIER"); }
}

/* ---------------------------------------------------------- набор */
static void dial(const char *arg, uint32_t now_ms)
{
    char host[64], port[8] = "23";
    const char *c;
    struct addrinfo hints, *ai = 0;
    int rc;

    while (*arg == ' ') arg++;
    if (arg != last_dial) snprintf(last_dial, sizeof last_dial, "%s", arg);
    c = strrchr(arg, ':');
    if (c) {
        size_t hl = (size_t)(c - arg);
        if (hl == 0 || hl >= sizeof host || strlen(c + 1) >= sizeof port) { result("ERROR"); return; }
        memcpy(host, arg, hl); host[hl] = 0;
        strcpy(port, c + 1);
    } else {
        if (!*arg || strlen(arg) >= sizeof host) { result("ERROR"); return; }
        strcpy(host, arg);
    }
    if (gopher_host(host, port) || !strcmp(port, "1965")) {    /* мост: сеть -- за fetch.h */
        g_gem = !strcmp(port, "1965");
        snprintf(g_host, sizeof g_host, "%s", host);
        g_sel_n = 0;
        gopher = 1;
        tn_reset();
        conn = C_GOPHER;
        mode = M_ONLINE;
        last_in_ms = now_ms;
        result("CONNECT");
        return;
    }
    if (!strcmp(port, "443")) {         /* TLS-туннель: шифрование -- за fetch.h */
        if (tls_open(host, 443) != 0) { result("NO ANSWER"); return; }
        tls_on = 1;
        tn_reset();
        conn = C_TLS;
        dial_ms = now_ms;
        mode = M_DIAL;
        return;
    }
    net_start();
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) { result("NO ANSWER"); return; }
    sock = net_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) { freeaddrinfo(ai); result("ERROR"); return; }
    net_nonblock(sock);
    rc = connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
    freeaddrinfo(ai);
    if (rc != 0 && !net_again()) { hang_up(); result("NO ANSWER"); return; }
    tn_reset();
    dial_ms = now_ms;
    mode = M_DIAL;
}

/* ---------------------------------------------------------- справка и BBS */
static const char help[] =
    "\r\nAT commands (the line ends with Return):\r\n"
    "  ATDT host[:port]   dial, port 23 by default; 443 -- over TLS, 1965 --\r\n"
    "                     Gemini as Gopher\r\n"
    "  ATBBS [word]       BBS list from the Telnet BBS Guide: search by name,\r\n"
    "                     address, software or place\r\n"
    "  ATDS n             dial BBS number n from the list\r\n"
    "  ATDL               dial the last address again\r\n"
    "  +++                back to commands, the line stays (1 s pause around)\r\n"
    "  ATO                back to the line       ATH   hang up\r\n"
    "  AT$SP=port         listen for calls on this TCP port (0 -- off)\r\n"
    "  ATA                answer a call (RING)   ATS0=n answer by itself on ring n\r\n"
    "  ATE0, ATE1         echo off, on           ATI   modem info\r\n"
    "  ATZ                reset                  A/    repeat the last command\r\n"
    "  AT?                this help";

#define BBS_SHOW 15

static void bbs_line(const bbs_t *b, void *arg)
{
    char hp[80], tail[80], out[200];
    int room;
    (void)arg;
    snprintf(hp, sizeof hp, "%s:%d", b->host, b->port);
    room = 79 - (4 + 1 + 24 + 1 + (int)strlen(hp) + 1);
    if (room < 0) room = 0;
    snprintf(tail, sizeof tail, "%s%s%s", b->software, *b->software && *b->location ? ", " : "", b->location);
    snprintf(out, sizeof out, "\r\n%4d %-24.24s %s %.*s", b->num, b->name, hp, room, tail);
    say(out);
}

static void cmd_bbs(const char *word, uint32_t now_ms)
{
    char buf[120];
    int n;
    while (*word == ' ') word++;
    say("\r\nThe Telnet BBS Guide, www.telnetbbsguide.com ...");
    n = bbs_fetch(now_ms);
    if (n < 0) { say("\r\nNo list: "); say(bbs_error()); result("ERROR"); return; }
    if (!*word) {
        snprintf(buf, sizeof buf, "\r\n%d BBS, issue %s. ATBBS word -- search, ATDS n -- dial.", n, bbs_issue());
        say(buf);
        result("OK");
        return;
    }
    n = bbs_search(word, BBS_SHOW, bbs_line, 0);
    if (!n) say("\r\nNothing found.");
    else if (n > BBS_SHOW) {
        snprintf(buf, sizeof buf, "\r\n... and %d more, a longer word narrows the search.", n - BBS_SHOW);
        say(buf);
    }
    result("OK");
}

static void cmd_dial_stored(const char *p, uint32_t now_ms)
{
    char host[64], arg[80];
    int port;
    if (bbs_fetch(now_ms) < 0) { say("\r\nNo list: "); say(bbs_error()); result("ERROR"); return; }
    if (!bbs_get(atoi(p), host, sizeof host, &port)) { result("ERROR"); return; }
    snprintf(arg, sizeof arg, "%s:%d", host, port);
    say("\r\nDialing "); say(arg);
    dial(arg, now_ms);
}

/* ---------------------------------------------------------- команды */
static void answer(uint32_t now_ms)
{
    if (rsock < 0) { result("NO CARRIER"); return; }
    sock = rsock;
    rsock = -1;
    tn_reset();
    conn = C_UNKNOWN;
    conn_ms = now_ms;
    last_in_ms = now_ms;
    online_cmd = 0;
    mode = M_ONLINE;
    result("CONNECT");
}

/* Число после '=' или '?' (показать). -1 -- ни того, ни другого. */
static int reg_arg(char **pp, unsigned *v)
{
    char *p = *pp;
    if (*p == '?') { *pp = p + 1; return 1; }
    if (*p != '=') return -1;
    p++;
    *v = (unsigned)strtoul(p, &p, 10);
    *pp = p;
    return 0;
}

static void command(uint32_t now_ms)
{
    char *p = line;

    line[line_n] = 0;
    line_n = 0;
    while (*p == ' ') p++;
    if (!*p) return;
    if ((p[0] != 'A' && p[0] != 'a') || (p[1] != 'T' && p[1] != 't')) { result("ERROR"); return; }
    snprintf(last_cmd, sizeof last_cmd, "%s", p);
    p += 2;
    while (*p) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        switch (c) {
        case ' ': break;
        case 'Z': hang_up(); listen_off(); listen_port = 0; s0 = 0; echo = 1; break;
        case 'A': answer(now_ms); return;
        case 'S': {
                  unsigned r = (unsigned)strtoul(p, &p, 10), v = 0;
                  int k = reg_arg(&p, &v);
                  char b[16];
                  if (k < 0) { result("ERROR"); return; }
                  if (k == 1) { snprintf(b, sizeof b, "\r\n%03u", r == 0 ? (unsigned)s0 : 0u); say(b); break; }
                  if (r == 0) s0 = v > 255 ? 255 : (int)v;     /* прочие регистры -- молча */
                  break;
                  }
        case '$': if (strncasecmp(p, "SP", 2)) { result("ERROR"); return; }
                  p += 2;
                  {
                      unsigned v = 0;
                      int k = reg_arg(&p, &v);
                      char b[16];
                      if (k < 0 || v > 65535) { result("ERROR"); return; }
                      if (k == 1) { snprintf(b, sizeof b, "\r\n%u", listen_port); say(b); break; }
                      if (v != listen_port) { listen_off(); listen_port = v; lsock_try_ms = now_ms - LISTEN_RETRY_MS; }
                  }
                  break;
        case 'E': echo = (*p == '1'); if (*p == '0' || *p == '1') p++; break;
        case 'H': if (*p == '0') p++; hang_up(); break;
        case 'I': if (*p >= '0' && *p <= '9') p++;
                  say("\r\n" MODEM_NAME); break;
        case 'O': if (!line_up()) { result("NO CARRIER"); return; }
                  online_cmd = 0; mode = M_ONLINE; result("CONNECT"); return;
        case 'D': if (line_up()) { hang_up(); say("\r\nHung up the old line"); }   /* новый набор -- старое прочь */
                  if (*p == 'S' || *p == 's') { cmd_dial_stored(p + 1, now_ms); return; }
                  if (*p == 'L' || *p == 'l') {
                      if (!last_dial[0]) { result("ERROR"); return; }
                      say("\r\nDialing "); say(last_dial);
                      dial(last_dial, now_ms); return;
                  }
                  if (*p == 'T' || *p == 't' || *p == 'P' || *p == 'p') p++;
                  dial(p, now_ms); return;
        case '?': say(help); result("OK"); return;
        case 'B': if (!strncasecmp(p, "BS", 2)) { cmd_bbs(p + 2, now_ms); return; }
                  result("ERROR"); return;
        default:  result("ERROR"); return;
        }
    }
    result("OK");
}

/* Знак строки команды. Эхо, если ATE1; ЗБ стирает; A/ -- повторить прошлую
 * команду, как у Hayes. ESC-последовательности (стрелки терминала) -- мимо. */
static void cmd_char(uint8_t b, uint32_t now_ms)
{
    if (cmd_esc == 1) { cmd_esc = (b == '[' || b == 'O') ? 2 : 0; return; }
    if (cmd_esc == 2) { if (b >= 0x40 && b <= 0x7E) cmd_esc = 0; return; }
    if (b == 27) { cmd_esc = 1; return; }
    if (b == '\r') {
        if (echo) say("\r");
        command(now_ms);
        return;
    }
    if (b == 8 || b == 127) {
        if (!line_n) return;
        line_n--;
        if (echo) say("\b \b");
        return;
    }
    if (b < 0x20 || line_n >= (int)sizeof line - 1) return;
    line[line_n++] = (char)b;
    if (echo) ring_put(&to_vec, b);
    if (line_n == 2 && (line[0] == 'A' || line[0] == 'a') && line[1] == '/') {
        line_n = 0;
        if (!last_cmd[0]) { result("ERROR"); return; }
        line_n = (int)strlen(last_cmd);
        memcpy(line, last_cmd, (size_t)line_n);
        command(now_ms);
    }
}

/* ---------------------------------------------------------- опрос */
static void online_char(uint8_t b, uint32_t now_ms)
{
    if (b == '+' && (plus_n > 0 || now_ms - last_in_ms >= GUARD_MS) && plus_n < 3) {
        if (++plus_n == 3) plus_ms = now_ms;
        last_in_ms = now_ms;
        return;
    }
    while (plus_n) { tn_out('+'); plus_n--; }
    tn_out(b);
    last_in_ms = now_ms;
}

void modem_poll(uint32_t now_ms)
{
    uint8_t b, buf[256];
    int n;

    /* вид соединения: сервер молчит -- сырой TCP */
    if (sock >= 0 && mode != M_DIAL && conn == C_UNKNOWN && now_ms - conn_ms >= CONN_WAIT_MS) conn = C_RAW;
    /* от машины; в соединении -- когда ясен его вид */
    while ((mode != M_ONLINE || (conn != C_UNKNOWN && net_out_n < (int)sizeof net_out - 4))
           && ring_get(&from_vec, &b)) {
        if (mode == M_ONLINE) online_char(b, now_ms);
        else if (mode == M_CMD) cmd_char(b, now_ms);
        /* в наборе байты пропадают, как у настоящего модема */
    }
    if (mode == M_ONLINE && plus_n == 3 && now_ms - plus_ms >= GUARD_MS) {
        plus_n = 0;
        online_cmd = 1;
        mode = M_CMD;
        result("OK");
    }

    if (gopher == 2) { if (g_gem) gem_pump(); else gopher_pump(); }

    /* входящие */
    if (listen_port && lsock < 0 && now_ms - lsock_try_ms >= LISTEN_RETRY_MS) {
        lsock_try_ms = now_ms;
        lsock = net_listen((unsigned short)listen_port);
    }
    if (lsock >= 0) {
        int c = net_accept(lsock);
        if (c >= 0) {
            if (line_up() || rsock >= 0 || mode == M_DIAL) net_close(c);    /* занято */
            else {
                net_nonblock(c);
                rsock = c;
                ring_n = 0;
                ring_start_ms = now_ms;
                ring_ms = now_ms - RING_MS;
            }
        }
    }
    if (rsock >= 0 && mode == M_CMD && !line_up()) {
        if (now_ms - ring_start_ms >= RING_GIVEUP_MS) { net_close(rsock); rsock = -1; }
        else if (now_ms - ring_ms >= RING_MS) {
            ring_ms = now_ms;
            ring_n++;
            result("RING");
            if (s0 && ring_n >= s0) answer(now_ms);
        }
    }
    if (tls_on) { tls_pump(now_ms); return; }
    if (sock < 0) return;

    if (mode == M_DIAL) {
        if (net_writable(sock)) {
            if (net_error(sock)) { hang_up(); result("NO ANSWER"); return; }
            mode = M_ONLINE;
            last_in_ms = now_ms;
            conn_ms = now_ms;
            result("CONNECT");
        } else if (now_ms - dial_ms >= DIAL_TIMEOUT_MS) {
            hang_up(); result("NO ANSWER"); return;
        }
        if (mode == M_DIAL) return;
    }

    /* в сеть */
    if (net_out_n) {
        n = net_send(sock, net_out, net_out_n);
        if (n > 0) {
            memmove(net_out, net_out + n, (size_t)(net_out_n - n));
            net_out_n -= n;
        } else if (n < 0 && !net_again()) {
            hang_up(); result("NO CARRIER"); return;
        }
    }
    /* из сети: не больше, чем влезет к машине (с запасом на ответы модема) */
    n = ring_room(&to_vec) - 16;
    if (n > (int)sizeof buf) n = (int)sizeof buf;
    if (n <= 0) return;
    n = net_recv(sock, buf, n);
    if (n > 0) {
        int i;
        for (i = 0; i < n; i++) tn_in(buf[i]);
    } else if (n == 0 || !net_again()) {
        hang_up(); result("NO CARRIER");
    }
}
