// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
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

State& GetState() noexcept {
    static State* state = new State();
    return *state;
}

void CopyDetail(char (&dst)[96], std::string_view src) noexcept {
    const std::size_t len = (std::min)(src.size(), sizeof(dst) - 1);
    if (len > 0) {
        std::memcpy(dst, src.data(), len);
    }
    dst[len] = '\0';
}

void AppendJsonEscaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const unsigned char c : text) {
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
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
}

void WriteRawCrashBytes(
#ifdef _WIN32
    HANDLE handle,
#else
    int fd,
#endif
    const char* data, std::size_t len) noexcept {
    if (len == 0) {
        return;
    }
#ifdef _WIN32
    DWORD written = 0;
    ::WriteFile(handle, data, static_cast<DWORD>(len), &written, nullptr);
#else
    std::size_t offset = 0;
    while (offset < len) {
        const ssize_t n = ::write(fd, data + offset, len - offset);
        if (n <= 0) {
            break;
        }
        offset += static_cast<std::size_t>(n);
    }
#endif
}

std::size_t AppendEscapedFixed(char* dst, std::size_t cap, std::size_t pos,
                               const char* src) noexcept {
    if (pos + 1 < cap) {
        dst[pos++] = '"';
    }
    for (std::size_t i = 0; src[i] != '\0' && pos + 2 < cap; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        if (c == '"' || c == '\\') {
            dst[pos++] = '\\';
            dst[pos++] = static_cast<char>(c);
        } else if (c < 0x20) {
            dst[pos++] = ' ';
        } else {
            dst[pos++] = static_cast<char>(c);
        }
    }
    if (pos + 1 < cap) {
        dst[pos++] = '"';
    }
    return pos;
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
    CopyDetail(ev.detail, detail);

    state.source_recorded[static_cast<std::size_t>(src_id)].fetch_add(1, std::memory_order_relaxed);
    return seq;
}

SnapshotResult Snapshot(u64 after_seq, std::size_t max_events) noexcept {
    SnapshotResult result;
    State& state = GetState();
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
    result.sources = BuildSourcesSnapshot(state);

    if (max_events == 0 || state.count == 0) {
        return result;
    }

    result.events.reserve((std::min)(state.count, max_events));
    for (std::size_t i = 0; i < state.count; ++i) {
        const Event& ev = state.ring[(state.head + i) % state.capacity];
        if (ev.seq > after_seq) {
            result.events.push_back(ev);
            if (result.events.size() >= max_events) {
                break;
            }
        }
    }
    return result;
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

bool WriteIncidentJsonl(const std::filesystem::path& path, std::string_view reason,
                        u64 marker_id) noexcept {
    try {
        const SnapshotResult snap = Snapshot(0, 4096);
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        std::FILE* fp =
#ifdef _WIN32
            ::_wfopen(path.c_str(), L"wb");
#else
            std::fopen(path.c_str(), "wb");
#endif
        if (!fp) {
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
        std::fwrite(header.data(), 1, header.size(), fp);

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
            std::fwrite(row.data(), 1, row.size(), fp);
        }

        std::fclose(fp);
        return true;
    } catch (...) {
        return false;
    }
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

        res.before_log = res.directory / "system_events_before.jsonl";
        const bool before_ok = WriteIncidentJsonl(res.before_log, label, res.mark_id);

        res.summary_path = res.directory / "mark_summary.json";
        const auto snap = Snapshot(0, 4096);
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

        std::string summary = "{";
        summary += "\"mark_id\":" + std::to_string(res.mark_id);
        summary += ",\"seq\":" + std::to_string(res.seq);
        summary += ",\"present_id\":" + std::to_string(res.present_id);
        summary += ",\"mono_ns\":" + std::to_string(now_ns);
        summary += ",\"label\":";
        AppendJsonEscaped(summary, label);
        summary += ",\"aftermath_delay_ms\":" + std::to_string(aftermath_delay_ms);
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

        std::FILE* fp =
#ifdef _WIN32
            ::_wfopen(res.summary_path.c_str(), L"wb");
#else
            std::fopen(res.summary_path.c_str(), "wb");
#endif
        if (fp) {
            std::fwrite(summary.data(), 1, summary.size(), fp);
            std::fclose(fp);
        }

        if (aftermath_delay_ms > 0) {
            const std::filesystem::path after_path = res.directory / "system_events_after.jsonl";
            const u64 id_copy = res.mark_id;
            std::thread([after_path, id_copy, aftermath_delay_ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds{aftermath_delay_ms});
                static_cast<void>(WriteIncidentJsonl(after_path, "aftermath", id_copy));
            }).detach();
        }

        res.ok = before_ok && fp != nullptr;
        return res;
    } catch (...) {
        res.ok = false;
        return res;
    }
}

void DumpCrashTail(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }
#endif

    State& state = GetState();
    std::unique_lock lock{state.mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        static constexpr char kNotice[] =
            "{\"type\":\"notice\",\"reason\":\"system_log_lock_held_at_crash\"}\n";
#ifdef _WIN32
        WriteRawCrashBytes(handle, kNotice, sizeof(kNotice) - 1);
        ::CloseHandle(handle);
#else
        WriteRawCrashBytes(fd, kNotice, sizeof(kNotice) - 1);
        ::close(fd);
#endif
        return;
    }

    char line[512];
    const int hdr_len = std::snprintf(
        line, sizeof(line),
        "{\"type\":\"crash_tail\",\"capacity\":%zu,\"newest\":%llu,\"evicted\":%llu,\"dropped\":%llu}\n",
        state.capacity, static_cast<unsigned long long>(state.next_seq - 1),
        static_cast<unsigned long long>(state.evicted),
        static_cast<unsigned long long>(state.dropped));
    if (hdr_len > 0) {
        const std::size_t n = (std::min)(static_cast<std::size_t>(hdr_len), sizeof(line) - 1);
#ifdef _WIN32
        WriteRawCrashBytes(handle, line, n);
#else
        WriteRawCrashBytes(fd, line, n);
#endif
    }

    const std::size_t tail_count = (std::min)(state.count, std::size_t{256});
    const std::size_t start_offset = state.count - tail_count;
    for (std::size_t i = start_offset; i < state.count; ++i) {
        const Event& ev = state.ring[(state.head + i) % state.capacity];
        const int prefix_len = std::snprintf(
            line, sizeof(line),
            "{\"type\":\"event\",\"seq\":%llu,\"mono_ns\":%llu,\"present_id\":%llu,"
            "\"subsystem\":\"%s\",\"severity\":\"%s\",\"kind\":\"%s\",\"thread_id\":%u,"
            "\"arg0\":%llu,\"arg1\":%llu,\"arg2\":%llu,\"arg3\":%lld,\"detail\":",
            static_cast<unsigned long long>(ev.seq), static_cast<unsigned long long>(ev.mono_ns),
            static_cast<unsigned long long>(ev.present_id), SubsystemName(ev.subsystem),
            SeverityName(ev.severity), EventKindName(ev.kind), ev.thread_id,
            static_cast<unsigned long long>(ev.arg0), static_cast<unsigned long long>(ev.arg1),
            static_cast<unsigned long long>(ev.arg2), static_cast<long long>(ev.arg3));
        if (prefix_len <= 0) {
            continue;
        }
        std::size_t pos = (std::min)(static_cast<std::size_t>(prefix_len), sizeof(line) - 4);
        pos = AppendEscapedFixed(line, sizeof(line) - 2, pos, ev.detail);
        line[pos++] = '}';
        line[pos++] = '\n';
#ifdef _WIN32
        WriteRawCrashBytes(handle, line, pos);
#else
        WriteRawCrashBytes(fd, line, pos);
#endif
    }

#ifdef _WIN32
    ::CloseHandle(handle);
#else
    ::close(fd);
#endif
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
    }
    return "Unknown";
}

} // namespace Common::SystemLog
