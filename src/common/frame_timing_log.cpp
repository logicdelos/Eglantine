// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/frame_timing_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common::FrameTiming {

namespace {

constexpr std::string_view RequiredSuffix = ".jsonl";
constexpr std::string_view PartialSuffix = ".partial";

// One log per Start. Two atomics per sample: the frame id and the time, where a time of zero
// means "claimed but not written yet", so a Record that races with Stop is counted as lost
// instead of being read half written. Logs are never freed before the process ends: a Record that
// already loaded the pointer of an earlier log must not write into freed memory.
struct Log {
    u64 capacity = 0;
    Source source = Source::HostPresent;
    std::string path;
    std::unique_ptr<std::atomic<u64>[]> slots;
    std::atomic<u64> next{0};
};

std::atomic<bool> g_active{false};
std::atomic<Log*> g_log{nullptr};
std::mutex g_mutex;                            // guards Start, Stop, GetSummary and the two below
std::vector<std::unique_ptr<Log>> g_logs;
Summary g_summary;

u64 NowNs() noexcept {
    const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
    const u64 value = ns > 0 ? static_cast<u64>(ns) : 1ULL;
    return value;
}

std::filesystem::path ToPath(const std::string& utf8) {
    const std::u8string text(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path{text};
}

bool EndsWith(const std::string& text, std::string_view suffix) {
    return text.size() > suffix.size() &&
           std::string_view{text}.substr(text.size() - suffix.size()) == suffix;
}

const char* SourceName(Source source) {
    return source == Source::HostPresent ? "host_present" : "replay_frame";
}

} // Anonymous namespace

namespace detail {

// One atomic check-and-rename on Windows (MoveFileExW without MOVEFILE_REPLACE_EXISTING). On other
// systems link() fails with EEXIST atomically, and the partial name is removed afterwards; a crash
// between the two leaves both names, never a replaced file.
PublishResult PublishNoReplace(const std::filesystem::path& partial,
                               const std::filesystem::path& final_path) noexcept {
#ifdef _WIN32
    if (MoveFileExW(partial.c_str(), final_path.c_str(), 0) != 0) {
        return PublishResult::Published;
    }
    const DWORD last_error = GetLastError();
    return last_error == ERROR_ALREADY_EXISTS || last_error == ERROR_FILE_EXISTS
               ? PublishResult::Exists
               : PublishResult::Failed;
#else
    std::error_code ec;
    std::filesystem::create_hard_link(partial, final_path, ec);
    if (ec) {
        return ec == std::errc::file_exists ? PublishResult::Exists : PublishResult::Failed;
    }
    std::filesystem::remove(partial, ec);
    return ec ? PublishResult::Failed : PublishResult::Published;
#endif
}

} // namespace detail

namespace detail {

bool Active() noexcept {
    return g_active.load(std::memory_order_relaxed);
}

void RecordSlow(u64 frame_id) noexcept {
    Log* const log = g_log.load(std::memory_order_acquire);
    if (log == nullptr) {
        return;
    }
    const u64 now = NowNs();
    const u64 index = log->next.fetch_add(1, std::memory_order_relaxed);
    if (index >= log->capacity) {
        return;
    }
    log->slots[index * 2].store(frame_id, std::memory_order_relaxed);
    log->slots[index * 2 + 1].store(now, std::memory_order_release);
}

} // namespace detail

bool Start(const Config& config, std::string& error) {
    error.clear();
    if (!EndsWith(config.path, RequiredSuffix)) {
        error = "the frame timing path must end in .jsonl";
        return false;
    }
    if (config.capacity == 0 || config.capacity > MaxCapacity) {
        error = "the frame timing capacity is out of range";
        return false;
    }
    if (config.source != Source::HostPresent && config.source != Source::ReplayFrame) {
        error = "the frame timing source is unknown";
        return false;
    }

    std::lock_guard lock{g_mutex};
    if (g_summary.started && !g_summary.stopped) {
        error = "a frame timing log is already running";
        return false;
    }
    try {
        const std::filesystem::path final_path = ToPath(config.path);
        std::error_code ec;
        if (std::filesystem::exists(final_path, ec)) {
            error = "the frame timing file already exists";
            return false;
        }
        const std::filesystem::path parent = final_path.parent_path();
        if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
            error = "the frame timing directory does not exist";
            return false;
        }

        auto log = std::make_unique<Log>();
        log->capacity = config.capacity;
        log->source = config.source;
        log->path = config.path;
        const std::size_t slot_count = static_cast<std::size_t>(config.capacity) * 2;
        log->slots.reset(new (std::nothrow) std::atomic<u64>[slot_count]);
        if (!log->slots) {
            error = "could not allocate the frame timing log";
            return false;
        }
        for (std::size_t i = 0; i < slot_count; ++i) {
            log->slots[i].store(0, std::memory_order_relaxed);
        }

        // Prove the output is writable now rather than after a whole run.
        const std::filesystem::path partial_path = ToPath(config.path + std::string{PartialSuffix});
        {
            std::ofstream probe(partial_path, std::ios::binary | std::ios::trunc);
            if (!probe) {
                error = "could not create the partial frame timing file";
                return false;
            }
        }

        Log* const raw = log.get();
        g_logs.push_back(std::move(log));
        g_summary = Summary{};
        g_summary.started = true;
        g_summary.source = config.source;
        g_log.store(raw, std::memory_order_release);
        g_active.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception&) {
        error = "could not start the frame timing log";
        return false;
    }
}

namespace {

// The caller holds g_mutex.
Summary StopLocked(bool run_ended_normally, std::string& error) {
    error.clear();
    if (!g_summary.started) {
        error = "the frame timing log was not started";
        return g_summary;
    }
    if (g_summary.stopped) {
        error = "the frame timing log was already stopped";
        return g_summary;
    }
    g_active.store(false, std::memory_order_release);
    Log& log = *g_log.load(std::memory_order_acquire);

    Summary summary = g_summary;
    summary.stopped = true;
    try {
        const u64 attempts = log.next.load(std::memory_order_acquire);
        const u64 slots = std::min(attempts, log.capacity);

        std::string text;
        text.reserve(static_cast<std::size_t>(slots) * 40 + 256);
        text += "{\"schema\":1,\"kind\":\"frame_times\",\"source\":\"";
        text += SourceName(log.source);
        text += "\",\"clock\":\"steady_ns\",\"capacity\":";
        text += std::to_string(log.capacity);
        text += "}\n";
        u64 recorded = 0;
        for (u64 i = 0; i < slots; ++i) {
            const u64 time = log.slots[i * 2 + 1].load(std::memory_order_acquire);
            if (time == 0) {
                continue; // claimed by a Record that had not finished when the log stopped
            }
            const u64 id = log.slots[i * 2].load(std::memory_order_relaxed);
            if (recorded == 0) {
                summary.first_ns = time;
            }
            summary.last_ns = time;
            ++recorded;
            text += "{\"id\":";
            text += std::to_string(id);
            text += ",\"t\":";
            text += std::to_string(time);
            text += "}\n";
        }
        summary.recorded = recorded;
        summary.lost = attempts - recorded;
        const bool lossless_normal = summary.lost == 0 && run_ended_normally;
        text += "{\"end\":true,\"recorded\":";
        text += std::to_string(summary.recorded);
        text += ",\"lost\":";
        text += std::to_string(summary.lost);
        text += ",\"complete\":";
        text += lossless_normal ? "true" : "false";
        text += "}\n";

        const std::filesystem::path final_path = ToPath(log.path);
        const std::filesystem::path partial_path = ToPath(log.path + std::string{PartialSuffix});
        {
            std::ofstream out(partial_path, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out) {
                error = "could not write the frame timing file";
            }
        }
        if (error.empty()) {
            switch (detail::PublishNoReplace(partial_path, final_path)) {
            case detail::PublishResult::Published:
                summary.file_written = true;
                break;
            case detail::PublishResult::Exists:
                error = "the frame timing file appeared while the log was running";
                break;
            case detail::PublishResult::Failed:
                error = "could not rename the partial frame timing file";
                break;
            }
        }
        summary.complete = lossless_normal && summary.file_written;
    } catch (const std::exception&) {
        error = "could not write the frame timing file";
        summary.file_written = false;
        summary.complete = false;
    }
    g_summary = summary;
    return g_summary;
}

} // Anonymous namespace

Summary Stop(bool run_ended_normally, std::string& error) {
    std::lock_guard lock{g_mutex};
    return StopLocked(run_ended_normally, error);
}

bool StopIncomplete(std::string& error) {
    // Never waits: another thread inside Stop is already writing the file, and a forced exit must
    // not hang on it.
    std::unique_lock lock{g_mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        error = "the frame timing log is being stopped by another thread";
        return false;
    }
    // StopLocked returns the kept summary when the log was never started or is already stopped;
    // that summary can say file_written from the earlier stop, so only a clean error means this
    // call wrote the file.
    const Summary summary = StopLocked(false, error);
    return error.empty() && summary.file_written;
}

Summary GetSummary() {
    std::lock_guard lock{g_mutex};
    Summary summary = g_summary;
    if (summary.started && !summary.stopped) {
        const Log* const log = g_log.load(std::memory_order_acquire);
        const u64 attempts = log->next.load(std::memory_order_relaxed);
        summary.recorded = std::min(attempts, log->capacity);
        summary.lost = attempts - summary.recorded;
    }
    return summary;
}

} // namespace Common::FrameTiming
