#pragma once

#include <string_view>

namespace apostol
{

// ─── Crash handler ───────────────────────────────────────────────────────────
//
// Install fatal-signal handlers (SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGABRT).
//
// On crash the handler logs a structured report to stderr and, if a log path
// has been set, appends the same report to the error log file:
//
//   -----BEGIN CRASH REPORT-----
//   2026/02/23 14:05:11 | crit | apostol: worker process (pid=12345)
//   Signal             : 11 (Segmentation fault)
//   Fault address      : 0x0000000000000008
//   Instruction pointer: 0x00005566778899aa
//   Binary             : /usr/sbin/apostol
//
//   Backtrace (9 frames):
//     #00  apostol::HttpServer::handle_request(HttpRequest const&)
//            src/net/http.cpp:312
//     #01  apostol::Application::worker_run()
//            src/core/application.cpp:690
//     ...
//   -----END CRASH REPORT-----
//
// File:line resolution requires addr2line (binutils) in PATH AND debug symbols
// (-g) in the binary. Release builds get demangled symbol names only.
//
// After logging the handler re-raises the signal with SIG_DFL so the kernel
// generates a core dump (if ulimit -c > 0).
//
// POSIX: sigaltstack is NOT inherited across fork(). Call install_crash_handler()
// (or at minimum setup_crash_altstack()) in every child process after fork.
//
// Usage:
//   // In Application::run(), right after init_logging():
//   apostol::install_crash_handler(settings_.error_log.string());
//
//   // In worker_run() / helper_run() / single_run(), after sigprocmask reset:
//   apostol::setup_crash_altstack();   // re-arm alternate stack for this child
//
//   // After config reload (SIGHUP):
//   apostol::set_crash_log_path(settings_.error_log.string());

/// Install crash signal handlers + set up the alternate signal stack.
/// Safe to call multiple times (sigaction is idempotent; altstack is re-armed).
///
/// @param log_file  Absolute path to the error log file.
///                  If empty, output goes to stderr only.
void install_crash_handler(std::string_view log_file = "") noexcept;

/// Re-arm the alternate signal stack for the calling process.
/// Call this at the start of every child process (worker, helper, single)
/// because POSIX does not inherit sigaltstack across fork().
/// The sigaction crash handlers are already inherited — only the stack needs
/// to be re-registered.
void setup_crash_altstack() noexcept;

/// Update the error log path without re-installing signal handlers.
/// Call after config reload (SIGHUP) if the log path changed.
void set_crash_log_path(std::string_view log_file) noexcept;

} // namespace apostol
