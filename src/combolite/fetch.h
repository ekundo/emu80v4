/* Скачать файл по HTTPS -- для моста «Gopher поверх HTTPS» в модеме (modem.c).
 *
 * GitHub Pages отдаёт только HTTPS, а GOPHER.COM ходит сырым TCP на порт 70.
 * Модем сам снимает шифрование: принимает строку выбора от «Вектора»,
 * скачивает файл и отдаёт его байты как ответ Gopher. Сам модем остаётся
 * переносимым, а скачивание -- за этим API:
 *   на плате  -- fetch_esp.c: esp_http_client в своей задаче, сертификат
 *                проверяется набором корневых ESP-IDF, байты -- через
 *                StreamBuffer;
 *   на маке   -- fetch_curl.c: curl в трубе (стенды tools/test_*.py, v06x,
 *                Emu80). Переменная COMBO_FETCH_BASE подменяет «https://хост»
 *                -- стенд проверяет мост без сети.
 *
 * Одно скачивание за раз. Чтение не блокирует. */
#ifndef COMBO_FETCH_H
#define COMBO_FETCH_H

#include <stdint.h>

#define FETCH_MAX  (1024 * 1024)        /* больше не отдаём: на «Векторе» столько и не уместить */

/* Начать скачивание. 0 -- пошло, иначе -- не вышло (уже идёт другое, нет памяти). */
int  fetch_open(const char *url);

/* Байты скачанного в buf, не больше max. Возвращает >0 -- сколько, 0 --
 * пока нечего, FETCH_DONE -- файл кончился, FETCH_FAIL -- не вышло
 * (fetch_status: код HTTP, 0 -- сеть). */
#define FETCH_DONE  (-1)
#define FETCH_FAIL  (-2)
int  fetch_read(uint8_t *buf, int max);
int  fetch_status(void);

/* Закончить (и прервать, если ещё идёт). */
void fetch_close(void);

/* Gemini (мост в modem.c): TLS на host:port, запрос -- url и CR LF, ответ
 * сервера целиком (строка состояния и тело) -- тем же fetch_read. Сертификат
 * -- «доверие с первого раза»: отпечаток запоминается при первом заходе на
 * хост, другой -- FETCH_FAIL с fetch_status() == FETCH_CERT, пока не придёт
 * trust_new. На маке проверки нет (стенды, эмуляторы), в Windows Gemini нет
 * вовсе (-1). COMBO_GEMINI_CONNECT=хост:порт на маке подменяет, куда
 * соединяться (имя для SNI остаётся) -- стенд без сети. */
#define FETCH_CERT  1
int  fetch_gemini(const char *host, int port, const char *url, int trust_new);

/* TLS-туннель (modem.c: набор на порт 443): байты в обе стороны как есть,
 * HTTP и прочее -- забота программы на «Векторе». Сертификат на плате
 * проверяется набором корневых ESP-IDF; на маке -- openssl s_client, а
 * COMBO_TLS_CONNECT=хост:порт подменяет, куда соединяться, и проверку
 * снимает (стенд с самоподписанным). В Windows туннеля нет (-1). Один за раз,
 * вместе со скачиванием -- тоже один. */
int  tls_open(const char *host, int port);
int  tls_ready(void);                   /* 1 -- соединились, 0 -- ещё нет, -1 -- не вышло */
int  tls_write(const uint8_t *buf, int n);      /* сколько взято, 0 -- пока некуда */
int  tls_read(uint8_t *buf, int max);   /* как fetch_read: >0, 0, FETCH_DONE, FETCH_FAIL */
void tls_close(void);

#endif
