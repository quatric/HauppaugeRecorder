/*
 * Operating system glue for the TV helpers (tvcore.hpp): time, files, child
 * processes with pipes, our own stdin/stdout, Ctrl-C. One implementation for
 * Windows and one for POSIX (Linux, macOS).
 */
#ifndef TVPLAT_HPP
#define TVPLAT_HPP

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

typedef long long i64;

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

namespace plat {

struct Proc {
#ifdef _WIN32
    HANDLE in = 0, out = 0, proc = 0;
#else
    int in = -1, out = -1;
    pid_t pid = -1;
#endif
};

#ifdef _WIN32

inline i64 now_100ns()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (i64)((double)c.QuadPart * 1e7 / (double)f.QuadPart);
}
inline void sleep_ms(int ms) { Sleep((DWORD)ms); }
inline int pid() { return (int)GetCurrentProcessId(); }
inline const char *exe_ext() { return ".exe"; }

inline std::string narrow(const wchar_t *w)
{
    char b[2048];
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, 0, 0) ? b : "";
}
inline std::wstring widen(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, 0, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

inline std::string temp_dir()
{
    wchar_t t[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH + 1, t);
    return n ? narrow(t) : ".\\";
}
inline std::string exe_dir()
{
    wchar_t me[MAX_PATH];
    GetModuleFileNameW(0, me, MAX_PATH);
    std::string d = narrow(me);
    return d.substr(0, d.find_last_of("\\/") + 1);
}
inline bool file_exists(const std::string &p) { return GetFileAttributesW(widen(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
inline void remove_file(const std::string &p) { DeleteFileW(widen(p).c_str()); }
inline FILE *fopen_utf8(const std::string &p, const char *mode) { return _wfopen(widen(p).c_str(), widen(mode).c_str()); }
inline std::string find_in_path(const std::string &name)
{
    wchar_t found[MAX_PATH];
    return SearchPathW(0, widen(name).c_str(), 0, MAX_PATH, found, 0) ? narrow(found) : "";
}
inline std::string quote(const std::string &s) { return "\"" + s + "\""; }
inline bool local_time(time_t t, struct tm *out) { return localtime_s(out, &t) == 0; }
inline char dir_sep() { return '\\'; }

inline bool spawn(const std::string &cmd, bool pipe_in, bool pipe_out, bool quiet, Proc &p)
{
    SECURITY_ATTRIBUTES sa{ sizeof sa, 0, TRUE };
    HANDLE inR = 0, inW = 0, outR = 0, outW = 0;
    if (pipe_in) { CreatePipe(&inR, &inW, &sa, 1 << 20); SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0); }
    if (pipe_out) { CreatePipe(&outR, &outW, &sa, 1 << 20); SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0); }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = pipe_in ? inR : nul;
    si.hStdOutput = pipe_out ? outW : (quiet ? nul : GetStdHandle(STD_OUTPUT_HANDLE));
    si.hStdError = quiet ? nul : GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring w = widen(cmd);
    BOOL ok = CreateProcessW(0, &w[0], 0, 0, TRUE, CREATE_NO_WINDOW, 0, 0, &si, &pi);
    if (inR) CloseHandle(inR);
    if (outW) CloseHandle(outW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { if (inW) CloseHandle(inW); if (outR) CloseHandle(outR); return false; }
    CloseHandle(pi.hThread);
    p.in = inW;
    p.out = outR;
    p.proc = pi.hProcess;
    return true;
}
inline bool write_in(Proc &p, const void *d, size_t n)
{
    const char *c = (const char *)d;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(p.in, c, (DWORD)(n > (1u << 20) ? (1u << 20) : n), &w, 0) || !w) return false;
        c += w; n -= w;
    }
    return true;
}
inline long read_out(Proc &p, void *d, size_t n)
{
    DWORD got = 0;
    if (!ReadFile(p.out, d, (DWORD)n, &got, 0)) return got ? (long)got : 0; /* a closed pipe reads as end of data */
    return (long)got;
}
inline void close_in(Proc &p) { if (p.in) { CloseHandle(p.in); p.in = 0; } }
inline void close_out(Proc &p) { if (p.out) { CloseHandle(p.out); p.out = 0; } }
/* exit code, or -1 if still running after timeout_ms (-1 = wait forever) */
inline int wait(Proc &p, int timeout_ms)
{
    if (!p.proc) return 0;
    if (WaitForSingleObject(p.proc, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms) == WAIT_TIMEOUT) return -1;
    DWORD code = 1;
    GetExitCodeProcess(p.proc, &code);
    CloseHandle(p.proc);
    p.proc = 0;
    return (int)code;
}
inline void kill(Proc &p) { if (p.proc) TerminateProcess(p.proc, 1); }

inline long stdin_read(void *d, size_t n)
{
    DWORD got = 0;
    return ReadFile(GetStdHandle(STD_INPUT_HANDLE), d, (DWORD)n, &got, 0) ? (long)got : -1;
}
inline bool stdout_write(const void *d, size_t n)
{
    const char *c = (const char *)d;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), c, (DWORD)n, &w, 0) || !w) return false;
        c += w; n -= w;
    }
    return true;
}
static std::atomic<bool> *g_stop_flag = nullptr;
inline BOOL WINAPI ctrl_handler(DWORD) { if (g_stop_flag) *g_stop_flag = true; return TRUE; }
inline void install_stop_handler(std::atomic<bool> *flag)
{
    g_stop_flag = flag;
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
}
/* leave without waiting for a thread blocked in a read of our stdin */
[[noreturn]] inline void hard_exit(int code) { TerminateProcess(GetCurrentProcess(), (UINT)code); for (;;) {} }

#else /* POSIX */

inline i64 now_100ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 10000000 + ts.tv_nsec / 100;
}
inline void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
inline int pid() { return (int)getpid(); }
inline const char *exe_ext() { return ""; }
inline char dir_sep() { return '/'; }

inline std::string temp_dir()
{
    const char *t = getenv("TMPDIR");
    std::string d = t && *t ? t : "/tmp";
    if (d.back() != '/') d += '/';
    return d;
}
inline std::string exe_dir()
{
    char p[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
    if (n <= 0) return "";
    p[n] = 0;
    std::string d = p;
    return d.substr(0, d.find_last_of('/') + 1);
}
inline bool file_exists(const std::string &p) { return access(p.c_str(), F_OK) == 0; }
inline void remove_file(const std::string &p) { unlink(p.c_str()); }
inline FILE *fopen_utf8(const std::string &p, const char *mode) { return fopen(p.c_str(), mode); }
inline std::string find_in_path(const std::string &name)
{
    const char *path = getenv("PATH");
    std::string s = path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(':', i);
        if (j == std::string::npos) j = s.size();
        std::string c = s.substr(i, j - i) + "/" + name;
        if (access(c.c_str(), X_OK) == 0) return c;
        i = j + 1;
    }
    return "";
}
inline std::string quote(const std::string &s)
{
    std::string r = "'";
    for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
    return r + "'";
}
inline bool local_time(time_t t, struct tm *out) { return localtime_r(&t, out) != nullptr; }

/* cmd is a shell command line (the core quotes its arguments with quote()) */
inline bool spawn(const std::string &cmd, bool pipe_in, bool pipe_out, bool quiet, Proc &p)
{
    int in[2] = { -1, -1 }, out[2] = { -1, -1 };
    if ((pipe_in && pipe(in)) || (pipe_out && pipe(out))) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (!pid) {
        int nul = open("/dev/null", O_RDWR);
        dup2(pipe_in ? in[0] : nul, 0);
        if (pipe_out) dup2(out[1], 1); else if (quiet) dup2(nul, 1);
        if (quiet) dup2(nul, 2);
        for (int i = 3; i < 1024; i++) close(i);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char *)nullptr);
        _exit(127);
    }
    if (pipe_in) { close(in[0]); p.in = in[1]; fcntl(p.in, F_SETFD, FD_CLOEXEC); }
    if (pipe_out) { close(out[1]); p.out = out[0]; fcntl(p.out, F_SETFD, FD_CLOEXEC); }
    p.pid = pid;
    return true;
}
inline bool write_in(Proc &p, const void *d, size_t n)
{
    const char *c = (const char *)d;
    while (n) {
        ssize_t w = write(p.in, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        c += w; n -= (size_t)w;
    }
    return true;
}
inline long read_out(Proc &p, void *d, size_t n)
{
    for (;;) {
        ssize_t r = read(p.out, d, n);
        if (r < 0 && errno == EINTR) continue;
        return (long)r;
    }
}
inline void close_in(Proc &p) { if (p.in >= 0) { close(p.in); p.in = -1; } }
inline void close_out(Proc &p) { if (p.out >= 0) { close(p.out); p.out = -1; } }
inline int wait(Proc &p, int timeout_ms)
{
    if (p.pid < 0) return 0;
    for (int waited = 0;; waited += 20) {
        int st = 0;
        pid_t r = waitpid(p.pid, &st, WNOHANG);
        if (r == p.pid) { p.pid = -1; return WIFEXITED(st) ? WEXITSTATUS(st) : 1; }
        if (timeout_ms >= 0 && waited >= timeout_ms) return -1;
        usleep(20000);
    }
}
inline void kill(Proc &p) { if (p.pid > 0) ::kill(p.pid, SIGKILL); }

inline long stdin_read(void *d, size_t n)
{
    for (;;) {
        ssize_t r = read(0, d, n);
        if (r < 0 && errno == EINTR) continue;
        return (long)r;
    }
}
inline bool stdout_write(const void *d, size_t n)
{
    const char *c = (const char *)d;
    while (n) {
        ssize_t w = write(1, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        c += w; n -= (size_t)w;
    }
    return true;
}
static std::atomic<bool> *g_stop_flag = nullptr;
inline void on_signal(int) { if (g_stop_flag) *g_stop_flag = true; }
inline void install_stop_handler(std::atomic<bool> *flag)
{
    g_stop_flag = flag;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}
[[noreturn]] inline void hard_exit(int code) { _exit(code); }

#endif

} // namespace plat

#endif
