/* Client for the TV capture helper; see tvdev.h and docs/TVHELPER.md. */
#include "tvdev.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define HELPER_NAME "wintv-capture.exe"
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#define HELPER_NAME "tv-capture-macos"
#else
#define HELPER_NAME "tv-capture-linux"
#endif
#endif

struct tvdev {
    tv_events_t ev;
    pthread_t reader;
    pthread_mutex_t wlock;
    volatile int quit;
#ifdef _WIN32
    HANDLE in, out, proc;
#else
    int in, out;
    pid_t pid;
#endif
};

/* the folder holding the running program, with a trailing separator */
static int exe_dir(char *out, size_t len)
{
#ifdef _WIN32
    wchar_t w[1024];
    DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (!n || n >= 1024)
        return 0;
    while (n && w[n - 1] != L'\\' && w[n - 1] != L'/')
        n--;
    w[n] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)len, NULL, NULL) != 0;
#else
    char p[PATH_MAX];
#ifdef __APPLE__
    uint32_t sz = sizeof(p);
    if (_NSGetExecutablePath(p, &sz))
        return 0;
#else
    ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
    if (n <= 0)
        return 0;
    p[n] = 0;
#endif
    char *s = strrchr(p, '/');
    if (!s)
        return 0;
    s[1] = 0;
    snprintf(out, len, "%s", p);
    return 1;
#endif
}

static int helper_path(char *out, size_t len)
{
    char dir[1100];
    if (!exe_dir(dir, sizeof(dir)))
        return 0;
    snprintf(out, len, "%s%s", dir, HELPER_NAME);
#ifdef _WIN32
    wchar_t w[1200];
    if (!MultiByteToWideChar(CP_UTF8, 0, out, -1, w, 1200))
        return 0;
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
#else
    return access(out, X_OK) == 0;
#endif
}

int tv_helper_available(void)
{
    char p[1300];
    return helper_path(p, sizeof(p));
}

/* ---- transport ---- */

static int rd_exact(tvdev_t *t, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(t->out, p, (DWORD)(n > 1u << 20 ? 1u << 20 : n), &got, NULL) || !got)
            return -1;
        p += got, n -= got;
#else
        ssize_t got = read(t->out, p, n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        p += got, n -= (size_t)got;
#endif
    }
    return 0;
}

static const char *kv(const char *s, const char *key, char *buf, size_t len)
{
    /* "a=1;b=text;c=3": value of key, up to the next ';' (the last key, msg/err, may contain anything) */
    size_t kl = strlen(key);
    const char *p = s;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, ';');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= len)
                n = len - 1;
            memcpy(buf, v, n);
            buf[n] = 0;
            return buf;
        }
        p = strchr(p, ';');
        if (p)
            p++;
    }
    buf[0] = 0;
    return buf;
}

static void handle(tvdev_t *t, char type, uint8_t *p, uint32_t n)
{
    char a[512], b[512], c[512];
    switch (type) {
    case 'S':
        if (t->ev.state && n < 1000) {
            char s[1024];
            memcpy(s, p, n);
            s[n] = 0;
            /* msg is last on the line and may contain ';' */
            const char *m = strstr(s, ";msg=");
            char msg[512] = "";
            if (m)
                snprintf(msg, sizeof(msg), "%s", m + 5);
            t->ev.state(t->ev.user, atoi(kv(s, "state", a, sizeof(a))), msg, atoi(kv(s, "w", b, sizeof(b))),
                        atoi(kv(s, "h", c, sizeof(c))), (int)(atof(kv(s, "fps", a, sizeof(a))) * 100 + 0.5),
                        atoi(kv(s, "wide", b, sizeof(b))));
        }
        break;
    case 'F':
        if (t->ev.frame && n > 8) {
            uint32_t w, h;
            memcpy(&w, p, 4);
            memcpy(&h, p + 4, 4);
            if (w && h && w < 4096 && h < 4096 && (uint64_t)w * h * 4 + 8 == n)
                t->ev.frame(t->ev.user, p + 8, (int)w, (int)h);
        }
        break;
    case 'L':
        if (t->ev.levels && n == 8) {
            float d[2];
            memcpy(d, p, 8);
            t->ev.levels(t->ev.user, d[0], d[1]);
        }
        break;
    case 'A':
        if (t->ev.audio)
            t->ev.audio(t->ev.user, (const int16_t *)(const void *)p, (int)(n / 4));
        break;
    case 'K':
        if (t->ev.record_started)
            t->ev.record_started(t->ev.user);
        break;
    case 'P':
        if (t->ev.progress && n == 16) {
            double sec;
            uint64_t bytes;
            memcpy(&sec, p, 8);
            memcpy(&bytes, p + 8, 8);
            t->ev.progress(t->ev.user, sec, bytes);
        }
        break;
    case 'R':
        if (t->ev.record_done && n < 2000) {
            char s[2048], path[1100], err[600];
            memcpy(s, p, n);
            s[n] = 0;
            /* path= and err= can hold ';' in theory; the helper puts them between known keys */
            const char *pp = strstr(s, ";path="), *ee = strstr(s, ";err="), *ff = strstr(s, ";frames=");
            path[0] = err[0] = 0;
            if (pp && ee && pp < ee)
                snprintf(path, sizeof(path), "%.*s", (int)(ee - (pp + 6)), pp + 6);
            if (ee && ff && ee < ff)
                snprintf(err, sizeof(err), "%.*s", (int)(ff - (ee + 5)), ee + 5);
            t->ev.record_done(t->ev.user, atoi(kv(s, "ok", a, sizeof(a))), path, err, atoi(kv(s, "frames", b, sizeof(b))),
                              atoi(kv(s, "cc", c, sizeof(c))));
        }
        break;
    default:
        break;
    }
}

static void *reader_thread(void *arg)
{
    tvdev_t *t = arg;
    uint8_t *buf = NULL;
    size_t cap = 0;
    while (!t->quit) {
        uint8_t h[8];
        if (rd_exact(t, h, 8))
            break;
        uint32_t n;
        memcpy(&n, h + 4, 4);
        if (n > (16u << 20))
            break; /* corrupt stream */
        if (n > cap) {
            free(buf);
            cap = n + 4096;
            buf = malloc(cap);
            if (!buf)
                break;
        }
        if (n && rd_exact(t, buf, n))
            break;
        handle(t, (char)h[0], buf, n);
    }
    free(buf);
    if (!t->quit && t->ev.state) /* the helper died */
        t->ev.state(t->ev.user, 1, "The tuner program stopped unexpectedly. Restart the app.", 0, 0, 0, 0);
    return NULL;
}

/* ---- start and stop ---- */

#ifdef _WIN32
tvdev_t *tv_open(const tv_events_t *ev)
{
    char path[1300];
    wchar_t wpath[1400], cmd[1500];
    if (!helper_path(path, sizeof(path)) || !MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1400))
        return NULL;
    tvdev_t *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->ev = *ev;
    pthread_mutex_init(&t->wlock, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE inR, inW, outR, outW;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 1 << 22)) {
        free(t);
        return NULL;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = nul;
    PROCESS_INFORMATION pi = { 0 };
    swprintf(cmd, 1500, L"\"%ls\" --serve", wpath);
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (!ok) {
        CloseHandle(inW);
        CloseHandle(outR);
        free(t);
        return NULL;
    }
    CloseHandle(pi.hThread);
    t->in = inW;
    t->out = outR;
    t->proc = pi.hProcess;
    if (pthread_create(&t->reader, NULL, reader_thread, t)) {
        TerminateProcess(t->proc, 1);
        CloseHandle(t->proc);
        CloseHandle(inW);
        CloseHandle(outR);
        free(t);
        return NULL;
    }
    return t;
}

void tv_send(tvdev_t *t, const char *fmt, ...)
{
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (!t || n <= 0)
        return;
    line[n++] = '\n';
    pthread_mutex_lock(&t->wlock);
    DWORD w;
    WriteFile(t->in, line, (DWORD)n, &w, NULL);
    pthread_mutex_unlock(&t->wlock);
}

void tv_close(tvdev_t *t)
{
    if (!t)
        return;
    tv_send(t, "quit");
    if (WaitForSingleObject(t->proc, 15000) == WAIT_TIMEOUT) /* a recording may be finishing */
        TerminateProcess(t->proc, 1);
    t->quit = 1;
    CloseHandle(t->in);
    pthread_join(t->reader, NULL);
    CloseHandle(t->out);
    CloseHandle(t->proc);
    free(t);
}

#else /* POSIX */

tvdev_t *tv_open(const tv_events_t *ev)
{
    char path[1300];
    if (!helper_path(path, sizeof(path)))
        return NULL;
    tvdev_t *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->ev = *ev;
    pthread_mutex_init(&t->wlock, NULL);
    int in[2], out[2];
    if (pipe(in) || pipe(out)) {
        free(t);
        return NULL;
    }
    signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();
    if (pid < 0) {
        free(t);
        return NULL;
    }
    if (!pid) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0)
            dup2(nul, 2);
        for (int i = 3; i < 256; i++)
            close(i);
        execl(path, path, "--serve", (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    t->in = in[1];
    t->out = out[0];
    t->pid = pid;
    if (pthread_create(&t->reader, NULL, reader_thread, t)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(t->in);
        close(t->out);
        free(t);
        return NULL;
    }
    return t;
}

void tv_send(tvdev_t *t, const char *fmt, ...)
{
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (!t || n <= 0)
        return;
    line[n++] = '\n';
    pthread_mutex_lock(&t->wlock);
    ssize_t w = write(t->in, line, (size_t)n);
    (void)w;
    pthread_mutex_unlock(&t->wlock);
}

void tv_close(tvdev_t *t)
{
    if (!t)
        return;
    tv_send(t, "quit");
    close(t->in);
    for (int i = 0; i < 150; i++) { /* up to 15 s: a recording may be finishing */
        if (waitpid(t->pid, NULL, WNOHANG) != 0)
            goto exited;
        usleep(100000);
    }
    kill(t->pid, SIGKILL);
    waitpid(t->pid, NULL, 0);
exited:
    t->quit = 1;
    pthread_join(t->reader, NULL);
    close(t->out);
    free(t);
}
#endif
