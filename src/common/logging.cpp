// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <regex>
#include <thread>

#if defined(__ANDROID__)
#include <android/log.h>
#endif
#ifdef _WIN32
#include <windows.h> // For OutputDebugStringW
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <boost/algorithm/string/replace.hpp>
#include <fmt/ranges.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/literals.h"
#include "common/log_retention.h"
#include "common/log_ring.h"
#include "common/nx2_sampler.h"
#include "common/polyfill_thread.h"
#include "common/system_log.h"
#include "common/thread.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/bounded_threadsafe_queue.h"

namespace Common::Log {

/// @brief A log entry. Log entries are store in a structured format to permit more varied output
/// formatting on different frontends, as well as facilitating filtering and aggregation.
struct Entry {
    char const* message = nullptr;
    size_t message_len = 0;
    std::chrono::microseconds timestamp;
    Class log_class{};
    Level log_level{};
    const char* filename = nullptr;
    const char* function = nullptr;
    uint32_t line_num = 0;
};

namespace {

/// @brief Returns the name of the passed log class as a C-string. Subclasses are separated by periods
/// instead of underscores as in the enumeration.
/// @note GetClassName is a macro defined by Windows.h, grrr...
const char* GetLogClassName(Class log_class) {
    switch (log_class) {
#define CLS(x) case Class::x: return #x;
#define SUB(x, y) case Class::x##_##y: return #x "." #y;
#include "common/log_classes.inc"
#undef CLS
#undef SUB
    default: return "?";
    }
}

/// @brief Returns the name of the passed log level as a C-string.
const char* GetLevelName(Level log_level) {
    switch (log_level) {
#define LVL(x) case Level::x: return #x;
    LVL(Trace)
    LVL(Debug)
    LVL(Info)
    LVL(Warning)
    LVL(Error)
    LVL(Critical)
#undef LVL
    default: return "?";
    }
}

// Some IDEs prefer <file>:<line> instead, so let's just do that :)
std::string FormatLogMessage(const Entry& entry) noexcept {
    if (!entry.filename) return "";
    auto const time_seconds = uint32_t(entry.timestamp.count() / 1000000);
    auto const time_fractional = uint32_t(entry.timestamp.count() % 1000000);
    auto const class_name = GetLogClassName(entry.log_class);
    auto const level_name = GetLevelName(entry.log_level);
    return fmt::format("[{:4d}.{:06d}] {} <{}> {}:{}:{}: {}\n", time_seconds, time_fractional, class_name, level_name, entry.filename, entry.line_num, entry.function, entry.message);
}

template <typename It>
Level GetLevelByName(const It begin, const It end) {
    for (u32 i = 0; i < u32(Level::Count); ++i) {
        const char* level_name = GetLevelName(Level(i));
        if (Common::ComparePartialString(begin, end, level_name))
            return Level(i);
    }
    return Level::Count;
}

template <typename It>
Class GetClassByName(const It begin, const It end) {
    for (u32 i = 0; i < u32(Class::Count); ++i) {
        const char* level_name = GetLogClassName(Class(i));
        if (Common::ComparePartialString(begin, end, level_name))
            return Class(i);
    }
    return Class::Count;
}

template <typename Iterator>
bool ParseFilterRule(Filter& instance, Iterator begin, Iterator end) {
    auto level_separator = std::find(begin, end, ':');
    if (level_separator == end) {
        LOG_ERROR(Log, "Invalid log filter. Must specify a log level after `:`: {}", std::string(begin, end));
        return false;
    }
    const Level level = GetLevelByName(level_separator + 1, end);
    if (level == Level::Count) {
        LOG_ERROR(Log, "Unknown log level in filter: {}", std::string(begin, end));
        return false;
    }
    if (Common::ComparePartialString(begin, level_separator, "*")) {
        instance.class_levels.fill(level);
        return true;
    }
    const Class log_class = GetClassByName(begin, level_separator);
    if (log_class == Class::Count) {
        LOG_ERROR(Log, "Unknown log class in filter: {}", std::string(begin, end));
        return false;
    }
    instance.SetClassLevel(log_class, level);
    return true;
}

/// @brief Trims up to and including the last of ../, ..\, src/, src\ in a string
/// do not be fooled this isn't generating new strings on .rodata :)
constexpr const char* TrimSourcePath(std::string_view source) noexcept {
    const auto rfind = [source](const std::string_view match) {
        return source.rfind(match) == source.npos ? 0 : (source.rfind(match) + match.size());
    };
    auto idx = (std::max)({rfind("src/"), rfind("src\\"), rfind("../"), rfind("..\\")});
    return source.data() + idx;
}

/// @brief Interface for logging backends.
struct Backend {
    virtual ~Backend() noexcept = default;
    virtual void Write(const Entry& entry) noexcept = 0;
    virtual void Flush() noexcept = 0;
};

/// @brief Instead of using fmt::format() just use the system's formatting capabilities directly
struct DirectFormatArgs {
    const char *class_name;
    const char *level_name;
    uint32_t time_seconds;
    uint32_t time_fractional;
};
[[nodiscard]] inline DirectFormatArgs GetDirectFormatArgs(Entry const& entry) noexcept {
    return {
        .class_name = GetLogClassName(entry.log_class),
        .level_name = GetLevelName(entry.log_level),
        .time_seconds = uint32_t(entry.timestamp.count() / 1000000),
        .time_fractional = uint32_t(entry.timestamp.count() % 1000000),
    };
}

/// @brief Backend that writes to stdout and with color
struct ColorConsoleBackend final : public Backend {
#ifdef _WIN32
    explicit ColorConsoleBackend() noexcept {
        console_handle = GetStdHandle(STD_ERROR_HANDLE);
        GetConsoleScreenBufferInfo(console_handle, &original_info);
    }
    ~ColorConsoleBackend() noexcept override {
        SetConsoleTextAttribute(console_handle, original_info.wAttributes);
    }
    void Write(const Entry& entry) noexcept override {
        if (enabled && console_handle != INVALID_HANDLE_VALUE) {
            WORD color = WORD([&entry]() {
                switch (entry.log_level) {
                case Level::Debug: return FOREGROUND_GREEN | FOREGROUND_BLUE; // Cyan
                case Level::Info: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // Bright gray
                case Level::Warning: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
                case Level::Error: return FOREGROUND_RED | FOREGROUND_INTENSITY;
                case Level::Critical: return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
                default: break;
                }
                return FOREGROUND_INTENSITY; // Grey
            }());
            SetConsoleTextAttribute(console_handle, color);
            auto const df = GetDirectFormatArgs(entry);
            // One write per line: MinGW's fprintf emits each character separately, and stdout is
            // unbuffered here, so every character became a WriteFile on the thread that logs.
            fmt::memory_buffer line;
            fmt::format_to(fmt::appender(line), "[{:4d}.{:06d}] {} <{}> {}:{}:{}: ", df.time_seconds, df.time_fractional, df.class_name, df.level_name, entry.filename, entry.line_num, entry.function);
            line.append(entry.message, entry.message + entry.message_len);
            line.push_back('\n');
            std::fwrite(line.data(), 1, line.size(), stdout);
        }
    }
    void Flush() noexcept override {}
    CONSOLE_SCREEN_BUFFER_INFO original_info = {};
    HANDLE console_handle = INVALID_HANDLE_VALUE;
    std::atomic_bool enabled = false;
#else // ^^^ Windows vvv POSIX
    explicit ColorConsoleBackend() noexcept {}
    ~ColorConsoleBackend() noexcept override {}
    void Write(const Entry& entry) noexcept override {
        if (enabled) {
            auto const color_str = [&entry]() -> const char* {
                switch (entry.log_level) {
                case Level::Debug: return "[0;36m"; // Cyan
                case Level::Info: return "[0;37m"; // Bright gray
                case Level::Warning: return "[1;33m"; // Bright yellow
                case Level::Error: return "[1;31m"; // Bright red
                case Level::Critical: return "[1;35m"; // Bright magenta
                default: return "[1;30m"; // Grey
                }
            }();
            auto const df = GetDirectFormatArgs(entry);
            // more restrictive, because take for example this simple prelude:
            // [  50.872256] Config <Info> common/settings.cpp:142:LogSettings:
            char buffer[256];
            auto result = fmt::format_to_n(buffer, sizeof(buffer) - 1, "\x1b{}[{:4d}.{:06d}] {} <{}> {}:{}:{}: ", color_str, df.time_seconds, df.time_fractional, df.class_name, df.level_name, entry.filename, entry.line_num, entry.function, entry.message);
            std::fwrite(buffer, 1, (std::min)(sizeof(buffer) - 1, result.size), stdout);
            std::fwrite(entry.message, 1, entry.message_len, stdout);
            std::fwrite("\x1b[0m\n", 1, sizeof("\x1b[0m\n"), stdout);
        }
    }
    void Flush() noexcept override {}
    std::atomic_bool enabled = false;
#endif
};

#ifndef __OPENORBIS__
/// Set while this thread is inside the file backend. The file raises its own errors through the
/// logger (a failed flush logs one) and the mutex is not recursive: such a message reaches every
/// other backend, but is not written to the file again.
struct FileReentryGuard {
    FileReentryGuard() noexcept : reentered{active} {
        active = true;
    }
    ~FileReentryGuard() {
        if (!reentered) {
            active = false;
        }
    }
    FileReentryGuard(const FileReentryGuard&) = delete;
    FileReentryGuard& operator=(const FileReentryGuard&) = delete;

    static inline thread_local bool active = false;
    const bool reentered;
};

/// @brief Backend that writes to a file passed into the constructor
struct FileBackend final : public Backend {
    explicit FileBackend(const std::filesystem::path filename) noexcept {
        // Archive the previous session's log as eden_log.<stamp>.txt (the newest 5 are kept, see
        // log_retention.h). The rename fails if the log is still open elsewhere (e.g. in an
        // emulator process that hasn't exited yet). A log that can't even be checked counts as a
        // failure, so it is never truncated. Android's share-log button reads the one fixed
        // "<log>.old.txt" name, so there the previous scheme stays.
        const auto rotate = [this](const std::filesystem::path& path) {
#ifdef __ANDROID__
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
                return !ec;
            auto old_path = path;
            old_path += ".old.txt";
            std::filesystem::rename(path, old_path, ec);
            return !ec;
#else
            auto result = Retention::Rotate(path);
            if (result.archived) {
                retention_notes.push_back(fmt::format(
                    "Log retention: previous session log kept as {}",
                    result.archived->filename().string()));
                needs_compression = true;
            }
            if (result.migrated) {
                retention_notes.push_back(fmt::format(
                    "Log retention: the old .old.txt log was renamed to {}",
                    result.migrated->filename().string()));
                needs_compression = true;
            }
            if (!result.deleted.empty()) {
                std::string names;
                for (const auto& deleted : result.deleted) {
                    names += (names.empty() ? "" : ", ") + deleted.filename().string();
                }
                retention_notes.push_back(
                    fmt::format("Log retention: kept the newest {} session logs, deleted {}",
                                Retention::KeepArchives, names));
            }
            return result.ok;
#endif
        };
        // If the log can't be moved, write this session to a second log (eden_log_2.txt) so that
        // the previous session is kept and this one still gets the full size limit. If that one
        // can't be moved either, append to it.
        auto path = filename;
        auto mode = FS::FileAccessMode::Write;
        if (!rotate(path)) {
            path = filename.parent_path() / filename.stem();
            path += "_2";
            path += filename.extension();
            if (!rotate(path))
                mode = FS::FileAccessMode::Append;
        }
        log_path = path;
        file.emplace(path, mode, FS::FileType::TextFile);
        // Count what is already there so the size limit still applies when appending.
        if (mode == FS::FileAccessMode::Append)
            bytes_written.store(file->GetSize(), std::memory_order_relaxed);
    }
    ~FileBackend() noexcept override = default;

    void Write(const Entry& entry) noexcept override {
        const FileReentryGuard guard;
        if (guard.reentered) {
            return;
        }
        std::scoped_lock lock{write_mutex};
        WriteUnlocked(entry);
    }

    void WriteFromCrash(const Entry& entry) noexcept {
        const FileReentryGuard guard;
        if (guard.reentered) {
            return;
        }
        std::unique_lock lock{write_mutex, std::try_to_lock};
        if (!lock.owns_lock()) {
            return;
        }
        WriteUnlocked(entry);
        FlushUnlocked();
    }

    void Flush() noexcept override {
        const FileReentryGuard guard;
        if (guard.reentered) {
            return;
        }
        std::scoped_lock lock{write_mutex};
        FlushUnlocked();
    }

    /// What the rotation at construction did (logged once the logger is up) and whether it left
    /// archives that the retention thread should compress.
    std::vector<std::string> retention_notes;
    bool needs_compression = false;
    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return log_path;
    }
    [[nodiscard]] std::size_t BytesWritten() const noexcept {
        return bytes_written.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool LimitReached() const noexcept {
        return limit_reached.load(std::memory_order_relaxed);
    }

    /// The size at which the file stops growing.
    [[nodiscard]] static std::size_t WriteLimit() noexcept {
        using namespace Common::Literals;
        static const bool dependency_trace = Nx2Sampler::DependencyTraceEnabled();
        return (Settings::values.extended_logging.GetValue() || dependency_trace) ? 1_GiB : 100_MiB;
    }
private:
    void WriteUnlocked(const Entry& entry) noexcept {
        if (!enabled.load(std::memory_order_relaxed))
            return;

        auto message = FormatLogMessage(entry);
#ifndef __ANDROID__
        if (Settings::values.censor_username.GetValue()) {
            // This must be a static otherwise it would get checked on EVERY
            // instance of logging an entry...
            static std::string username = []() -> std::string {
                // in order of precedence
                // LOGNAME usually works on UNIX, USERNAME on Windows
                // Some UNIX systems suck and don't use LOGNAME so we also need USER :(
                for (auto const var : { "LOGNAME", "USERNAME", "USER", })
                    if (auto const s = ::getenv(var); s != nullptr)
                        return std::string{s};
                return std::string{};
            }();
            if (!username.empty())
                boost::replace_all(message, username, "user");
        }
#endif
        bytes_written += file->WriteSpan(std::span<const char>{message.begin(), message.end()});

        // Option to log each line rather than 4k buffers
        if (Settings::values.log_flush_line.GetValue())
            FlushUnlocked();

        // Prevent logs from exceeding a set maximum size in the event that log entries are spammed.
        const auto write_limit = WriteLimit();
        const bool write_limit_exceeded = bytes_written.load(std::memory_order_relaxed) > write_limit;
        if (entry.log_level >= Level::Error || write_limit_exceeded) {
            // Stop writing after the write limit is exceeded.
            // Don't close the file so we can print a stacktrace if necessary
            if (write_limit_exceeded) {
                enabled.store(false, std::memory_order_relaxed);
                // Without this line the file just ends, and a reader cannot tell a quiet session
                // from a full file.
                if (!limit_reached.exchange(true)) {
                    const auto marker = fmt::format(
                        "[{:4d}.{:06d}] Log <Warning> common/logging.cpp:{}:Write: Log file size "
                        "limit of {} bytes reached; later lines of this session are not written "
                        "to this file\n",
                        uint32_t(entry.timestamp.count() / 1000000),
                        uint32_t(entry.timestamp.count() % 1000000), __LINE__, write_limit);
                    static_cast<void>(
                        file->WriteSpan(std::span<const char>{marker.begin(), marker.end()}));
                }
            }
            FlushUnlocked();
        }
    }

    void FlushUnlocked() noexcept {
        file->Flush();
    }

    std::mutex write_mutex;
    std::optional<FS::IOFile> file;
    std::filesystem::path log_path;
    std::atomic<std::size_t> bytes_written{0};
    std::atomic<bool> limit_reached{false};
    std::atomic<bool> enabled{true};
};
#endif
/// @brief Backend that keeps the last lines in memory, off unless the debug server turns it on
struct RingBackend final : public Backend {
    void Write(const Entry& entry) noexcept override {
        if (capacity.load(std::memory_order_relaxed) == 0) {
            return;
        }
        try {
            std::string text = FormatLogMessage(entry);
            if (!text.empty() && text.back() == '\n') {
                text.pop_back();
            }
            std::scoped_lock lock{mutex};
            lines.push_back({next_sequence++, std::move(text)});
            while (lines.size() > capacity.load(std::memory_order_relaxed)) {
                lines.pop_front();
                ++evicted;
            }
        } catch (const std::exception&) {
            // Out of memory: drop the line rather than the process.
        }
    }
    void Flush() noexcept override {}

    std::atomic<std::size_t> capacity{};
    std::mutex mutex;
    std::deque<RingLine> lines;
    u64 next_sequence = 1;
    /// Lines dropped to make room; guarded by mutex.
    u64 evicted = 0;
};
#ifdef _WIN32
/// @brief Backend that writes to Visual Studio's output window
struct DebuggerBackend final : public Backend {
    explicit DebuggerBackend() noexcept = default;
    ~DebuggerBackend() noexcept override = default;
    void Write(const Entry& entry) noexcept override {
        ::OutputDebugStringW(UTF8ToUTF16W(FormatLogMessage(entry)).c_str());
    }
    void Flush() noexcept override {}
};
#endif
#ifdef __ANDROID__
/// @brief Backend that writes to the Android logcat
struct LogcatBackend : public Backend {
    explicit LogcatBackend() noexcept = default;
    ~LogcatBackend() noexcept override = default;
    void Write(const Entry& entry) noexcept override {
        android_LogPriority android_log_priority = [&]() {
            switch (entry.log_level) {
            case Level::Debug: return ANDROID_LOG_DEBUG;
            case Level::Info: return ANDROID_LOG_INFO;
            case Level::Warning: return ANDROID_LOG_WARN;
            case Level::Error: return ANDROID_LOG_ERROR;
            case Level::Critical: return ANDROID_LOG_FATAL;
            case Level::Count:
            case Level::Trace: return ANDROID_LOG_VERBOSE;
            }
        }();
        auto const df = GetDirectFormatArgs(entry);
        __android_log_print(android_log_priority, "YuzuNative", "%s %s:%u:%s: %s", df.class_name, entry.filename, entry.line_num, entry.function, entry.message);
    }
    void Flush() noexcept override {}
};
#endif

/// @brief Static state as a singleton.
struct Impl {
    // Well, I mean it's the default constructor!
    explicit Impl() noexcept : filter(Level::Trace) {}

    template<typename F>
    void ForEachBackend(F&& lambda) noexcept {
        lambda(static_cast<Backend&>(color_console_backend));
        lambda(static_cast<Backend&>(ring_backend));
#ifndef __OPENORBIS__
        if (file_backend)
            lambda(static_cast<Backend&>(*file_backend));
#endif
#ifdef _WIN32
        lambda(static_cast<Backend&>(debugger_backend));
#endif
#ifdef __ANDROID__
        lambda(static_cast<Backend&>(lc_backend));
#endif
    }

    Filter filter;
    ColorConsoleBackend color_console_backend{};
    RingBackend ring_backend{};
#ifndef __OPENORBIS__
    std::optional<FileBackend> file_backend;
#endif
#ifdef _WIN32
    DebuggerBackend debugger_backend{};
#endif
#ifdef __ANDROID__
    LogcatBackend lc_backend{};
#endif
    std::chrono::steady_clock::time_point time_origin{std::chrono::steady_clock::now()};
#ifndef __OPENORBIS__
    // Last, so it stops before the file backend it flushes is destroyed.
    std::jthread file_flush_thread;
    // After it: stops and is joined first, while the file backend its logging goes to still lives.
    std::jthread retention_thread;
#endif
};
} // Anonymous namespace

void Filter::ParseFilterString(std::string_view filter_view) {
    auto clause_begin = filter_view.cbegin();
    while (clause_begin < filter_view.cend()) {
        auto clause_end = std::find(clause_begin, filter_view.cend(), ' ');
        // If clause isn't empty
        if (clause_end != clause_begin) {
            ParseFilterRule(*this, clause_begin, clause_end);
        }
        if (clause_end != filter_view.cend()) {
            // Skip over the whitespace
            ++clause_end;
        }
        clause_begin = clause_end;
    }
}

// Constructor shall NOT depend upon Settings() or whatever
// it's ran at global static ctor() time... so BE CAREFUL MFER!
static std::optional<Common::Log::Impl> logging_instance{};
/// See SetExternalSink. Loaded once per message that passes the filter; nullptr when unset.
static std::atomic<Common::Log::ExternalSink> external_sink{nullptr};
/// See GetLogCounts: the messages of those levels that passed the filter.
static std::atomic<u64> error_log_count{0};
static std::atomic<u64> critical_log_count{0};

void SetExternalSink(ExternalSink sink) {
    external_sink.store(sink, std::memory_order_release);
}

LogCounts GetLogCounts() noexcept {
    return {.error = error_log_count.load(std::memory_order_relaxed),
            .critical = critical_log_count.load(std::memory_order_relaxed)};
}

namespace {
struct RecentErrors {
    std::mutex mutex;
    std::array<RecentError, RecentErrorSlots> slots{};
    u64 next_stamp{1};
};
// Leaked: messages are logged during static destruction too.
RecentErrors& Recent() {
    static auto* recent = new RecentErrors;
    return *recent;
}

void Assign(char* destination, std::size_t capacity, std::string_view text) noexcept {
    const std::size_t length = (std::min)(text.size(), capacity - 1);
    std::memcpy(destination, text.data(), length);
    destination[length] = '\0';
}
} // namespace

/// Remembers an Error or Critical message for GetRecentErrors: one slot per source line.
static void NoteRecentError(const Entry& e) {
    std::string_view file = e.filename ? e.filename : "?";
    if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
        file.remove_prefix(slash + 1);
    }
    char site[sizeof(RecentError::site)];
    const auto written = fmt::format_to_n(site, sizeof(site) - 1, "{}:{}", file, e.line_num);
    const std::string_view key{site, (std::min)(written.size, sizeof(site) - 1)};
    auto& recent = Recent();
    std::scoped_lock lock{recent.mutex};
    RecentError* slot = nullptr;
    for (RecentError& candidate : recent.slots) {
        if (candidate.stamp != 0 && key == candidate.site) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        // The oldest one (an unused slot has stamp 0) is replaced.
        slot = &*std::min_element(
            recent.slots.begin(), recent.slots.end(),
            [](const RecentError& a, const RecentError& b) { return a.stamp < b.stamp; });
        *slot = {};
        Assign(slot->site, sizeof(slot->site), key);
    }
    Assign(slot->message, sizeof(slot->message), {e.message, e.message_len});
    ++slot->count;
    slot->last_seconds = static_cast<double>(e.timestamp.count()) / 1e6;
    slot->critical = e.log_level == Level::Critical;
    slot->stamp = recent.next_stamp++;
}

std::size_t GetRecentErrors(RecentError* out, std::size_t max, bool lock) noexcept {
    auto& recent = Recent();
    std::unique_lock guard{recent.mutex, std::defer_lock};
    if (lock) {
        guard.lock();
    }
    std::array<const RecentError*, RecentErrorSlots> used{};
    std::size_t count = 0;
    for (const RecentError& slot : recent.slots) {
        if (slot.stamp != 0) {
            used[count++] = &slot;
        }
    }
    std::sort(used.begin(), used.begin() + count,
              [](const RecentError* a, const RecentError* b) { return a->stamp < b->stamp; });
    // The newest `max`, oldest first.
    const std::size_t first = count > max ? count - max : 0;
    for (std::size_t i = first; i < count; ++i) {
        out[i - first] = *used[i];
    }
    return count - first;
}

void Initialize() {
    if (logging_instance) {
        LOG_WARNING(Log, "Reinitializing logging backend");
    } else {
        logging_instance.emplace();
        logging_instance->filter.ParseFilterString(Settings::values.log_filter.GetValue());
#ifndef __OPENORBIS__
        using namespace Common::FS;
        const auto& log_dir = GetEdenPath(EdenPath::LogDir);
        void(CreateDir(log_dir));
        logging_instance->file_backend.emplace(log_dir / LOG_FILE);
        for (const auto& note : logging_instance->file_backend->retention_notes)
            LOG_INFO(Log, "{}", note);
        if (logging_instance->file_backend->needs_compression) {
            // Compress the older archives off the boot path, at background priority; the previous
            // session's log stays plain text. Joined (and stopped between blocks) at exit.
            logging_instance->retention_thread = std::jthread(
                [log_path = logging_instance->file_backend->Path()](std::stop_token token) {
                    Common::SetCurrentThreadName("LogRetention");
#ifdef _WIN32
                    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
                    const auto count = Retention::CompressOlderArchives(log_path, token);
                    if (count != 0)
                        LOG_INFO(Log, "Log retention: compressed {} older session log(s) to .zst",
                                 count);
                });
        }
#endif
    }
}

void Start() {
#ifndef __OPENORBIS__
    // Lines below Error wait in the file's buffer until it fills. Flushing it every second keeps a
    // hang or a kill from losing more than the last second of the log.
    if (!logging_instance || !logging_instance->file_backend || logging_instance->file_flush_thread.joinable())
        return;
    logging_instance->file_flush_thread = std::jthread([&backend = *logging_instance->file_backend](std::stop_token token) {
        Common::SetCurrentThreadName("LogFlush");
        while (Common::StoppableTimedWait(token, std::chrono::seconds{1}))
            backend.Flush();
    });
#endif
}

void Stop() {
    if (logging_instance)
        logging_instance->ForEachBackend([](Backend& backend) { backend.Flush(); });
}

void Shutdown() {
    if (!logging_instance)
        return;
#ifndef __OPENORBIS__
    // The retention worker logs through the singleton, so stop and join both workers while it is
    // still fully engaged; only then is it safe to tear down.
    for (std::jthread* worker : {&logging_instance->retention_thread, &logging_instance->file_flush_thread}) {
        worker->request_stop();
    }
    for (std::jthread* worker : {&logging_instance->retention_thread, &logging_instance->file_flush_thread}) {
        if (worker->joinable())
            worker->join();
    }
#endif
    Stop();
    logging_instance.reset();
}

void FmtCrashMessageImpl(const char* filename, unsigned int line_num, const char* function, fmt::string_view format, const fmt::format_args& args) noexcept {
#ifndef __OPENORBIS__
    if (!logging_instance || !logging_instance->file_backend)
        return;
    char buffer[2048];
    const auto result = fmt::vformat_to_n(buffer, sizeof(buffer) - 1, format, args);
    const std::size_t length = (std::min)(result.size, sizeof(buffer) - 1);
    buffer[length] = '\0';
    const Entry e{
        .message = buffer,
        .message_len = length,
        .timestamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - logging_instance->time_origin),
        .log_class = Class::Common,
        .log_level = Level::Critical,
        .filename = TrimSourcePath(filename),
        .function = function,
        .line_num = line_num,
    };
    logging_instance->file_backend->WriteFromCrash(e);
#endif
}

void SetGlobalFilter(const Filter& filter) {
    if (logging_instance)
        logging_instance->filter = filter;
}

void SetColorConsoleBackendEnabled(bool enabled) {
    if (logging_instance)
        logging_instance->color_console_backend.enabled = enabled;
}

void SetRingCapacity(std::size_t capacity) {
    if (!logging_instance)
        return;
    auto& ring = logging_instance->ring_backend;
    std::scoped_lock lock{ring.mutex};
    ring.capacity.store(capacity, std::memory_order_relaxed);
    while (ring.lines.size() > capacity) {
        ring.lines.pop_front();
        ++ring.evicted;
    }
}

std::size_t RingCapacity() {
    return logging_instance ? logging_instance->ring_backend.capacity.load(std::memory_order_relaxed)
                            : 0;
}

u64 RingNewestSequence() {
    if (!logging_instance)
        return 0;
    auto& ring = logging_instance->ring_backend;
    std::scoped_lock lock{ring.mutex};
    return ring.next_sequence - 1;
}

std::vector<RingLine> ReadRing(u64 after) {
    std::vector<RingLine> result;
    if (!logging_instance)
        return result;
    auto& ring = logging_instance->ring_backend;
    std::scoped_lock lock{ring.mutex};
    for (const RingLine& line : ring.lines) {
        if (line.sequence > after) {
            result.push_back(line);
        }
    }
    return result;
}

RingRead ReadRingWithStatus(u64 after) {
    RingRead result{};
    if (!logging_instance)
        return result;
    auto& ring = logging_instance->ring_backend;
    std::scoped_lock lock{ring.mutex};
    for (const RingLine& line : ring.lines) {
        if (line.sequence > after) {
            result.lines.push_back(line);
        }
    }
    result.status = {
        .capacity = ring.capacity.load(std::memory_order_relaxed),
        .newest = ring.next_sequence - 1,
        .oldest = ring.lines.empty() ? ring.next_sequence : ring.lines.front().sequence,
        .evicted = ring.evicted,
    };
    return result;
}

FileStatus GetFileStatus() {
    FileStatus status{};
#ifndef __OPENORBIS__
    if (logging_instance && logging_instance->file_backend) {
        const FileBackend& file = *logging_instance->file_backend;
        status.open = true;
        status.path = FS::PathToUTF8String(file.Path());
        status.bytes_written = file.BytesWritten();
        status.limit = FileBackend::WriteLimit();
        status.limit_reached = file.LimitReached();
    }
#endif
    return status;
}

bool IsLevelEnabled(Class log_class, Level log_level) {
    return logging_instance && logging_instance->filter.CheckMessage(log_class, log_level);
}

/// Formats a message that passes the filter and writes it to every backend and the external sink.
void FmtLogMessageImpl(Class log_class, Level log_level, const char* filename, unsigned int line_num, const char* function, fmt::string_view format, const fmt::format_args& args) {
    if (logging_instance && logging_instance->filter.CheckMessage(log_class, log_level)) {
        // Counted once, after the filter and before any backend sees the message.
        if (log_level == Level::Error) {
            error_log_count.fetch_add(1, std::memory_order_relaxed);
        } else if (log_level == Level::Critical) {
            critical_log_count.fetch_add(1, std::memory_order_relaxed);
        }
        auto const flush = ::Settings::values.log_flush_line.GetValue();
        char buffer[BUFSIZ];
        auto result = fmt::vformat_to_n(buffer, sizeof(buffer) - 1, format, args);
        Entry e{
            .message = nullptr,
            .message_len = 0,
            .timestamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - logging_instance->time_origin),
            .log_class = log_class,
            .log_level = log_level,
            .filename = TrimSourcePath(filename),
            .function = function,
            .line_num = line_num,
        };
        const auto sink = external_sink.load(std::memory_order_acquire);
        if (result.size <= sizeof(buffer) - 1) {
            buffer[(std::min)(result.size, sizeof(buffer) - 1)] = '\0';
            e.message = buffer;
            e.message_len = (std::min)(result.size, sizeof(buffer) - 1);
            logging_instance->ForEachBackend([=](Backend& backend) {
                backend.Write(e);
                if (flush) backend.Flush();
            });
            if (sink) sink(GetLogClassName(log_class), log_level, {e.message, e.message_len});
            if (log_level >= Level::Error) NoteRecentError(e);
        } else {
            std::string s = fmt::vformat(format, args);
            e.message = s.c_str();
            e.message_len = s.size();
            logging_instance->ForEachBackend([=](Backend& backend) {
                backend.Write(e);
                if (flush) backend.Flush();
            });
            if (sink) sink(GetLogClassName(log_class), log_level, s);
            if (log_level >= Level::Error) NoteRecentError(e);
        }
        if (log_level >= Level::Warning) {
            // Only where the message came from, never its text: it can carry values and dumps
            // read from guest memory, and this record goes into crash tails and exported bundles.
            const auto sev = log_level == Level::Critical ? SystemLog::Severity::Critical
                           : log_level == Level::Error    ? SystemLog::Severity::Error
                                                          : SystemLog::Severity::Warning;
            const auto kind = log_level == Level::Critical ? SystemLog::EventKind::LogCritical
                            : log_level == Level::Error    ? SystemLog::EventKind::LogError
                                                           : SystemLog::EventKind::LogWarning;
            std::string_view site = e.filename ? e.filename : "?";
            if (const auto slash = site.find_last_of("/\\"); slash != std::string_view::npos) {
                site.remove_prefix(slash + 1);
            }
            char detail_buf[96];
            const auto detail_res =
                fmt::format_to_n(detail_buf, sizeof(detail_buf) - 1, "{}:{}:{}",
                                 GetLogClassName(log_class), site, line_num);
            const std::size_t detail_len = (std::min)(detail_res.size, sizeof(detail_buf) - 1);
            SystemLog::Record(SystemLog::Subsystem::Log, sev, kind, static_cast<u64>(log_class),
                              line_num, 0, 0, std::string_view{detail_buf, detail_len});
        }
    }
}
} // namespace Common::Log
