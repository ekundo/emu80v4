/* Скачивание для моста Gopher на маке и в Linux: curl в трубе (fetch.h).
 *
 * Без новых библиотек: curl есть везде, где живут стенды и эмуляторы. Труба
 * неблокирующая, поэтому модем, как и на плате, не замирает. Ошибка HTTP --
 * curl с -f молчит и выходит с кодом 22; тогда считаем, что файла нет (404),
 * а прочие коды выхода -- что не дошли (0). */

#include "fetch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static FILE *pipe_f;
static int   status;
static int   state;                     /* 0 -- нет, 1 -- идёт, 2 -- кончилось, 3 -- не вышло */
static long  got;

int fetch_open(const char *url)
{
    char cmd[1024], full[768];
    const char *base = getenv("COMBO_FETCH_BASE");

    if (pipe_f) return -1;
    if (strchr(url, '\'')) return -1;   /* адрес идёт в оболочку в кавычках */
    full[0] = 0;
    if (base && *base) {                /* стенд: https://хост/путь -> base/путь */
        const char *p = strstr(url, "://");
        p = p ? strchr(p + 3, '/') : 0;
        snprintf(full, sizeof full, "%s%s", base, p ? p : "/");
        url = full;
    }
    snprintf(cmd, sizeof cmd,
             "curl -s -f -L --max-redirs 5 --max-time 60 --max-filesize %d -A ComboLite '%s' 2>/dev/null",
             FETCH_MAX, url);
    pipe_f = popen(cmd, "r");
    if (!pipe_f) return -1;
    fcntl(fileno(pipe_f), F_SETFL, fcntl(fileno(pipe_f), F_GETFL, 0) | O_NONBLOCK);
    status = 0;
    got = 0;
    state = 1;
    return 0;
}

int fetch_read(uint8_t *buf, int max)
{
    ssize_t n;
    int rc;

    if (state == 2) return FETCH_DONE;
    if (state != 1 || !pipe_f) return FETCH_FAIL;
    n = read(fileno(pipe_f), buf, (size_t)max);
    if (n > 0) { got += n; return (int)n; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    rc = pclose(pipe_f);                /* конец трубы: как вышел curl */
    pipe_f = 0;
    if (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0) { state = 2; return FETCH_DONE; }
    status = (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 22) ? 404 : 0;
    state = 3;
    return FETCH_FAIL;
}

int fetch_status(void) { return status; }

void fetch_close(void)
{
    if (pipe_f) pclose(pipe_f);         /* ждёт curl: на прерванном это доли секунды */
    pipe_f = 0;
    state = 0;
}
