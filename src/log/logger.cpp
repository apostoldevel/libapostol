#include "apostol/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fmt/format.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <syslog.h>
#include <unistd.h>
#include <zlib.h>

namespace apostol
{

// ─── Level helpers ───────────────────────────────────────────────────────────

std::string_view level_name(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::emerg:
            return "emerg";
        case LogLevel::alert:
            return "alert";
        case LogLevel::crit:
            return "crit";
        case LogLevel::error:
            return "error";
        case LogLevel::warn:
            return "warn";
        case LogLevel::notice:
            return "notice";
        case LogLevel::info:
            return "info";
        case LogLevel::debug:
            return "debug";
    }
    return "unknown";
}

LogLevel level_from_string(std::string_view name)
{
    if (name == "emerg")
        return LogLevel::emerg;
    if (name == "alert")
        return LogLevel::alert;
    if (name == "crit")
        return LogLevel::crit;
    if (name == "error")
        return LogLevel::error;
    if (name == "warn" || name == "warning")
        return LogLevel::warn;
    if (name == "notice")
        return LogLevel::notice;
    if (name == "info")
        return LogLevel::info;
    if (name == "debug")
        return LogLevel::debug;
    throw std::invalid_argument(fmt::format("unknown log level: '{}'", name));
}

// ─── StderrTarget ────────────────────────────────────────────────────────────

// ANSI color codes per level — mirrors v1 level_colors[].
// Applied only when stderr is a TTY; plain text otherwise.
static std::string_view level_color(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::emerg:  return "\033[94m";   // light blue
        case LogLevel::alert:  return "\033[95m";   // light magenta
        case LogLevel::crit:   return "\033[1;91m"; // bold light red
        case LogLevel::error:  return "\033[91m";   // light red
        case LogLevel::warn:   return "\033[93m";   // light yellow
        case LogLevel::notice: return "\033[36m";   // cyan
        case LogLevel::info:   return "\033[32m";   // green
        case LogLevel::debug:  return "\033[37m";   // white
    }
    return "";
}

StderrTarget::StderrTarget() : is_tty_(::isatty(STDERR_FILENO) == 1) {}

void StderrTarget::write(LogLevel level, std::string_view message)
{
    if (is_tty_)
    {
        auto   color = level_color(level);
        // Build: COLOR + message + RESET + newline — single write for atomicity
        std::string line;
        line.reserve(color.size() + message.size() + 5);
        line += color;
        line += message;
        line += "\033[0m\n";
        [[maybe_unused]] ssize_t n = ::write(STDERR_FILENO, line.data(), line.size());
    }
    else
    {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    }
}

// ─── FileTarget ──────────────────────────────────────────────────────────────

FileTarget::FileTarget(std::string path, std::size_t max_size_bytes,
                       int max_backups, bool compress) :
    path_(std::move(path)), max_size_(max_size_bytes),
    max_backups_(max_backups), compress_(compress)
{
    open_file();
}

FileTarget::~FileTarget()
{
    if (fd_ >= 0)
        ::close(fd_);
}

void FileTarget::open_file()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }

    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ < 0)
        throw std::system_error(errno, std::system_category(), fmt::format("open log file '{}'", path_));

    struct stat st {};
    if (::fstat(fd_, &st) == 0)
        current_size_ = static_cast<std::size_t>(st.st_size);
}

void FileTarget::rotate()
{
    ::close(fd_);
    fd_ = -1;
    current_size_ = 0;

    const auto ext = compress_ ? ".gz" : "";

    // Delete the oldest backup
    auto oldest = fmt::format("{}.{}{}", path_, max_backups_, ext);
    ::unlink(oldest.c_str());

    // Shift chain: N-1 → N, N-2 → N-1, ..., 1 → 2
    for (int i = max_backups_ - 1; i >= 1; --i)
    {
        auto from = fmt::format("{}.{}{}", path_, i, ext);
        auto to   = fmt::format("{}.{}{}", path_, i + 1, ext);
        ::rename(from.c_str(), to.c_str());
    }

    // Current log → log.1 (or log.1.gz)
    if (compress_)
    {
        auto gz_path = fmt::format("{}.1.gz", path_);
        if (compress_file(path_, gz_path))
            ::unlink(path_.c_str());
        else
            ::rename(path_.c_str(), fmt::format("{}.1", path_).c_str()); // fallback
    }
    else
    {
        ::rename(path_.c_str(), fmt::format("{}.1", path_).c_str());
    }

    open_file();
}

bool FileTarget::compress_file(const std::string& src, const std::string& dst)
{
    int in_fd = ::open(src.c_str(), O_RDONLY);
    if (in_fd < 0)
        return false;

    gzFile gz = ::gzopen(dst.c_str(), "wb6");
    if (!gz)
    {
        ::close(in_fd);
        return false;
    }

    char buf[65536];
    ssize_t n;
    while ((n = ::read(in_fd, buf, sizeof(buf))) > 0)
        ::gzwrite(gz, buf, static_cast<unsigned>(n));

    ::gzclose(gz);
    ::close(in_fd);
    return n >= 0;
}

void FileTarget::write(LogLevel /*level*/, std::string_view message)
{
    if (fd_ < 0)
        return;

    if (max_size_ > 0 && current_size_ >= max_size_)
        rotate();

    // Single write call for atomicity on Linux
    std::string line = std::string(message) + '\n';
    auto written = ::write(fd_, line.data(), line.size());
    if (written > 0)
        current_size_ += static_cast<std::size_t>(written);
}

void FileTarget::flush()
{
    if (fd_ >= 0)
        ::fdatasync(fd_);
}

void FileTarget::reopen()
{
    open_file(); // close current fd (if any) and open a fresh one at path_
}

// ─── SyslogTarget ────────────────────────────────────────────────────────────

static int to_syslog_priority(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::emerg:
            return LOG_EMERG;
        case LogLevel::alert:
            return LOG_ALERT;
        case LogLevel::crit:
            return LOG_CRIT;
        case LogLevel::error:
            return LOG_ERR;
        case LogLevel::warn:
            return LOG_WARNING;
        case LogLevel::notice:
            return LOG_NOTICE;
        case LogLevel::info:
            return LOG_INFO;
        case LogLevel::debug:
            return LOG_DEBUG;
    }
    return LOG_INFO;
}

SyslogTarget::SyslogTarget(std::string ident) : ident_(std::move(ident))
{
    ::openlog(ident_.c_str(), LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

SyslogTarget::~SyslogTarget()
{
    ::closelog();
}

void SyslogTarget::write(LogLevel level, std::string_view message)
{
    ::syslog(to_syslog_priority(level), "%.*s", static_cast<int>(message.size()), message.data());
}

// ─── Logger ──────────────────────────────────────────────────────────────────

void Logger::add_target(std::unique_ptr<LogTarget> target)
{
    std::lock_guard lock(mutex_);
    targets_.push_back(std::move(target));
}

void Logger::set_file_target(std::string path, std::size_t max_size,
                             int max_backups, bool compress)
{
    std::lock_guard lock(mutex_);
    // Remove all existing FileTargets
    std::erase_if(targets_,
        [](const auto& t) { return dynamic_cast<FileTarget*>(t.get()) != nullptr; });
    targets_.push_back(std::make_unique<FileTarget>(std::move(path), max_size,
                                                     max_backups, compress));
}

std::string Logger::format_message(LogLevel level, std::string_view message)
{
    // Format mirrors v1 CLog::ErrorCore():
    //   [YYYY/MM/DD HH:MM:SS] [pid] [tid] level: message
    using namespace std::chrono;
    auto now = system_clock::now();
    auto tt  = system_clock::to_time_t(now);

    std::tm tm_buf {};
    ::localtime_r(&tt, &tm_buf);

    const pid_t pid = ::getpid();
    const pid_t tid = static_cast<pid_t>(::syscall(SYS_gettid));

    return fmt::format("[{:04d}/{:02d}/{:02d} {:02d}:{:02d}:{:02d}] [{}] [{}] {}: {}",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        pid, tid, level_name(level), message);
}

void Logger::log(LogLevel level, std::string_view message)
{
    if (level > level_)
        return;

    auto formatted = format_message(level, message);

    std::lock_guard lock(mutex_);
    for (auto& target : targets_)
        target->write(level, formatted);
}

void Logger::flush()
{
    std::lock_guard lock(mutex_);
    for (auto& target : targets_)
        target->flush();
}

void Logger::reopen()
{
    std::lock_guard lock(mutex_);
    for (auto& target : targets_)
        target->reopen();
}

// ─── Global logger ───────────────────────────────────────────────────────────

Logger& global_logger() noexcept
{
    static Logger instance;
    return instance;
}

} // namespace apostol
