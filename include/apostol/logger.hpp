#pragma once

#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace apostol
{

enum class LogLevel : int
{
    emerg = 0,  // system is unusable
    alert = 1,  // action must be taken immediately
    crit = 2,   // critical conditions
    error = 3,  // error conditions
    warn = 4,   // warning conditions
    notice = 5, // normal but significant condition
    info = 6,   // informational messages
    debug = 7   // debug-level messages
};

std::string_view level_name(LogLevel level) noexcept;
LogLevel level_from_string(std::string_view name);

// ─── Log targets ─────────────────────────────────────────────────────────────

struct LogTarget
{
    virtual ~LogTarget() = default;
    virtual void write(LogLevel level, std::string_view message) = 0;
    virtual void flush() {}
    virtual void reopen() {} // reopen file handles (called on SIGUSR1 / log rotation)
};

struct StderrTarget : LogTarget
{
    StderrTarget();
    void write(LogLevel level, std::string_view message) override;
private:
    bool is_tty_{false};
};

struct FileTarget : LogTarget
{
    static constexpr std::uint64_t k_default_max_size = APP_DEFAULT_LOG_MAX_SIZE;
    static constexpr int k_default_max_backups = 5;

    explicit FileTarget(std::string path,
                        std::size_t max_size_bytes = k_default_max_size,
                        int max_backups = k_default_max_backups,
                        bool compress = true);
    ~FileTarget() override;

    void write(LogLevel level, std::string_view message) override;
    void flush() override;
    void reopen() override;

    void set_max_size(std::size_t bytes) noexcept { max_size_ = bytes; }
    void set_max_backups(int n) noexcept { max_backups_ = n; }
    void set_compress(bool c) noexcept { compress_ = c; }

private:
    void rotate();
    void open_file();
    static bool compress_file(const std::string& src, const std::string& dst);

    std::string path_;
    std::size_t max_size_;
    int max_backups_;
    bool compress_;
    int fd_{-1};
    std::size_t current_size_{0};
};

struct SyslogTarget : LogTarget
{
    explicit SyslogTarget(std::string ident);
    ~SyslogTarget() override;

    void write(LogLevel level, std::string_view message) override;

private:
    std::string ident_;
};

// ─── Logger ──────────────────────────────────────────────────────────────────

class Logger
{
public:
    Logger() = default;
    ~Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void add_target(std::unique_ptr<LogTarget> target);

    // Replace (or add) the FileTarget — removes all existing FileTargets first.
    // Safe to call on SIGHUP reload: path unchanged → no-op; path changed → reopen.
    void set_file_target(std::string path,
                         std::size_t max_size = FileTarget::k_default_max_size,
                         int max_backups = FileTarget::k_default_max_backups,
                         bool compress = true);

    void set_level(LogLevel level) noexcept { level_ = level; }
    LogLevel level() const noexcept { return level_; }

    void log(LogLevel level, std::string_view message);

    template<typename... Args>
    void log(LogLevel level, fmt::format_string<Args...> fmt, Args&&... args)
    {
        if (level > level_)
            return;
        log(level, fmt::format(fmt, std::forward<Args>(args)...));
    }

    // Convenience methods
    template<typename... Args>
    void emerg(fmt::format_string<Args...> fmt, Args&&... args)
    {
        log(LogLevel::emerg, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void alert(fmt::format_string<Args...> fmt, Args&&... args)
    {
        log(LogLevel::alert, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void crit(fmt::format_string<Args...> fmt, Args&&... args)
    {
        log(LogLevel::crit, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(fmt::format_string<Args...> fmt, Args&&... args)
    {
        log(LogLevel::error, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(fmt::format_string<Args...> fmt, Args&&... args)
    {
        log(LogLevel::warn, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void notice(fmt::format_string<Args...> fmt, Args&&... args)
    {
        log(LogLevel::notice, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(fmt::format_string<Args...> fmt, Args&&... args)
    {
        log(LogLevel::info, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void debug(fmt::format_string<Args...> fmt, Args&&... args)
    {
        log(LogLevel::debug, fmt, std::forward<Args>(args)...);
    }

    void flush();
    void reopen(); // reopen all FileTarget handles (log rotation support)

private:
    std::string format_message(LogLevel level, std::string_view message);

    std::vector<std::unique_ptr<LogTarget>> targets_;
    LogLevel level_{LogLevel::info};
    mutable std::mutex mutex_;
};

// ─── Global logger ───────────────────────────────────────────────────────────

Logger& global_logger() noexcept;

// Convenience free functions that forward to global_logger()
template<typename... Args>
void log_emerg(fmt::format_string<Args...> fmt, Args&&... args)
{
    global_logger().emerg(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void log_error(fmt::format_string<Args...> fmt, Args&&... args)
{
    global_logger().error(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void log_warn(fmt::format_string<Args...> fmt, Args&&... args)
{
    global_logger().warn(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void log_notice(fmt::format_string<Args...> fmt, Args&&... args)
{
    global_logger().notice(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void log_info(fmt::format_string<Args...> fmt, Args&&... args)
{
    global_logger().info(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void log_debug(fmt::format_string<Args...> fmt, Args&&... args)
{
    global_logger().debug(fmt, std::forward<Args>(args)...);
}

} // namespace apostol
