/* Скачивание для моста Gopher на маке, в Linux и Windows: curl в трубе (fetch.h).
 *
 * Без новых библиотек: curl есть везде, где живут стенды и эмуляторы (в
 * Windows -- свой, с Windows 10). Труба не блокирует модем: в POSIX она
 * неблокирующая, в Windows прежде чтения спрашиваем, сколько в ней есть
 * (PeekNamedPipe). Ошибка HTTP -- curl с -f молчит и выходит с кодом 22;
 * тогда считаем, что файла нет (404), а прочие коды выхода -- что не дошли (0). */

#include "fetch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define popen  _popen
#define pclose _pclose
#define QUOTE  "\""                     /* cmd.exe: адрес в двойных кавычках */
#define NULL_DEV "NUL"
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#define QUOTE  "'"
#define NULL_DEV "/dev/null"
#endif

static FILE *pipe_f;
static int   status;
static int   state;                     /* 0 -- нет, 1 -- идёт, 2 -- кончилось, 3 -- не вышло */

/* Код выхода curl по итогу pclose; -1 -- неизвестен. */
static int exit_code(int rc)
{
#ifdef _WIN32
    return rc;
#else
    return (rc != -1 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
#endif
}

int fetch_open(const char *url)
{
    char cmd[1024], full[768];
    const char *base = getenv("COMBO_FETCH_BASE");

    if (pipe_f) return -1;
    if (strchr(url, '\'') || strchr(url, '"')) return -1;   /* адрес идёт в оболочку в кавычках */
    full[0] = 0;
    if (base && *base) {                /* стенд: https://хост/путь -> base/путь */
        const char *p = strstr(url, "://");
        p = p ? strchr(p + 3, '/') : 0;
        snprintf(full, sizeof full, "%s%s", base, p ? p : "/");
        url = full;
    }
    snprintf(cmd, sizeof cmd,
             "curl -s -f -L --max-redirs 5 --max-time 60 --max-filesize %d -A ComboLite "
             QUOTE "%s" QUOTE " 2>" NULL_DEV, FETCH_MAX, url);
#ifdef _WIN32
    pipe_f = popen(cmd, "rb");
#else
    pipe_f = popen(cmd, "r");
    if (pipe_f) fcntl(fileno(pipe_f), F_SETFL, fcntl(fileno(pipe_f), F_GETFL, 0) | O_NONBLOCK);
#endif
    if (!pipe_f) return -1;
    status = 0;
    state = 1;
    return 0;
}

/* Сколько байт трубы можно прочесть, не блокируясь: >0, 0 -- пока нечего,
 * -1 -- труба кончилась (curl вышел). */
static int pipe_read(uint8_t *buf, int max)
{
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(pipe_f));
    DWORD avail = 0;
    if (!PeekNamedPipe(h, 0, 0, 0, &avail, 0)) return -1;  /* писатель закрыл, всё прочитано */
    if (!avail) return 0;
    if ((DWORD)max > avail) max = (int)avail;
    return _read(_fileno(pipe_f), buf, (unsigned)max);
#else
    ssize_t n = read(fileno(pipe_f), buf, (size_t)max);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
#endif
}

int fetch_read(uint8_t *buf, int max)
{
    int n, code;

    if (state == 2) return FETCH_DONE;
    if (state != 1 || !pipe_f) return FETCH_FAIL;
    n = pipe_read(buf, max);
    if (n > 0) return n;
    if (n == 0) return 0;
    code = exit_code(pclose(pipe_f));   /* конец трубы: как вышел curl */
    pipe_f = 0;
    if (code == 0) { state = 2; return FETCH_DONE; }
    status = code == 22 ? 404 : 0;
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
