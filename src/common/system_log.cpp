// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

#include "common/system_log.h"

namespace Common::SystemLog {
namespace {

constexpr std::size_t DefaultCapacity = 8192;
constexpr std::size_t DiagnosticCapacity = 32768;

enum class SourceId : std::size_t {
    NativeLog = 0,
    GpuAlways,
    GpuTrace,
    AudioAlways,
    AudioSummary,
    FsAlways,
    SchedSummary,
    IpcSummary,
    InputSummary,
    CrashHandler,
    Count,
};

bool EnvIsOne(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool EnvNonEmpty(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
}

u64 MonoNsNow() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

u32 CurrentThreadIdFast() noexcept {
#ifdef _WIN32
    return static_cast<u32>(::GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<u32>(::gettid());
#else
    return static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

SourceId ClassifySource(Subsystem subsystem, EventKind kind) noexcept {
    switch (kind) {
    case EventKind::LogWarning:
    case EventKind::LogError:
    case EventKind::LogCritical:
    case EventKind::Marker:
        return SourceId::NativeLog;
    case EventKind::GpuSubmit:
    case EventKind::GpuRenderPass:
    case EventKind::GpuPresent:
        return SourceId::GpuTrace;
    case EventKind::GpuFenceWait:
    case EventKind::GpuError:
    case EventKind::GpuDeviceLost:
    case EventKind::GpuSwapchainRecreate:
    case EventKind::FrameHitch:
        return SourceId::GpuAlways;
    case EventKind::AudioSummary:
        return SourceId::AudioSummary;
    case EventKind::AudioUnderrun:
    case EventKind::AudioError:
        return SourceId::AudioAlways;
    case EventKind::FsShortRead:
    case EventKind::FsSlowRead:
    case EventKind::FsError:
        return SourceId::FsAlways;
    case EventKind::SchedSummary:
    case EventKind::KernelStall:
        return SourceId::SchedSummary;
    case EventKind::IpcSummary:
    case EventKind::IpcError:
        return SourceId::IpcSummary;
    case EventKind::InputSummary:
        return SourceId::InputSummary;
    case EventKind::HostCrash:
        return SourceId::CrashHandler;
    case EventKind::SceneChange:
        return SourceId::GpuAlways;
    case EventKind::LowMemory:
    case EventKind::NoProgress:
        return SourceId::NativeLog;
    }
    switch (subsystem) {
    case Subsystem::Gpu:
    case Subsystem::Graphics:
        return SourceId::GpuAlways;
    case Subsystem::Audio:
        return SourceId::AudioAlways;
    case Subsystem::Filesystem:
        return SourceId::FsAlways;
    case Subsystem::Kernel:
        return SourceId::SchedSummary;
    case Subsystem::Service:
        return SourceId::IpcSummary;
    case Subsystem::Input:
        return SourceId::InputSummary;
    case Subsystem::Crash:
        return SourceId::CrashHandler;
    default:
        return SourceId::NativeLog;
    }
}

struct State {
    State() {
        const std::size_t initial_cap =
            EnvIsOne("EDEN_NX2_DIAGNOSTIC") ? DiagnosticCapacity : DefaultCapacity;
        ring.resize(initial_cap);
        capacity = initial_cap;
    }

    std::mutex mutex;
    std::vector<Event> ring;
    std::size_t capacity{DefaultCapacity};
    std::size_t head{0};
    std::size_t count{0};
    u64 next_seq{1};
    u64 evicted{0};
    u64 dropped{0};
    std::atomic<const std::atomic<u64>*> present_counter{nullptr};
    std::array<std::atomic<u64>, static_cast<std::size_t>(SourceId::Count)> source_recorded{};
    std::array<std::atomic<u64>, static_cast<std::size_t>(SourceId::Count)> source_dropped{};
};

/// Set once the state exists. The crash tail reads it instead of GetState(), whose function-local
/// static would make a crash that interrupts the first use wait for that initialization.
std::atomic<State*> published_state{nullptr};

State& GetState() noexcept {
    static State* state = [] {
        auto* created = new State();
        published_state.store(created, std::memory_order_release);
        return created;
    }();
    return *state;
}

/// Length of the well-formed UTF-8 sequence starting at `p` (`avail` bytes remain), or 0.
std::size_t Utf8SequenceLength(const unsigned char* p, std::size_t avail) noexcept {
    const unsigned char c = p[0];
    std::size_t len = 0;
    if (c < 0x80) {
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
    } else {
        return 0;
    }
    if (avail < len) {
        return 0;
    }
    for (std::size_t i = 1; i < len; ++i) {
        if ((p[i] & 0xC0) != 0x80) {
            return 0;
        }
    }
    if ((c == 0xE0 && p[1] < 0xA0) || (c == 0xED && p[1] >= 0xA0) ||
        (c == 0xF0 && p[1] < 0x90) || (c == 0xF4 && p[1] >= 0x90)) {
        return 0; // overlong, surrogate or above U+10FFFF
    }
    return len;
}

constexpr char ReplacementCharacter[] = "\xEF\xBF\xBD";

/// Copies `src` as valid UTF-8 into `dst` (`cap` bytes, NUL-terminated): every invalid byte
/// becomes U+FFFD, and the copy ends before the first character that would not fit whole.
void CopyUtf8(char* dst, std::size_t cap, std::string_view src) noexcept {
    std::size_t out = 0;
    for (std::size_t i = 0; i < src.size();) {
        const char* piece = src.data() + i;
        std::size_t piece_len =
            Utf8SequenceLength(reinterpret_cast<const unsigned char*>(piece), src.size() - i);
        std::size_t consumed = piece_len;
        if (piece_len == 0) {
            piece = ReplacementCharacter;
            piece_len = sizeof(ReplacementCharacter) - 1;
            consumed = 1;
        }
        if (out + piece_len > cap - 1) {
            break;
        }
        std::memcpy(dst + out, piece, piece_len);
        out += piece_len;
        i += consumed;
    }
    dst[out] = '\0';
}

void CopyDetail(char (&dst)[96], std::string_view src) noexcept {
    CopyUtf8(dst, sizeof(dst), src);
}

void AppendJsonEscaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                out += buf;
            } else if (c < 0x80) {
                out.push_back(static_cast<char>(c));
            } else if (const std::size_t len = Utf8SequenceLength(
                           reinterpret_cast<const unsigned char*>(text.data() + i), text.size() - i);
                       len != 0) {
                out.append(text.data() + i, len);
                i += len;
                continue;
            } else {
                out += ReplacementCharacter;
            }
            break;
        }
        ++i;
    }
    out.push_back('"');
}

#ifdef _WIN32
using RawFile = HANDLE;

RawFile OpenRawFile(const wchar_t* path) noexcept {
    return ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool IsOpen(RawFile file) noexcept {
    return file != INVALID_HANDLE_VALUE;
}

void CloseRawFile(RawFile file) noexcept {
    ::CloseHandle(file);
}

void WriteRawBytes(RawFile file, const char* data, std::size_t len) noexcept {
    DWORD written = 0;
    if (len != 0) {
        ::WriteFile(file, data, static_cast<DWORD>(len), &written, nullptr);
    }
}
#else
using RawFile = int;

RawFile OpenRawFile(const char* path) noexcept {
    return ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

bool IsOpen(RawFile file) noexcept {
    return file >= 0;
}

void CloseRawFile(RawFile file) noexcept {
    ::close(file);
}

void WriteRawBytes(RawFile file, const char* data, std::size_t len) noexcept {
    std::size_t offset = 0;
    while (offset < len) {
        const ssize_t n = ::write(file, data + offset, len - offset);
        if (n <= 0) {
            break;
        }
        offset += static_cast<std::size_t>(n);
    }
}
#endif

/// Text appended to a fixed buffer: no allocation and no C runtime formatting, for the crash tail.
class RawLine {
public:
    RawLine(char* data_, std::size_t capacity_) noexcept : data{data_}, capacity{capacity_} {}

    void Add(const char* text) noexcept {
        while (*text != '\0' && size + 1 < capacity) {
            data[size++] = *text++;
        }
    }

    void AddUnsigned(u64 value) noexcept {
        char digits[20];
        std::size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0 && size + 1 < capacity) {
            data[size++] = digits[--count];
        }
    }

    void AddSigned(s64 value) noexcept {
        if (value < 0) {
            Add("-");
            AddUnsigned(0 - static_cast<u64>(value));
        } else {
            AddUnsigned(static_cast<u64>(value));
        }
    }

    /// A JSON string; control characters become spaces. Never splits the closing quote off.
    void AddQuoted(const char* text) noexcept {
        Add("\"");
        for (; *text != '\0' && size + 3 < capacity; ++text) {
            const unsigned char c = static_cast<unsigned char>(*text);
            if (c == '"' || c == '\\') {
                data[size++] = '\\';
                data[size++] = static_cast<char>(c);
            } else {
                data[size++] = c < 0x20 ? ' ' : static_cast<char>(c);
            }
        }
        Add("\"");
    }

    char* data;
    std::size_t capacity;
    std::size_t size = 0;
};

/// One event row in the layout of WriteIncidentJsonl's, built with RawLine only.
void FormatEventRow(RawLine& line, const Event& ev) noexcept {
    line.Add("{\"type\":\"event\",\"seq\":");
    line.AddUnsigned(ev.seq);
    line.Add(",\"mono_ns\":");
    line.AddUnsigned(ev.mono_ns);
    line.Add(",\"present_id\":");
    line.AddUnsigned(ev.present_id);
    line.Add(",\"subsystem\":");
    line.AddQuoted(SubsystemName(ev.subsystem));
    line.Add(",\"severity\":");
    line.AddQuoted(SeverityName(ev.severity));
    line.Add(",\"kind\":");
    line.AddQuoted(EventKindName(ev.kind));
    line.Add(",\"thread_id\":");
    line.AddUnsigned(ev.thread_id);
    line.Add(",\"arg0\":");
    line.AddUnsigned(ev.arg0);
    line.Add(",\"arg1\":");
    line.AddUnsigned(ev.arg1);
    line.Add(",\"arg2\":");
    line.AddUnsigned(ev.arg2);
    line.Add(",\"arg3\":");
    line.AddSigned(ev.arg3);
    line.Add(",\"detail\":");
    line.AddQuoted(ev.detail);
    line.Add("}\n");
}

std::vector<SourceInfo> BuildSourcesSnapshot(const State& state) noexcept {
    const bool diag = DiagnosticEnabled();
    const bool gpu_trace = GpuTraceEnabled();
    const bool audio_sum = AudioSummaryEnabled();
    const bool sched = diag || EnvIsOne("EDEN_NX2_SCHED_SUMMARY");
    const bool ipc = diag || EnvNonEmpty("EDEN_NX2_IPC_SUMMARY");
    const bool input = diag || EnvIsOne("EDEN_NX2_HID_SUMMARY");

    auto make = [&](SourceId id, const char* name, bool enabled, const char* reason) -> SourceInfo {
        const auto idx = static_cast<std::size_t>(id);
        return SourceInfo{
            .name = name,
            .enabled = enabled,
            .recorded = state.source_recorded[idx].load(std::memory_order_relaxed),
            .dropped = state.source_dropped[idx].load(std::memory_order_relaxed),
            .missing_reason = enabled ? "" : reason,
        };
    };

    return {
        make(SourceId::NativeLog, "native_log_warnings_errors", true, ""),
        make(SourceId::GpuAlways, "gpu_errors_and_long_waits", true, ""),
        make(SourceId::GpuTrace, "gpu_trace", gpu_trace, "diagnostic_off"),
        make(SourceId::AudioAlways, "audio_underruns_and_errors", true, ""),
        make(SourceId::AudioSummary, "audio_summary", audio_sum, "diagnostic_off"),
        make(SourceId::FsAlways, "fs_short_and_slow_reads", true, ""),
        make(SourceId::SchedSummary, "kernel_sched_summary", sched, "diagnostic_off"),
        make(SourceId::IpcSummary, "service_ipc_summary", ipc, "diagnostic_off"),
        make(SourceId::InputSummary, "input_hid_summary", input, "diagnostic_off"),
        make(SourceId::CrashHandler, "crash_handler", true, ""),
    };
}

} // Anonymous namespace

bool DiagnosticEnabled() noexcept {
    static const bool enabled = EnvIsOne("EDEN_NX2_DIAGNOSTIC");
    return enabled;
}

bool GpuTraceEnabled() noexcept {
    static const bool enabled = DiagnosticEnabled() || EnvIsOne("EDEN_NX2_GPU_TRACE");
    return enabled;
}

bool AudioSummaryEnabled() noexcept {
    static const bool enabled = DiagnosticEnabled() || EnvIsOne("EDEN_NX2_AUDIO_SUMMARY");
    return enabled;
}

bool FsSummaryEnabled() noexcept {
    static const bool enabled = DiagnosticEnabled() || EnvIsOne("EDEN_NX2_FS_SUMMARY");
    return enabled;
}

void SetPresentCounter(const std::atomic<u64>* counter) noexcept {
    GetState().present_counter.store(counter, std::memory_order_release);
}

u64 Record(Subsystem subsystem, Severity severity, EventKind kind, u64 arg0, u64 arg1, u64 arg2,
           s64 arg3, std::string_view detail) noexcept {
    State& state = GetState();
    const auto* counter = state.present_counter.load(std::memory_order_acquire);
    const u64 present_id = counter ? counter->load(std::memory_order_relaxed) : 0;
    const u64 now_ns = MonoNsNow();
    const u32 tid = CurrentThreadIdFast();
    const SourceId src_id = ClassifySource(subsystem, kind);
    // Validated and cut at a character boundary before the lock is taken.
    char detail_copy[96]{};
    CopyDetail(detail_copy, detail);

    std::scoped_lock lock{state.mutex};
    if (state.capacity == 0 || state.ring.empty()) {
        ++state.dropped;
        state.source_dropped[static_cast<std::size_t>(src_id)].fetch_add(1,
                                                                         std::memory_order_relaxed);
        return 0;
    }

    const u64 seq = state.next_seq++;
    std::size_t slot = 0;
    if (state.count < state.capacity) {
        slot = (state.head + state.count) % state.capacity;
        ++state.count;
    } else {
        slot = state.head;
        state.head = (state.head + 1) % state.capacity;
        ++state.evicted;
    }

    Event& ev = state.ring[slot];
    ev.seq = seq;
    ev.mono_ns = now_ns;
    ev.present_id = present_id;
    ev.subsystem = subsystem;
    ev.severity = severity;
    ev.kind = kind;
    ev.thread_id = tid;
    ev.arg0 = arg0;
    ev.arg1 = arg1;
    ev.arg2 = arg2;
    ev.arg3 = arg3;
    std::memcpy(ev.detail, detail_copy, sizeof(detail_copy));

    state.source_recorded[static_cast<std::size_t>(src_id)].fetch_add(1, std::memory_order_relaxed);
    return seq;
}

namespace {

/// Rows are consecutive in the ring: the row at offset i from the head has seq oldest + i.
/// `after_seq < seq <= through_seq`; when more rows match than `max_events`, the newest ones are
/// kept if `newest_window`, the oldest otherwise. The sources are built after the lock is released.
SnapshotResult SnapshotRange(u64 after_seq, u64 through_seq, std::size_t max_events,
                             bool newest_window) noexcept {
    SnapshotResult result;
    State& state = GetState();
    {
        std::scoped_lock lock{state.mutex};
        const u64 newest = state.next_seq - 1;
        const u64 oldest = state.count == 0 ? state.next_seq : state.ring[state.head].seq;
        result.status = RingStatus{
            .capacity = state.capacity,
            .newest = newest,
            .oldest = oldest,
            .evicted = state.evicted,
            .dropped = state.dropped,
            .diagnostic_mode = DiagnosticEnabled(),
        };
        if (!newest_window && after_seq < oldest - 1) {
            result.lost = oldest - 1 - after_seq;
        }
        if (max_events != 0 && state.count != 0) {
            const u64 first = after_seq >= newest ? newest + 1 : (std::max)(after_seq + 1, oldest);
            const u64 last = (std::min)(through_seq, newest);
            if (first <= last) {
                const u64 total = last - first + 1;
                const u64 take = (std::min)(total, static_cast<u64>(max_events));
                result.omitted = total - take;
                const u64 start = newest_window ? last - take + 1 : first;
                result.events.reserve(static_cast<std::size_t>(take));
                std::size_t slot = (state.head + static_cast<std::size_t>(start - oldest)) %
                                   state.capacity;
                for (u64 i = 0; i < take; ++i) {
                    result.events.push_back(state.ring[slot]);
                    slot = (slot + 1) % state.capacity;
                }
            }
        }
    }
    result.sources = BuildSourcesSnapshot(state);
    return result;
}

} // Anonymous namespace

SnapshotResult Snapshot(u64 after_seq, std::size_t max_events) noexcept {
    return SnapshotRange(after_seq, (std::numeric_limits<u64>::max)(), max_events, false);
}

SnapshotResult SnapshotThrough(u64 through_seq, std::size_t max_events) noexcept {
    return SnapshotRange(0, through_seq, max_events, true);
}

EventPage QueryEvents(const EventQuery& query) noexcept {
    EventPage page;
    page.next = query.after_seq;
    State& state = GetState();
    {
        std::scoped_lock lock{state.mutex};
        const u64 newest = state.next_seq - 1;
        const u64 oldest = state.count == 0 ? state.next_seq : state.ring[state.head].seq;
        page.status = RingStatus{
            .capacity = state.capacity,
            .newest = newest,
            .oldest = oldest,
            .evicted = state.evicted,
            .dropped = state.dropped,
            .diagnostic_mode = DiagnosticEnabled(),
        };
        if (query.after_seq < oldest - 1) {
            page.lost = oldest - 1 - query.after_seq;
        }
        if (state.count != 0 && query.after_seq < newest) {
            const u64 first = (std::max)(query.after_seq + 1, oldest);
            std::size_t slot =
                (state.head + static_cast<std::size_t>(first - oldest)) % state.capacity;
            page.events.reserve((std::min)(query.limit, state.count));
            for (u64 seq = first; seq <= newest; ++seq) {
                const Event& ev = state.ring[slot];
                slot = (slot + 1) % state.capacity;
                if ((query.filter_subsystem && ev.subsystem != query.subsystem) ||
                    ev.severity < query.min_severity) {
                    continue;
                }
                if (page.events.size() >= query.limit) {
                    // The next page starts at this match; the rows skipped on the way are not
                    // looked at again.
                    page.more = true;
                    page.next = seq - 1;
                    break;
                }
                page.events.push_back(ev);
            }
            if (!page.more) {
                page.next = newest;
            }
        }
    }
    page.sources = BuildSourcesSnapshot(state);
    return page;
}

RingStatus GetStatus() noexcept {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    return RingStatus{
        .capacity = state.capacity,
        .newest = state.next_seq - 1,
        .oldest = state.count == 0 ? state.next_seq : state.ring[state.head].seq,
        .evicted = state.evicted,
        .dropped = state.dropped,
        .diagnostic_mode = DiagnosticEnabled(),
    };
}

std::vector<SourceInfo> GetSources() noexcept {
    return BuildSourcesSnapshot(GetState());
}

void ResetForTests(std::size_t capacity) noexcept {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.ring.assign(capacity, Event{});
    state.capacity = capacity;
    state.head = 0;
    state.count = 0;
    state.next_seq = 1;
    state.evicted = 0;
    state.dropped = 0;
    for (auto& c : state.source_recorded) {
        c.store(0, std::memory_order_relaxed);
    }
    for (auto& c : state.source_dropped) {
        c.store(0, std::memory_order_relaxed);
    }
}

namespace {

constexpr std::size_t IncidentMaxEvents = 4096;
/// Marks whose aftermath file is waiting for its delay; a further request is refused, and the
/// mark's summary says so.
constexpr std::size_t MaxPendingAftermath = 8;

/// A file whose every write and the close are checked: a full disk is a failure, not a "saved".
class CheckedFile {
public:
    explicit CheckedFile(const std::filesystem::path& path) noexcept
        : fp{
#ifdef _WIN32
              ::_wfopen(path.c_str(), L"wb")
#else
              std::fopen(path.c_str(), "wb")
#endif
          } {
    }
    ~CheckedFile() {
        if (fp) {
            std::fclose(fp);
        }
    }
    CheckedFile(const CheckedFile&) = delete;
    CheckedFile& operator=(const CheckedFile&) = delete;

    [[nodiscard]] bool IsOpen() const noexcept {
        return fp != nullptr;
    }

    void Write(const std::string& text) noexcept {
        if (fp && ok && std::fwrite(text.data(), 1, text.size(), fp) != text.size()) {
            ok = false;
        }
    }

    /// True when the file is open and every write and the close succeeded.
    [[nodiscard]] bool Close() noexcept {
        if (!fp) {
            return false;
        }
        const bool closed = std::fclose(fp) == 0;
        fp = nullptr;
        return ok && closed;
    }

private:
    std::FILE* fp;
    bool ok = true;
};

/// Writes `snap` as JSONL: a header (ring, sources, and the `window` the rows were selected by)
/// and one row per event. `mode` names the selection: "newest_through_seq" (the newest
/// `max_events` rows up to and including `anchor_seq`) or "after_seq" (the oldest `max_events` rows
/// after `anchor_seq`).
bool WriteIncidentFile(const std::filesystem::path& path, std::string_view reason, u64 marker_id,
                       const SnapshotResult& snap, std::string_view mode, u64 anchor_seq,
                       std::size_t max_events) noexcept {
    try {
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        CheckedFile file{path};
        if (!file.IsOpen()) {
            return false;
        }

        std::string header = "{\"type\":\"header\",\"reason\":";
        AppendJsonEscaped(header, reason);
        header += ",\"marker_id\":" + std::to_string(marker_id);
        header += ",\"ring\":{\"capacity\":" + std::to_string(snap.status.capacity) +
                  ",\"newest\":" + std::to_string(snap.status.newest) +
                  ",\"oldest\":" + std::to_string(snap.status.oldest) +
                  ",\"evicted\":" + std::to_string(snap.status.evicted) +
                  ",\"dropped\":" + std::to_string(snap.status.dropped) +
                  ",\"diagnostic_mode\":" + (snap.status.diagnostic_mode ? "true" : "false") + "}";
        header += ",\"window\":{\"mode\":";
        AppendJsonEscaped(header, mode);
        header += ",\"anchor_seq\":" + std::to_string(anchor_seq) +
                  ",\"max_events\":" + std::to_string(max_events) +
                  ",\"included\":" + std::to_string(snap.events.size()) +
                  ",\"omitted\":" + std::to_string(snap.omitted) +
                  ",\"lost\":" + std::to_string(snap.lost) + "}";

        header += ",\"sources\":[";
        std::string missing = "[";
        bool first_missing = true;
        for (std::size_t i = 0; i < snap.sources.size(); ++i) {
            const auto& s = snap.sources[i];
            if (i > 0) {
                header.push_back(',');
            }
            header += "{\"name\":";
            AppendJsonEscaped(header, s.name);
            header += ",\"enabled\":";
            header += (s.enabled ? "true" : "false");
            header += ",\"recorded\":" + std::to_string(s.recorded);
            header += ",\"dropped\":" + std::to_string(s.dropped);
            header += ",\"missing_reason\":";
            AppendJsonEscaped(header, s.missing_reason);
            header.push_back('}');
            if (!s.enabled) {
                if (!first_missing) {
                    missing.push_back(',');
                }
                first_missing = false;
                missing += "{\"name\":";
                AppendJsonEscaped(missing, s.name);
                missing += ",\"reason\":";
                AppendJsonEscaped(missing, s.missing_reason);
                missing.push_back('}');
            }
        }
        missing.push_back(']');
        header += "],\"missing_sources\":" + missing;
        header += ",\"dropped\":" + std::to_string(snap.status.dropped) + "}\n";
        file.Write(header);

        std::string row;
        row.reserve(256);
        for (const Event& ev : snap.events) {
            row.clear();
            row += "{\"type\":\"event\",\"seq\":" + std::to_string(ev.seq) +
                   ",\"mono_ns\":" + std::to_string(ev.mono_ns) +
                   ",\"present_id\":" + std::to_string(ev.present_id) + ",\"subsystem\":";
            AppendJsonEscaped(row, SubsystemName(ev.subsystem));
            row += ",\"severity\":";
            AppendJsonEscaped(row, SeverityName(ev.severity));
            row += ",\"kind\":";
            AppendJsonEscaped(row, EventKindName(ev.kind));
            row += ",\"thread_id\":" + std::to_string(ev.thread_id) +
                   ",\"arg0\":" + std::to_string(ev.arg0) +
                   ",\"arg1\":" + std::to_string(ev.arg1) +
                   ",\"arg2\":" + std::to_string(ev.arg2) +
                   ",\"arg3\":" + std::to_string(ev.arg3) + ",\"detail\":";
            AppendJsonEscaped(row, ev.detail);
            row += "}\n";
            file.Write(row);
        }
        return file.Close();
    } catch (...) {
        return false;
    }
}

struct AftermathJob {
    std::chrono::steady_clock::time_point due;
    std::filesystem::path path;
    u64 mark_id{};
    u64 mark_seq{};
};

/// One worker thread serves every mark's delayed aftermath file, in due order, from a bounded
/// queue. Never destroyed: the thread may still run at process exit.
struct AftermathQueue {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<AftermathJob> jobs;
    bool worker_started = false;
};

AftermathQueue& GetAftermathQueue() noexcept {
    static AftermathQueue* queue = new AftermathQueue();
    return *queue;
}

void AftermathWorker() noexcept {
    AftermathQueue& queue = GetAftermathQueue();
    std::unique_lock lock{queue.mutex};
    for (;;) {
        if (queue.jobs.empty()) {
            queue.wake.wait(lock);
            continue;
        }
        const auto next = std::min_element(
            queue.jobs.begin(), queue.jobs.end(),
            [](const AftermathJob& a, const AftermathJob& b) { return a.due < b.due; });
        if (std::chrono::steady_clock::now() < next->due) {
            queue.wake.wait_until(lock, next->due);
            continue;
        }
        AftermathJob job = std::move(*next);
        queue.jobs.erase(next);
        lock.unlock();
        const SnapshotResult snap = Snapshot(job.mark_seq, IncidentMaxEvents);
        static_cast<void>(WriteIncidentFile(job.path, "aftermath", job.mark_id, snap, "after_seq",
                                            job.mark_seq, IncidentMaxEvents));
        lock.lock();
    }
}

/// False when the queue is full or the worker cannot be started.
bool ScheduleAftermath(AftermathJob job) noexcept {
    try {
        AftermathQueue& queue = GetAftermathQueue();
        std::scoped_lock lock{queue.mutex};
        if (queue.jobs.size() >= MaxPendingAftermath) {
            return false;
        }
        if (!queue.worker_started) {
            std::thread{AftermathWorker}.detach();
            queue.worker_started = true;
        }
        queue.jobs.push_back(std::move(job));
        queue.wake.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}

} // Anonymous namespace

bool WriteIncidentJsonl(const std::filesystem::path& path, std::string_view reason,
                        u64 marker_id) noexcept {
    const SnapshotResult snap = SnapshotThrough((std::numeric_limits<u64>::max)(), IncidentMaxEvents);
    return WriteIncidentFile(path, reason, marker_id, snap, "newest_through_seq",
                             snap.status.newest, IncidentMaxEvents);
}

QuickMarkResult TriggerQuickMark(const std::filesystem::path& incidents_root,
                                 std::string_view label, u32 aftermath_delay_ms) noexcept {
    QuickMarkResult res{};
    try {
        static std::atomic<u64> quick_mark_counter{0};
        res.mark_id = quick_mark_counter.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto* counter = GetState().present_counter.load(std::memory_order_acquire);
        res.present_id = counter ? counter->load(std::memory_order_relaxed) : 0;
        const u64 now_ns = MonoNsNow();
        res.seq = Record(Subsystem::Marker, Severity::Info, EventKind::Marker, res.mark_id,
                         res.present_id, label.size(), 0, label);

        const std::time_t now_t = std::time(nullptr);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &now_t);
#else
        localtime_r(&now_t, &tm_buf);
#endif
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", tm_buf.tm_year + 1900,
                      tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min,
                      tm_buf.tm_sec);

        const std::string folder_name =
            "mark-" + std::string{stamp} + "-" + std::to_string(res.mark_id) + "-p" +
            std::to_string(res.present_id);
        res.directory = incidents_root / folder_name;
        std::error_code ec;
        std::filesystem::create_directories(res.directory, ec);
        if (ec) {
            return res;
        }

        // The window ends at the mark, so it is in its own before file however much the ring has
        // seen since; the older rows beyond the bound are counted, not silently dropped.
        const u64 through = res.seq != 0 ? res.seq : (std::numeric_limits<u64>::max)();
        const SnapshotResult snap = SnapshotThrough(through, IncidentMaxEvents);
        res.before_log = res.directory / "system_events_before.jsonl";
        const bool before_ok = WriteIncidentFile(res.before_log, label, res.mark_id, snap,
                                                 "newest_through_seq", res.seq, IncidentMaxEvents);

        const bool aftermath_requested = aftermath_delay_ms > 0;
        if (aftermath_requested) {
            res.aftermath_scheduled = ScheduleAftermath(AftermathJob{
                .due = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds{aftermath_delay_ms},
                .path = res.directory / "system_events_after.jsonl",
                .mark_id = res.mark_id,
                .mark_seq = res.seq,
            });
        }

        res.summary_path = res.directory / "mark_summary.json";
        const RingStatus& st = snap.status;
        const auto& sources = snap.sources;
        u64 warn_err = 0;
        u64 frame_hitches = 0;
        u64 gpu_errors = 0;
        u64 audio_underruns = 0;
        u64 fs_anomalies = 0;
        std::vector<std::pair<std::string, u64>> hitch_causes;
        for (const auto& ev : snap.events) {
            if (ev.severity >= Severity::Warning) {
                ++warn_err;
            }
            switch (ev.kind) {
            case EventKind::FrameHitch: {
                ++frame_hitches;
                const std::string c = ev.detail[0] != '\0' ? std::string{ev.detail} : "Cpu/Unknown";
                auto it = std::find_if(hitch_causes.begin(), hitch_causes.end(),
                                       [&](const auto& p) { return p.first == c; });
                if (it != hitch_causes.end()) {
                    ++it->second;
                } else {
                    hitch_causes.emplace_back(c, 1);
                }
                break;
            }
            case EventKind::GpuDeviceLost:
            case EventKind::GpuError:
                ++gpu_errors;
                break;
            case EventKind::AudioUnderrun:
            case EventKind::AudioError:
                ++audio_underruns;
                break;
            case EventKind::FsShortRead:
            case EventKind::FsSlowRead:
            case EventKind::FsError:
                ++fs_anomalies;
                break;
            default:
                break;
            }
        }

        std::string missing_json = "[";
        bool first_missing = true;
        for (const auto& s : sources) {
            if (!s.enabled) {
                if (!first_missing) {
                    missing_json.push_back(',');
                }
                first_missing = false;
                missing_json += "{\"name\":";
                AppendJsonEscaped(missing_json, s.name);
                missing_json += ",\"reason\":";
                AppendJsonEscaped(missing_json, s.missing_reason);
                missing_json.push_back('}');
            }
        }
        missing_json.push_back(']');

        std::string suspected_json = "[";
        bool first_cause = true;
        auto add_cause = [&](std::string_view cause, std::string_view basis,
                             std::string_view confidence, u64 count) {
            if (!first_cause) {
                suspected_json.push_back(',');
            }
            first_cause = false;
            suspected_json += "{\"cause\":";
            AppendJsonEscaped(suspected_json, cause);
            suspected_json += ",\"basis\":";
            AppendJsonEscaped(suspected_json, basis);
            suspected_json += ",\"confidence\":";
            AppendJsonEscaped(suspected_json, confidence);
            suspected_json += ",\"evidence_count\":" + std::to_string(count) + "}";
        };
        for (const auto& [cause, cnt] : hitch_causes) {
            add_cause(cause, "dominant_wait_during_frame_hitch", "suspected", cnt);
        }
        if (gpu_errors > 0) {
            add_cause("gpu_device_or_driver_error", "vulkan_error_event", "suspected", gpu_errors);
        }
        if (audio_underruns > 0) {
            add_cause("audio_sink_starvation", "audio_underrun_or_stall", "suspected",
                      audio_underruns);
        }
        if (fs_anomalies > 0) {
            add_cause("storage_io_delay_or_short_read", "filesystem_event", "suspected",
                      fs_anomalies);
        }
        if (first_cause) {
            add_cause("undetermined", "no_subsystem_anomaly_in_ring", "unknown", 0);
        }
        suspected_json.push_back(']');

        const bool contains_mark =
            res.seq != 0 && !snap.events.empty() && snap.events.back().seq == res.seq;
        std::string summary = "{";
        summary += "\"mark_id\":" + std::to_string(res.mark_id);
        summary += ",\"seq\":" + std::to_string(res.seq);
        summary += ",\"present_id\":" + std::to_string(res.present_id);
        summary += ",\"mono_ns\":" + std::to_string(now_ns);
        summary += ",\"label\":";
        AppendJsonEscaped(summary, label);
        summary += ",\"aftermath_delay_ms\":" + std::to_string(aftermath_delay_ms);
        summary += ",\"aftermath\":{\"requested\":";
        summary += aftermath_requested ? "true" : "false";
        summary += ",\"scheduled\":";
        summary += res.aftermath_scheduled ? "true" : "false";
        summary += ",\"after_seq\":" + std::to_string(res.seq) + "}";
        summary += ",\"before_window\":{\"through_seq\":" + std::to_string(res.seq) +
                   ",\"max_events\":" + std::to_string(IncidentMaxEvents) +
                   ",\"included\":" + std::to_string(snap.events.size()) +
                   ",\"omitted_older\":" + std::to_string(snap.omitted) +
                   ",\"contains_mark\":" + (contains_mark ? "true" : "false") + "}";
        summary += ",\"ring\":{\"capacity\":" + std::to_string(st.capacity) +
                   ",\"newest\":" + std::to_string(st.newest) +
                   ",\"oldest\":" + std::to_string(st.oldest) +
                   ",\"evicted\":" + std::to_string(st.evicted) +
                   ",\"dropped\":" + std::to_string(st.dropped) + "}";
        summary += ",\"missing_sources\":" + missing_json;
        summary += ",\"observed\":{\"mark_id\":" + std::to_string(res.mark_id) +
                   ",\"seq\":" + std::to_string(res.seq) +
                   ",\"present_id\":" + std::to_string(res.present_id) +
                   ",\"mono_ns\":" + std::to_string(now_ns) + ",\"label\":";
        AppendJsonEscaped(summary, label);
        summary += ",\"events_in_ring\":" + std::to_string(snap.events.size()) +
                   ",\"warnings_or_errors\":" + std::to_string(warn_err) +
                   ",\"frame_hitches\":" + std::to_string(frame_hitches) +
                   ",\"gpu_errors\":" + std::to_string(gpu_errors) +
                   ",\"audio_underruns\":" + std::to_string(audio_underruns) +
                   ",\"fs_anomalies\":" + std::to_string(fs_anomalies) +
                   ",\"dropped_events\":" + std::to_string(st.dropped) +
                   ",\"evicted_events\":" + std::to_string(st.evicted) +
                   ",\"missing_sources\":" + missing_json + "}";
        summary += ",\"suspected_causes\":" + suspected_json + "}\n";

        bool summary_ok = false;
        {
            CheckedFile file{res.summary_path};
            file.Write(summary);
            summary_ok = file.Close();
        }

        res.ok = before_ok && summary_ok;
        return res;
    } catch (...) {
        res.ok = false;
        return res;
    }
}

void DumpCrashTail(const std::filesystem::path::value_type* path, const CrashInfo* crash) noexcept {
    if (path == nullptr || path[0] == 0) {
        return;
    }
    const RawFile file = OpenRawFile(path);
    if (!IsOpen(file)) {
        return;
    }

    char buffer[1024];
    State* const state = published_state.load(std::memory_order_acquire);
    bool wrote_ring = false;
    if (state != nullptr) {
        std::unique_lock lock{state->mutex, std::try_to_lock};
        if (lock.owns_lock()) {
            RawLine header{buffer, sizeof(buffer)};
            header.Add("{\"type\":\"crash_tail\",\"capacity\":");
            header.AddUnsigned(state->capacity);
            header.Add(",\"newest\":");
            header.AddUnsigned(state->next_seq - 1);
            header.Add(",\"evicted\":");
            header.AddUnsigned(state->evicted);
            header.Add(",\"dropped\":");
            header.AddUnsigned(state->dropped);
            header.Add("}\n");
            WriteRawBytes(file, buffer, header.size);

            const std::size_t tail_count = (std::min)(state->count, std::size_t{256});
            for (std::size_t i = state->count - tail_count; i < state->count; ++i) {
                RawLine line{buffer, sizeof(buffer)};
                FormatEventRow(line, state->ring[(state->head + i) % state->capacity]);
                WriteRawBytes(file, buffer, line.size);
            }
            wrote_ring = true;
        }
    }
    if (!wrote_ring) {
        RawLine notice{buffer, sizeof(buffer)};
        notice.Add(state == nullptr
                       ? "{\"type\":\"notice\",\"reason\":\"system_log_not_initialized\"}\n"
                       : "{\"type\":\"notice\",\"reason\":\"system_log_lock_held_at_crash\"}\n");
        WriteRawBytes(file, buffer, notice.size);
    }

    if (crash != nullptr) {
        Event ev{};
        ev.mono_ns = MonoNsNow();
        if (state != nullptr) {
            const auto* counter = state->present_counter.load(std::memory_order_acquire);
            ev.present_id = counter ? counter->load(std::memory_order_relaxed) : 0;
        }
        ev.subsystem = Subsystem::Crash;
        ev.severity = Severity::Critical;
        ev.kind = EventKind::HostCrash;
        ev.thread_id = crash->thread_id;
        ev.arg0 = crash->code;
        ev.arg1 = crash->address;
        ev.arg2 = crash->thread_id;
        if (crash->module != nullptr) {
            CopyDetail(ev.detail, crash->module);
        }
        RawLine line{buffer, sizeof(buffer)};
        FormatEventRow(line, ev);
        WriteRawBytes(file, buffer, line.size);
    }
    CloseRawFile(file);
}

const char* SubsystemName(Subsystem subsystem) noexcept {
    switch (subsystem) {
    case Subsystem::Graphics:
        return "Graphics";
    case Subsystem::Gpu:
        return "Gpu";
    case Subsystem::Audio:
        return "Audio";
    case Subsystem::Kernel:
        return "Kernel";
    case Subsystem::Service:
        return "Service";
    case Subsystem::Filesystem:
        return "Filesystem";
    case Subsystem::Input:
        return "Input";
    case Subsystem::Core:
        return "Core";
    case Subsystem::Memory:
        return "Memory";
    case Subsystem::Crash:
        return "Crash";
    case Subsystem::Marker:
        return "Marker";
    case Subsystem::Log:
        return "Log";
    }
    return "Unknown";
}

const char* SeverityName(Severity severity) noexcept {
    switch (severity) {
    case Severity::Trace:
        return "Trace";
    case Severity::Info:
        return "Info";
    case Severity::Warning:
        return "Warning";
    case Severity::Error:
        return "Error";
    case Severity::Critical:
        return "Critical";
    }
    return "Unknown";
}

const char* EventKindName(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::GpuSubmit:
        return "GpuSubmit";
    case EventKind::GpuRenderPass:
        return "GpuRenderPass";
    case EventKind::GpuFenceWait:
        return "GpuFenceWait";
    case EventKind::GpuPresent:
        return "GpuPresent";
    case EventKind::GpuError:
        return "GpuError";
    case EventKind::GpuDeviceLost:
        return "GpuDeviceLost";
    case EventKind::GpuSwapchainRecreate:
        return "GpuSwapchainRecreate";
    case EventKind::FrameHitch:
        return "FrameHitch";
    case EventKind::AudioUnderrun:
        return "AudioUnderrun";
    case EventKind::AudioError:
        return "AudioError";
    case EventKind::AudioSummary:
        return "AudioSummary";
    case EventKind::FsShortRead:
        return "FsShortRead";
    case EventKind::FsSlowRead:
        return "FsSlowRead";
    case EventKind::FsError:
        return "FsError";
    case EventKind::SchedSummary:
        return "SchedSummary";
    case EventKind::KernelStall:
        return "KernelStall";
    case EventKind::IpcSummary:
        return "IpcSummary";
    case EventKind::IpcError:
        return "IpcError";
    case EventKind::InputSummary:
        return "InputSummary";
    case EventKind::LogWarning:
        return "LogWarning";
    case EventKind::LogError:
        return "LogError";
    case EventKind::LogCritical:
        return "LogCritical";
    case EventKind::Marker:
        return "Marker";
    case EventKind::HostCrash:
        return "HostCrash";
    case EventKind::SceneChange:
        return "SceneChange";
    case EventKind::LowMemory:
        return "LowMemory";
    case EventKind::NoProgress:
        return "NoProgress";
    }
    return "Unknown";
}

} // namespace Common::SystemLog
