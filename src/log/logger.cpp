#include "apostol/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fmt/format.h>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
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

namespace
{

// flock() on the lock file for the scope of one write. held() is false when
// the lock could not be taken; the caller must not rotate then.
class FileLock
{
public:
    FileLock(int fd, int op) : fd_(fd) { lock(op); }
    ~FileLock()
    {
        if (held_)
            ::flock(fd_, LOCK_UN);
    }

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    // Converting a held lock is not atomic: it is released first, so another
    // process may run in between — the caller re-checks what it locked for.
    void lock(int op)
    {
        held_ = false;
        if (fd_ < 0)
            return;
        int rc;
        while ((rc = ::flock(fd_, op)) != 0 && errno == EINTR) {}
        held_ = rc == 0;
        if (!held_)
            error_ = errno;
    }

    bool held() const noexcept { return held_; }
    int error() const noexcept { return error_; }

private:
    int fd_;
    bool held_{false};
    int error_{0};
};

} // namespace

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
    if (lock_fd_ >= 0)
        ::close(lock_fd_);
}

void FileTarget::open_file()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }

    if (!try_open_file())
        throw std::system_error(errno, std::system_category(), fmt::format("open log file '{}'", path_));
}

// Opens path_ and switches to it; on failure keeps the current descriptor.
bool FileTarget::try_open_file()
{
    int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;

    if (fd_ >= 0)
        ::close(fd_);
    fd_ = fd;

    struct stat st {};
    if (::fstat(fd_, &st) == 0)
    {
        current_size_ = static_cast<std::size_t>(st.st_size);
        dev_ = static_cast<std::uint64_t>(st.st_dev);
        ino_ = static_cast<std::uint64_t>(st.st_ino);
    }
    return true;
}

// Re-aims fd_ at the file path_ names now — another process may have rotated
// it, or logrotate moved it, since this one last wrote — and takes the real
// size of that file: the threshold is the file's, not this process's writes.
void FileTarget::follow_path()
{
    struct stat st {};
    if (::stat(path_.c_str(), &st) == 0 &&
        static_cast<std::uint64_t>(st.st_dev) == dev_ &&
        static_cast<std::uint64_t>(st.st_ino) == ino_)
    {
        current_size_ = static_cast<std::size_t>(st.st_size);
        return;
    }

    try_open_file();
}

// The lock is taken on <path>.lock, which outlives every rotation; it is opened
// for writing, as flock() emulated over fcntl() (NFS) requires. A flock()
// belongs to the open file description, and a descriptor inherited across
// fork() shares the parent's — so each process opens its own.
int FileTarget::lock_fd()
{
    const int pid = static_cast<int>(::getpid());
    if (lock_pid_ != pid)
    {
        if (lock_fd_ >= 0)
            ::close(lock_fd_);

        lock_fd_ = ::open(fmt::format("{}.lock", path_).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        lock_errno_ = lock_fd_ < 0 ? errno : 0;
        lock_pid_ = pid;
    }
    return lock_fd_;
}

void FileTarget::shift_backups(const char* ext)
{
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
}

// Under the exclusive lock: moves the file away and starts a new one. Returns
// the staging name left to compress, or an empty string.
std::string FileTarget::rotate()
{
    if (!compress_)
    {
        shift_backups("");
        const auto backup = fmt::format("{}.1", path_);
        if (::rename(path_.c_str(), backup.c_str()) == 0 && !try_open_file())
            ::rename(backup.c_str(), path_.c_str()); // keep writing where we are
        return {};
    }

    // The inode is unique among the files that exist, so the name cannot meet
    // a staged file of another rotation, even a leftover one.
    auto staged = fmt::format("{}.rotating.{}", path_, ino_);
    if (::rename(path_.c_str(), staged.c_str()) != 0)
        return {};

    if (!try_open_file())
    {
        // Cannot start a new file: put this one back rather than write into a
        // file that is about to be compressed and removed.
        ::rename(staged.c_str(), path_.c_str());
        return {};
    }
    return staged;
}

// Outside the lock: no process writes to the staged file any more, since each
// one follows the path under the lock before it writes. The chain is shifted
// only now, so two rotations whose compressions overlap may land in the order
// they finish.
void FileTarget::finish_rotation(const std::string& staged)
{
    const auto gz_staged = staged + ".gz";
    const bool compressed = compress_file(staged, gz_staged);

    FileLock lock(lock_fd(), LOCK_EX);
    shift_backups(".gz");

    if (compressed)
    {
        ::rename(gz_staged.c_str(), fmt::format("{}.1.gz", path_).c_str());
        ::unlink(staged.c_str());
    }
    else
    {
        ::unlink(gz_staged.c_str());
        ::rename(staged.c_str(), fmt::format("{}.1", path_).c_str()); // fallback
    }
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

    // Single write call for atomicity on Linux
    std::string line = std::string(message) + '\n';
    std::string staged;
    {
        FileLock lock(lock_fd(), LOCK_SH);
        follow_path();

        if (max_size_ > 0 && current_size_ >= max_size_)
        {
            if (lock.held())
                lock.lock(LOCK_EX);

            if (!lock.held())
            {
                // Without the lock a rotation races the other processes and
                // loses their lines: let the file grow instead, and say so once.
                if (!lock_warned_)
                {
                    lock_warned_ = true;
                    auto note = fmt::format("[{}] log rotation is off: cannot lock '{}.lock': {}\n",
                                            ::getpid(), path_, std::strerror(lock_fd_ < 0 ? lock_errno_ : lock.error()));
                    [[maybe_unused]] auto n = ::write(fd_, note.data(), note.size());
                }
            }
            else
            {
                follow_path(); // another process may have rotated it meanwhile
                if (current_size_ >= max_size_)
                    staged = rotate();
            }
        }

        auto written = ::write(fd_, line.data(), line.size());
        if (written > 0)
            current_size_ += static_cast<std::size_t>(written);
    }

    if (!staged.empty())
        finish_rotation(staged);
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
