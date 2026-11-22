// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/log_retention.h"
#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/read_attribution.h"
#include "common/thread.h"
#include "core/file_sys/game_file_identity.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_integrity.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys::NupIntegrity {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view CacheHeader = "eglantine-nup-integrity 1";
constexpr const char* CacheFileName = "nup_integrity.txt";

/// Control characters would break the line format; a name with one is never matched again.
std::string Sanitize(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            c = '?';
        }
    }
    return out;
}

/// Splits at most `max_parts` tab-separated fields; the last one keeps any remaining text.
std::vector<std::string_view> SplitTabs(std::string_view line, std::size_t max_parts) {
    std::vector<std::string_view> parts;
    while (parts.size() + 1 < max_parts) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos) {
            break;
        }
        parts.push_back(line.substr(0, tab));
        line.remove_prefix(tab + 1);
    }
    parts.push_back(line);
    return parts;
}

template <typename T>
bool ParseNumber(std::string_view text, T& out) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && end == text.data() + text.size();
}

} // namespace

std::string SerializeCache(std::span<const CacheEntry> entries) {
    std::string text{CacheHeader};
    text += '\n';
    const std::size_t count = std::min(entries.size(), MaxCacheEntries);
    for (std::size_t i = 0; i < count; ++i) {
        const CacheEntry& entry = entries[i];
        text += fmt::format("file\t{}\t{}\t{}\t{}\t{}\n", entry.stamp.size, entry.stamp.mtime,
                            entry.checked_at, entry.members, Sanitize(entry.stamp.path));
        for (const BadMember& bad : entry.bad) {
            text += fmt::format("bad\t{}\t{}\n", Sanitize(bad.reason), Sanitize(bad.path));
        }
    }
    return text;
}

std::vector<CacheEntry> ParseCache(std::string_view text) {
    std::vector<CacheEntry> entries;
    bool first = true;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (first) {
            first = false;
            if (line != CacheHeader) {
                return {};
            }
            continue;
        }
        if (line.starts_with("file\t")) {
            const auto parts = SplitTabs(line, 6);
            CacheEntry entry;
            if (parts.size() != 6 || !ParseNumber(parts[1], entry.stamp.size) ||
                !ParseNumber(parts[2], entry.stamp.mtime) ||
                !ParseNumber(parts[3], entry.checked_at) || !ParseNumber(parts[4], entry.members) ||
                parts[5].empty()) {
                continue;
            }
            entry.stamp.path = std::string{parts[5]};
            entries.push_back(std::move(entry));
        } else if (line.starts_with("bad\t") && !entries.empty()) {
            const auto parts = SplitTabs(line, 3);
            if (parts.size() == 3 && !parts[2].empty()) {
                entries.back().bad.push_back({std::string{parts[2]}, std::string{parts[1]}});
            }
        }
    }
    if (entries.size() > MaxCacheEntries) {
        entries.resize(MaxCacheEntries);
    }
    return entries;
}

const CacheEntry* FindCached(std::span<const CacheEntry> entries, const FileStamp& stamp) {
    const auto it = std::ranges::find_if(
        entries, [&](const CacheEntry& entry) { return entry.stamp == stamp; });
    return it == entries.end() ? nullptr : &*it;
}

void Remember(std::vector<CacheEntry>& entries, CacheEntry entry) {
    std::erase_if(entries, [&](const CacheEntry& other) {
        return other.stamp.path == entry.stamp.path;
    });
    entries.insert(entries.begin(), std::move(entry));
    if (entries.size() > MaxCacheEntries) {
        entries.resize(MaxCacheEntries);
    }
}

std::vector<std::string> FormatBadMembers(std::string_view file_name,
                                          std::span<const BadMember> bad) {
    std::vector<std::string> lines;
    for (std::size_t first = 0; first < bad.size(); first += NamesPerWarning) {
        const std::size_t last = std::min(bad.size(), first + NamesPerWarning);
        std::string line = fmt::format("NUP integrity: {} has {} corrupt member(s), {}-{}:",
                                       file_name, bad.size(), first + 1, last);
        for (std::size_t i = first; i < last; ++i) {
            line += fmt::format("{} {} ({})", i == first ? "" : ",", bad[i].path, bad[i].reason);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::chrono::nanoseconds ReadPacer::Admit(u64 game_reads, u64 game_bytes, Clock::time_point now,
                                          u64 bytes) {
    if (!started) {
        started = true;
        sampled_at = now;
        sampled_reads = game_reads;
        sampled_bytes = game_bytes;
        next_read = now;
    } else if (now - sampled_at >= PollInterval) {
        const double seconds = std::chrono::duration<double>(now - sampled_at).count();
        const double reads = game_reads >= sampled_reads
                                 ? static_cast<double>(game_reads - sampled_reads)
                                 : 0.0;
        const double read_bytes = game_bytes >= sampled_bytes
                                      ? static_cast<double>(game_bytes - sampled_bytes)
                                      : 0.0;
        busy = reads / seconds >= BusyReadsPerSecond || read_bytes / seconds >= BusyBytesPerSecond;
        sampled_at = now;
        sampled_reads = game_reads;
        sampled_bytes = game_bytes;
    }
    if (busy) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(PollInterval);
    }
    if (now < next_read) {
        return std::min<std::chrono::nanoseconds>(
            next_read - now, std::chrono::duration_cast<std::chrono::nanoseconds>(PollInterval));
    }
    next_read = now + std::chrono::nanoseconds{static_cast<s64>(
                          static_cast<double>(bytes) * 1.0e9 / static_cast<double>(MaxBytesPerSecond))};
    return std::chrono::nanoseconds::zero();
}

namespace {

/// A read-only file with a handle of its own. It deliberately bypasses RealVfsFile, whose reads
/// are charged to the game's file-read statistics. Used by one thread only.
class DiskFile final : public VfsFile {
public:
    explicit DiskFile(const fs::path& path)
        : file{path, Common::FS::FileAccessMode::Read}, name{path.filename().string()} {
        if (file.IsOpen()) {
            size = file.GetSize();
        }
    }

    bool IsOpen() const {
        return file.IsOpen();
    }
    std::string GetName() const override {
        return name;
    }
    std::size_t GetSize() const override {
        return static_cast<std::size_t>(size);
    }
    bool Resize(std::size_t) override {
        return false;
    }
    VirtualDir GetContainingDirectory() const override {
        return nullptr;
    }
    bool IsWritable() const override {
        return false;
    }
    bool IsReadable() const override {
        return true;
    }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        if (offset >= size || !file.Seek(static_cast<s64>(offset))) {
            return 0;
        }
        length = std::min<std::size_t>(length, static_cast<std::size_t>(size - offset));
        return file.ReadSpan(std::span<u8>{data, length});
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Rename(std::string_view) override {
        return false;
    }

private:
    Common::FS::IOFile file;
    std::string name;
    u64 size{};
};

fs::path CachePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / CacheFileName;
}

std::vector<CacheEntry> LoadCache() {
    std::ifstream in{CachePath(), std::ios::binary};
    if (!in) {
        return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return ParseCache(text.str());
}

void SaveCache(const std::vector<CacheEntry>& entries) {
    const fs::path target = CachePath();
    fs::path temp = target;
    temp += ".part";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    {
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        if (!out) {
            return;
        }
        out << SerializeCache(entries);
        if (!out) {
            out.close();
            fs::remove(temp, ec);
            return;
        }
    }
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(temp, ec);
    }
}

bool StampOf(const fs::path& path, FileStamp& stamp) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        return false;
    }
    const auto time = fs::last_write_time(path, ec);
    if (ec) {
        return false;
    }
    stamp.path = Common::FS::PathToUTF8String(path);
    stamp.size = static_cast<u64>(size);
    stamp.mtime = static_cast<s64>(time.time_since_epoch().count());
    return true;
}

void LogBadMembers(std::string_view file_name, std::span<const BadMember> bad) {
    for (const std::string& line : FormatBadMembers(file_name, bad)) {
        LOG_WARNING(Loader, "{}", line);
    }
}

std::string PackageSummary(const NupContainer& container) {
    std::size_t bases = 0;
    std::size_t updates = 0;
    std::size_t addons = 0;
    for (const auto& package : container.GetPackages()) {
        switch (package.kind) {
        case NupContainer::PackageKind::Base:
            ++bases;
            break;
        case NupContainer::PackageKind::Update:
            ++updates;
            break;
        case NupContainer::PackageKind::Addon:
            ++addons;
            break;
        }
    }
    return fmt::format("{} base, {} update, {} addon", bases, updates, addons);
}

void Run(const fs::path& path, std::stop_token stop) {
    Common::SetCurrentThreadName("NupIntegrity");
    Common::SetCurrentThreadToBackgroundWork();
#ifdef _WIN32
    // Lowers the CPU, memory and IO priority of this thread for as long as it lives.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif

    FileStamp stamp;
    if (!StampOf(path, stamp)) {
        return;
    }
    const std::string file_name = Common::FS::PathToUTF8String(path.filename());

    auto cache = LoadCache();
    if (const CacheEntry* cached = FindCached(cache, stamp)) {
        LOG_INFO(Loader,
                 "NUP integrity check skipped: {} is unchanged (size, modification time) since it was "
                 "checked on {}: {} member(s), {} corrupt",
                 file_name, Common::Log::Retention::LocalDateTime(static_cast<std::time_t>(
                                cached->checked_at)),
                 cached->members, cached->bad.size());
        LogBadMembers(file_name, cached->bad);
        return;
    }

    const auto disk = std::make_shared<DiskFile>(path);
    if (!disk->IsOpen() || !NupContainer::IsNupFile(disk)) {
        return; // Not a NUP, or not readable now: nothing to say.
    }
    const NupContainer container{disk};
    if (container.GetStatus() != Loader::ResultStatus::Success) {
        LOG_WARNING(Loader, "NUP integrity check could not read the container of {}: {}", file_name,
                    Loader::GetResultStatusString(container.GetStatus()));
        return;
    }

    LOG_INFO(Loader,
             "NUP integrity check started in the background: {}, {} member(s), {}, packages: {}; "
             "reads at most {} MiB/s at low priority and pauses while the game is reading",
             file_name, container.GetFileCount(),
             GameFileIdentity::FormatSize(stamp.size), PackageSummary(container),
             MaxBytesPerSecond / (1024 * 1024));

    using Clock = ReadPacer::Clock;
    const auto started = Clock::now();
    Clock::duration paused{};
    ReadPacer pacer;
    const auto before_read = [&](u64 bytes) {
        for (;;) {
            if (stop.stop_requested()) {
                return false;
            }
            const auto wait = pacer.Admit(Common::ReadAttribution::TotalReads(),
                                          Common::ReadAttribution::TotalBytes(), Clock::now(),
                                          bytes);
            if (wait == std::chrono::nanoseconds::zero()) {
                return true;
            }
            if (pacer.GameIsBusy()) {
                paused += wait;
            }
            Common::StoppableTimedWait(stop, wait);
        }
    };
    const NupContainer::MemberVerifyResult result = container.VerifyMembers(before_read);
    std::vector<BadMember> bad;
    bad.reserve(result.bad.size());
    for (const auto& member : result.bad) {
        bad.push_back({member.path, member.reason});
    }

    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const double paused_seconds = std::chrono::duration<double>(paused).count();
    if (result.cancelled) {
        LOG_INFO(Loader,
                 "NUP integrity check cancelled after {} of {} member(s) ({} corrupt so far); it "
                 "runs again at the next launch",
                 result.checked, result.members, result.bad.size());
        LogBadMembers(file_name, bad);
        return;
    }

    LOG_INFO(Loader,
             "NUP integrity check {}: {}, {} member(s), {} read, {} corrupt, took {:.0f} s ({:.0f} "
             "s paused while the game was reading)",
             result.bad.empty() ? "passed" : "FAILED", file_name, result.members,
             GameFileIdentity::FormatSize(result.bytes_read), result.bad.size(), seconds,
             paused_seconds);
    LogBadMembers(file_name, bad);

    CacheEntry entry;
    entry.stamp = stamp;
    entry.checked_at = static_cast<s64>(std::time(nullptr));
    entry.members = result.members;
    entry.bad = std::move(bad);
    Remember(cache, std::move(entry));
    SaveCache(cache);
}

struct State {
    std::mutex mutex;
    fs::path file;
    std::jthread thread;
};

State& GetState() {
    static State state;
    return state;
}

} // namespace

void Start(const fs::path& file) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (state.thread.joinable() && state.file == file) {
        return;
    }
    state.thread = {};
    state.file = file;
    if (file.empty()) {
        return;
    }
    state.thread = std::jthread{[file](std::stop_token stop) { Run(file, stop); }};
}

void Stop() noexcept {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.thread = {};
    state.file.clear();
}

} // namespace FileSys::NupIntegrity
