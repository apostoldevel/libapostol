// src/core/crash_handler.cpp
//
// Fatal-signal crash reporter.
//
// Installs SA_SIGINFO + SA_ONSTACK handlers for SIGSEGV/SIGFPE/SIGILL/SIGBUS/SIGABRT.
// On crash:
//   1. Opens the error log file (if configured) with O_WRONLY | O_APPEND.
//   2. Writes a structured report to both stderr and the log file.
//   3. For each backtrace frame, calls addr2line for file:line resolution
//      (requires debug symbols in the binary; falls back to backtrace_symbols
//       + abi::__cxa_demangle if addr2line is unavailable or returns "??").
//   4. Re-raises the signal with SIG_DFL so the kernel generates a core dump.
//
// Signal safety note:
//   The crash handler deliberately uses functions that are NOT in the
//   async-signal-safe set (popen, fprintf, abi::__cxa_demangle, ...).
//   This is intentional: we are about to terminate the process anyway, and
//   the only realistic alternative (purely async-safe I/O) cannot produce
//   demangled symbols or file:line information.  The approach is identical to
//   what glibc, nginx, PostgreSQL, and most production daemons do.

#include "apostol/crash_handler.hpp"

#include <cxxabi.h>
#include <execinfo.h>
#include <ucontext.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dlfcn.h>
#include <fcntl.h>
#include <cstdint>
#include <sys/prctl.h>
#include <unistd.h>

namespace apostol
{

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr int    kMaxFrames    = 64;
static constexpr size_t kAltStackSize = 65536; // 64 KiB — POSIX minimum is SIGSTKSZ

// ─── Per-process alternate signal stack ──────────────────────────────────────
// Declared static so each translation unit that calls setup_crash_altstack()
// allocates its own buffer.  In practice only one TU links this file.

alignas(16) static char g_altstack[kAltStackSize];

// ─── Global crash log path ───────────────────────────────────────────────────
// Written only from the main thread (Application::run / config reload).
// Read only from the crash handler (single-threaded at that point).

static char g_log_path[4096] = {};

// ─── Low-level I/O helpers ───────────────────────────────────────────────────

// Async-signal-safe write of a C string to fd.
static void raw_write(int fd, const char* s) noexcept
{
    if (fd < 0 || !s) return;
    size_t len = ::strlen(s);
    while (len > 0)
    {
        ssize_t n = ::write(fd, s, len);
        if (n <= 0) return;
        s   += n;
        len -= (size_t)n;
    }
}

// printf-style write to two file descriptors (fd2 == -1 → skip).
// NOT async-signal-safe, but used only in the crash handler (we exit after).
[[gnu::format(printf, 3, 4)]]
static void emit(int fd1, int fd2, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    raw_write(fd1, buf);
    if (fd2 >= 0) raw_write(fd2, buf);
}

// ─── Symbol resolution ───────────────────────────────────────────────────────

// Demangle a C++ mangled symbol.  Returns the input on failure.
// Caller must free() the returned pointer only if it != input.
static const char* demangle(const char* sym, char* buf, size_t buf_sz)
{
    int status = 0;
    size_t n   = buf_sz;
    char* out  = abi::__cxa_demangle(sym, buf, &n, &status);
    return (status == 0 && out) ? out : sym;
}

// Extract the mangled symbol name from a backtrace_symbols() string.
// Format: "path(mangled_name+0xOFFSET) [0xADDR]"
// Returns true and fills 'sym' on success.
static bool extract_symbol(const char* bt_line, char* sym, size_t sym_sz)
{
    const char* lp = ::strchr(bt_line, '(');
    if (!lp) return false;
    const char* rp = ::strchr(lp, '+');
    if (!rp || rp == lp + 1) return false;
    size_t len = (size_t)(rp - lp - 1);
    if (len >= sym_sz) len = sym_sz - 1;
    ::memcpy(sym, lp + 1, len);
    sym[len] = '\0';
    return sym[0] != '\0';
}

// Call addr2line to resolve 'addr' to "function at file:line".
//
// For PIE binaries (ASLR), backtrace() returns absolute virtual addresses.
// addr2line needs the offset relative to the load base of the binary/library.
// We use dladdr() to obtain the load base (dli_fbase) and the object path
// (dli_fname), then compute: offset = addr - dli_fbase.
//
// Output format (addr2line -f -C -p): "function_name at file:line\n"
// Returns true if a useful result was obtained; fills func_out / loc_out.
static bool addr2line_resolve(void* addr,
                               char* func_out, size_t func_sz,
                               char* loc_out,  size_t loc_sz)
{
    func_out[0] = loc_out[0] = '\0';

    // Resolve load base + object file via dladdr
    Dl_info dl = {};
    if (!::dladdr(addr, &dl) || !dl.dli_fbase || !dl.dli_fname)
        return false;

    // Relative offset within the binary (works for both PIE and non-PIE)
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)dl.dli_fbase;

    char cmd[1024];
    // -f: show function name  -C: demangle  -p: pretty-print  -e: binary
    ::snprintf(cmd, sizeof(cmd),
               "addr2line -e '%s' -f -C -p 0x%zx 2>/dev/null",
               dl.dli_fname, (size_t)offset);

    FILE* pipe = ::popen(cmd, "r");
    if (!pipe) return false;

    char buf[512] = {};
    bool ok = ::fgets(buf, sizeof(buf), pipe) != nullptr;
    ::pclose(pipe);

    if (!ok || buf[0] == '\0' || ::strncmp(buf, "??", 2) == 0)
        return false;

    // Strip trailing newline
    char* nl = ::strchr(buf, '\n');
    if (nl) *nl = '\0';

    // Split on " at " → function name + file:line
    char* at = ::strstr(buf, " at ");
    if (at)
    {
        size_t fn_len = (size_t)(at - buf);
        if (fn_len >= func_sz) fn_len = func_sz - 1;
        ::memcpy(func_out, buf, fn_len);
        func_out[fn_len] = '\0';

        const char* loc = at + 4;
        if (::strncmp(loc, "??", 2) == 0)
            loc_out[0] = '\0'; // no file:line — omit
        else
        {
            ::snprintf(loc_out, loc_sz, "%s", loc);
        }
    }
    else
    {
        ::snprintf(func_out, func_sz, "%s", buf);
    }

    return func_out[0] != '\0';
}

// ─── Crash handler ───────────────────────────────────────────────────────────

static void crash_handler(int signo, siginfo_t* si, void* ctx)
{
    // ── Resolve binary path ───────────────────────────────────────────────────
    char binary[512] = {};
    {
        ssize_t n = ::readlink("/proc/self/exe", binary, sizeof(binary) - 1);
        if (n > 0)  binary[n] = '\0';
        else        ::strncpy(binary, "?", sizeof(binary) - 1);
    }

    // ── Resolve process title (set via prctl PR_SET_NAME) ─────────────────────
    char proc_name[64] = {};
    ::prctl(PR_GET_NAME, proc_name, 0, 0, 0);

    // ── Instruction pointer (faulting instruction address) ────────────────────
    void* ip = nullptr;
#if   defined(__x86_64__)
    ip = reinterpret_cast<void*>(
             static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__i386__)
    ip = reinterpret_cast<void*>(
             static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_EIP]);
#elif defined(__aarch64__)
    ip = reinterpret_cast<void*>(
             static_cast<ucontext_t*>(ctx)->uc_mcontext.pc);
#elif defined(__arm__)
    ip = reinterpret_cast<void*>(
             static_cast<ucontext_t*>(ctx)->uc_mcontext.arm_pc);
#endif
    (void)ctx; // suppress unused warning on unknown arches

    // ── Collect backtrace ─────────────────────────────────────────────────────
    void* frames[kMaxFrames];
    int n_frames = ::backtrace(frames, kMaxFrames);

    // ── Open log file (if configured) ─────────────────────────────────────────
    int log_fd = -1;
    if (g_log_path[0] != '\0')
        log_fd = ::open(g_log_path, O_WRONLY | O_APPEND | O_CREAT, 0644);

    int e = STDERR_FILENO;

    // ── Timestamp ─────────────────────────────────────────────────────────────
    char ts[32] = {};
    {
        time_t t = ::time(nullptr);
        struct tm tm_buf;
        ::localtime_r(&t, &tm_buf);
        ::strftime(ts, sizeof(ts), "%Y/%m/%d %H:%M:%S", &tm_buf);
    }

    // ── Header ────────────────────────────────────────────────────────────────
    raw_write(e, "\n-----BEGIN CRASH REPORT-----\n");
    if (log_fd >= 0) raw_write(log_fd, "\n-----BEGIN CRASH REPORT-----\n");

    emit(e, log_fd, "%s | crit | %s (pid=%d)\n", ts, proc_name, (int)::getpid());
    emit(e, log_fd, "Signal             : %d (%s)\n", signo, ::strsignal(signo));
    emit(e, log_fd, "Fault address      : %p\n",      si->si_addr);
    if (ip)
        emit(e, log_fd, "Instruction pointer: %p\n",  ip);
    emit(e, log_fd, "Binary             : %s\n\n",    binary);

    // ── Backtrace ─────────────────────────────────────────────────────────────
    emit(e, log_fd, "Backtrace (%d frames):\n", n_frames);

    char** bt_syms = ::backtrace_symbols(frames, n_frames);

    for (int i = 0; i < n_frames; ++i)
    {
        char func_name[512] = {};
        char location[512]  = {};

        bool resolved = addr2line_resolve(frames[i],
                                          func_name, sizeof(func_name),
                                          location,  sizeof(location));
        if (resolved)
        {
            emit(e, log_fd, "  #%02d  %s\n", i, func_name);
            if (location[0])
                emit(e, log_fd, "       %s\n", location);
        }
        else
        {
            // Fallback: raw backtrace_symbols string, demangled if possible
            const char* raw = bt_syms ? bt_syms[i] : nullptr;
            if (raw)
            {
                char mangled[256] = {};
                char dm_buf[512]  = {};
                if (extract_symbol(raw, mangled, sizeof(mangled)))
                {
                    const char* dm = demangle(mangled, dm_buf, sizeof(dm_buf));
                    emit(e, log_fd, "  #%02d  %s\n", i, dm);
                }
                else
                {
                    emit(e, log_fd, "  #%02d  %s\n", i, raw);
                }
            }
            else
            {
                emit(e, log_fd, "  #%02d  %p\n", i, frames[i]);
            }
        }
    }

    ::free(bt_syms);

    // ── Footer ────────────────────────────────────────────────────────────────
    raw_write(e, "-----END CRASH REPORT-----\n");
    if (log_fd >= 0)
    {
        raw_write(log_fd, "-----END CRASH REPORT-----\n");
        ::close(log_fd);
    }

    // ── Re-raise with default handler → core dump ─────────────────────────────
    struct sigaction sa = {};
    ::sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_DFL;
    ::sigaction(signo, &sa, nullptr);
    ::raise(signo);
}

// ─── Public API ──────────────────────────────────────────────────────────────

void setup_crash_altstack() noexcept
{
    stack_t ss = {};
    ss.ss_sp    = g_altstack;
    ss.ss_size  = sizeof(g_altstack);
    ss.ss_flags = 0;
    ::sigaltstack(&ss, nullptr);
}

void set_crash_log_path(std::string_view log_file) noexcept
{
    if (log_file.empty())
    {
        g_log_path[0] = '\0';
        return;
    }
    size_t n = log_file.size();
    if (n >= sizeof(g_log_path))
        n = sizeof(g_log_path) - 1;
    ::memcpy(g_log_path, log_file.data(), n);
    g_log_path[n] = '\0';
}

void install_crash_handler(std::string_view log_file) noexcept
{
    set_crash_log_path(log_file);
    setup_crash_altstack();

    struct sigaction sa = {};
    ::sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;

    static constexpr int kCrashSignals[] =
        { SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGABRT };

    for (int sig : kCrashSignals)
        ::sigaction(sig, &sa, nullptr);
}

} // namespace apostol
