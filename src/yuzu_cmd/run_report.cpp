// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
extern char** environ;
#endif

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "common/nx2_bottleneck.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/settings_common.h"
#include "yuzu_cmd/run_report.h"

// After the others: windows.h defines min and max.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace RunReport {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

/// Held for the life of the process: the stuck-shutdown watchdog can publish while the main
/// thread is already unwinding static state.
struct State {
    std::mutex mutex;
    bool prepared = false;
    bool published = false;
    bool published_ok = true;
    fs::path destination;
    fs::path temporary;
    fs::path crash_temporary;
    bool temporary_created = false; // Prepare created `temporary` (it did not exist before)
    Json tunables = Json::object();
    Json initial_settings = Json::object();
    std::optional<u64> title_id;
};

State& GetState() {
    static auto* state = new State;
    return *state;
}

int ProcessId() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

/// The settings a run's behaviour and speed depend on; paths, UI, controls and the like are left
/// out of the report on purpose.
bool IsReported(Settings::Category category) {
    using Settings::Category;
    switch (category) {
    case Category::Audio:
    case Category::Core:
    case Category::Cpu:
    case Category::CpuDebug:
    case Category::CpuUnsafe:
    case Category::Renderer:
    case Category::RendererAdvanced:
    case Category::RendererHacks:
    case Category::RendererExtensions:
    case Category::RendererDebug:
    case Category::RendererMemory:
    case Category::System:
    case Category::SystemAudio:
    case Category::GpuDriver:
        return true;
    default:
        return false;
    }
}

/// The canonical text of a setting as a JSON scalar of its own type where the text is exactly a
/// number or a boolean; anything else (enum names, strings, hex) stays a string.
Json ScalarOf(const Settings::BasicSetting& setting) {
    const std::string text = setting.Canonicalize();
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    if (setting.IsEnum() || text.empty()) {
        return text;
    }
    const char* const begin = text.c_str();
    char* end = nullptr;
    if (setting.IsIntegral()) {
        errno = 0;
        if (text[0] == '-') {
            const long long value = std::strtoll(begin, &end, 10);
            if (errno == 0 && *end == '\0') {
                return value;
            }
        } else if (text[0] >= '0' && text[0] <= '9') {
            const unsigned long long value = std::strtoull(begin, &end, 10);
            if (errno == 0 && *end == '\0') {
                return value;
            }
        }
    } else if (setting.IsFloatingPoint()) {
        errno = 0;
        const double value = std::strtod(begin, &end);
        if (errno == 0 && end != begin && *end == '\0' && std::isfinite(value)) {
            return value;
        }
    }
    return text;
}

Json ReportedSettings() {
    Json result = Json::object();
    for (const auto& [key, setting] : Settings::values.linkage.by_key) {
        if (IsReported(setting->GetCategory())) {
            result[key] = ScalarOf(*setting);
        }
    }
    return result;
}

/// The environment variables that tune the emulator: EDEN_*, NX2* and EGLANTINE_*. There is no
/// table of defaults, so only the variables that are set appear.
Json SetTunables() {
    Json result = Json::object();
#ifdef _WIN32
    char** const variables = _environ;
#else
    char** const variables = environ;
#endif
    if (variables == nullptr) {
        return result;
    }
    for (char** entry = variables; *entry != nullptr; ++entry) {
        const std::string_view text{*entry};
        const size_t equals = text.find('=');
        // A leading '=' is how Windows keeps per-drive directories in the environment block.
        if (equals == std::string_view::npos || equals == 0) {
            continue;
        }
        const std::string_view name = text.substr(0, equals);
        if (!name.starts_with("EDEN_") && !name.starts_with("NX2") &&
            !name.starts_with("EGLANTINE_")) {
            continue;
        }
        Json tunable = Json::object();
        tunable["raw"] = std::string{text.substr(equals + 1)};
        tunable["source"] = "environment";
        tunable["effective"] = Json::object();
        result[std::string{name}] = std::move(tunable);
    }
    return result;
}

Json Number(const std::optional<double>& value) {
    return value ? Json(*value) : Json(nullptr);
}

Json WaitJson(const Nx2Bottleneck::WaitSummary& wait) {
    Json json = Json::object();
    json["total_ms"] = wait.total_ms;
    json["longest_ms"] = wait.longest_ms;
    json["count"] = wait.count;
    return json;
}

Json SplitJson(const Nx2Bottleneck::ThreadSplit& split) {
    if (!split.seen) {
        return nullptr;
    }
    static constexpr std::array<std::string_view, 5> names{"host_gpu", "shader_build",
                                                           "vulkan_worker", "present", "gpu_back"};
    static_assert(std::tuple_size_v<decltype(split.waits_ms)> == names.size());
    Json waits = Json::object();
    for (size_t i = 0; i < names.size(); ++i) {
        waits[std::string{names[i]}] = split.waits_ms[i];
    }
    Json json = Json::object();
    json["observed_seconds"] = split.observed_seconds;
    json["work_ms"] = split.work_ms;
    json["idle_ms"] = split.idle_ms;
    json["waits_ms"] = std::move(waits);
    return json;
}

Json MetricsJson(const Nx2Bottleneck::RunMetrics& metrics) {
    Json frame_time = Json::object();
    frame_time["samples"] = metrics.samples;
    frame_time["mean"] = Number(metrics.mean_ms);
    frame_time["p50"] = Number(metrics.p50_ms);
    frame_time["p95"] = Number(metrics.p95_ms);
    frame_time["p99"] = Number(metrics.p99_ms);
    frame_time["max"] = Number(metrics.max_ms);
    frame_time["method"] = "nearest_rank_log_histogram_us";
    frame_time["relative_bucket_width"] = 0.015625;
    frame_time["over_25_ms"] = metrics.over_25ms;
    frame_time["over_50_ms"] = metrics.over_50ms;
    frame_time["over_100_ms"] = metrics.over_100ms;

    Json threads = Json::object();
    threads["gpu"] = SplitJson(metrics.threads[0]);
    threads["gpu_back"] = SplitJson(metrics.threads[1]);
    threads["vulkan_worker"] = SplitJson(metrics.threads[2]);

    Json guest_wait = WaitJson(metrics.guest_gpu_wait);
    guest_wait["fraction"] = Number(metrics.guest_gpu_fraction);

    // A run that never started the guest has no cores to report.
    Json cores = Json::array();
    if (metrics.active_seconds > 0.0) {
        for (const double busy_ms : metrics.guest_cpu_core_busy_ms) {
            cores.push_back(busy_ms);
        }
    }

    Json json = Json::object();
    json["active_seconds"] = metrics.active_seconds;
    json["frames"] = metrics.frames;
    json["fps"] = Number(metrics.fps);
    json["frame_time_ms"] = std::move(frame_time);
    json["threads"] = std::move(threads);
    json["guest_gpu_wait"] = std::move(guest_wait);
    json["guest_cpu_core_busy_ms"] = std::move(cores);
    json["shader_wait"] = WaitJson(metrics.shader_wait);
    json["limiter"] = metrics.limiter;
    return json;
}

Json BuildJson() {
    Json json = Json::object();
    json["revision"] = std::string(Common::g_scm_rev);
    json["branch"] = std::string(Common::g_scm_branch);
    json["description"] = std::string(Common::g_scm_desc);
    json["build_id"] = std::string(Common::g_build_id);
    json["build_date"] = std::string(Common::g_build_date);
    json["compiler"] = std::string(Common::g_compiler_id);
    json["name"] = std::string(Common::g_build_name);
    json["full_name"] = std::string(Common::g_build_fullname);
    json["version"] = std::string(Common::g_build_version);
    json["display_version"] = std::string(Common::g_display_version);
    json["flavour"] = std::string(Common::g_build_flavour);
    return json;
}

Json LimitsJson() {
    const Common::RunOutcome::Limits limits = Common::RunOutcome::EffectiveLimits();
    Json json = Json::object();
    json["seconds"] = limits.seconds ? Json(*limits.seconds) : Json(nullptr);
    json["frames"] = limits.frames ? Json(*limits.frames) : Json(nullptr);
    return json;
}

/// Everything in the file, taken in one go (the recording is consumed).
std::string Document(State& state, Common::RunOutcome::Reason reason, int code,
                     bool shutdown_complete, std::string_view detail) {
    const Nx2Bottleneck::RunRecording recording = Nx2Bottleneck::EndRecording(!shutdown_complete);
    const Common::Log::LogCounts counts = Common::Log::GetLogCounts();

    Json windows = Json::array();
    for (const Nx2Bottleneck::RunWindow& window : recording.windows) {
        Json entry = Json::object();
        entry["start_seconds"] = window.start_seconds;
        entry["end_seconds"] = window.end_seconds;
        entry["partial"] = window.partial;
        entry["metrics"] = MetricsJson(window.metrics);
        windows.push_back(std::move(entry));
    }

    Json logs = Json::object();
    logs["scope"] = "filtered_emitted";
    logs["error"] = counts.error;
    logs["critical"] = counts.critical;

    Json exit_json = Json::object();
    exit_json["reason"] = std::string{Common::RunOutcome::ReasonName(reason)};
    exit_json["code"] = code;
    exit_json["shutdown_complete"] = shutdown_complete;
    exit_json["metrics_complete"] = recording.complete;
    exit_json["detail"] = std::string{detail};

    Json settings = Json::object();
    settings["initial"] = state.initial_settings;
    settings["final"] = ReportedSettings();

    Json document = Json::object();
    document["schema_version"] = kSchemaVersion;
    document["build"] = BuildJson();
    document["title_id"] = state.title_id ? Json(fmt::format("{:016X}", *state.title_id))
                                          : Json(nullptr);
    document["limits"] = LimitsJson();
    document["elapsed_seconds"] =
        Common::RunOutcome::Active() ? Common::RunOutcome::ElapsedSeconds() : 0.0;
    document["summary"] = MetricsJson(recording.summary);
    document["windows"] = std::move(windows);
    document["logs"] = std::move(logs);
    document["exit"] = std::move(exit_json);
    document["tunables"] = state.tunables;
    document["settings"] = std::move(settings);
    // Invalid UTF-8 (a detail quoting a game string) must not cost the whole report.
    return document.dump(2, ' ', false, Json::error_handler_t::replace) + '\n';
}

bool Fail(const std::string& message) {
    LOG_ERROR(Frontend, "Cannot write the report: {}", message);
    std::fprintf(stderr, "Cannot write the report: %s\n", message.c_str());
    return false;
}

/// Moves `from` to `to`, never over an existing file (the report is never overwritten).
bool RenameNoReplace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), 0) != 0;
#else
    if (link(from.c_str(), to.c_str()) != 0) {
        return false;
    }
    unlink(from.c_str());
    return true;
#endif
}

bool WriteReportFile(State& state, const std::string& text) {
    std::error_code ec;
    {
        std::ofstream out{state.temporary, std::ios::binary | std::ios::out | std::ios::trunc};
        if (!out) {
            out.close();
            fs::remove(state.temporary, ec);
            return Fail("cannot open " + Common::FS::ToUTF8String(state.temporary.u8string()));
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(state.temporary, ec);
            return Fail("writing " + Common::FS::ToUTF8String(state.temporary.u8string()) +
                        " failed");
        }
    }
    // Prepare refused an existing destination; refuse one that appeared since too.
    if (fs::exists(fs::symlink_status(state.destination, ec))) {
        fs::remove(state.temporary, ec);
        return Fail(Common::FS::ToUTF8String(state.destination.u8string()) +
                    " appeared while the run was going");
    }
    if (!RenameNoReplace(state.temporary, state.destination)) {
        fs::remove(state.temporary, ec);
        return Fail("renaming to " + Common::FS::ToUTF8String(state.destination.u8string()) +
                    " failed (the destination exists, or cannot be written)");
    }
    return true;
}

// The report of a crash of the emulator itself (a crash handler calls WriteCrashReport through
// Common::RunOutcome::ReportHostCrash). The process is damaged then, so nothing here takes a lock,
// allocates, or waits: the file is opened before the run, and the text is formatted into static
// buffers before the run too, except for a handful of numbers and the detail line. There are two
// static documents so that one can be rebuilt (title, settings) while the other can be read.

enum CrashPhase : int {
    CrashUnarmed,
    CrashArmed,
    CrashWriting,
    CrashPublishing,
};

constexpr size_t CrashHeadCapacity = 8 * 1024;
constexpr size_t CrashTailCapacity = 96 * 1024;
constexpr size_t CrashMiddleCapacity = 8 * 1024;
constexpr size_t CrashPathCapacity = 1024;

struct CrashDocument {
    char head[CrashHeadCapacity];
    char tail[CrashTailCapacity];
    size_t head_size;
    size_t tail_size;
};

CrashDocument crash_documents[2];
std::atomic<int> crash_document{-1};
std::atomic<int> crash_phase{CrashUnarmed};
#ifdef _WIN32
HANDLE crash_file = INVALID_HANDLE_VALUE;
wchar_t crash_temporary[CrashPathCapacity];
wchar_t crash_destination[CrashPathCapacity];
wchar_t crash_normal_temporary[CrashPathCapacity];
using CrashChar = wchar_t;
#else
int crash_file = -1;
char crash_temporary[CrashPathCapacity];
char crash_destination[CrashPathCapacity];
char crash_normal_temporary[CrashPathCapacity];
using CrashChar = char;
#endif

/// Text appended to a fixed buffer, cut off where it ends.
class CrashText {
public:
    CrashText(char* data_, size_t capacity_) noexcept : data{data_}, capacity{capacity_} {}

    void Add(std::string_view text) noexcept {
        const size_t count = text.size() < capacity - size ? text.size() : capacity - size;
        std::memcpy(data + size, text.data(), count);
        size += count;
    }

    void AddNumber(unsigned long long value) noexcept {
        char digits[24];
        size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0) {
            Add(std::string_view{&digits[--count], 1});
        }
    }

    /// As the inside of a JSON string, ASCII only; leaves `reserve` bytes free after it.
    void AddEscaped(std::string_view text, size_t reserve) noexcept {
        static constexpr char hex[] = "0123456789abcdef";
        const size_t limit = capacity > reserve ? capacity - reserve : 0;
        for (const char c : text) {
            const auto byte = static_cast<unsigned char>(c);
            if (c == '"' || c == '\\') {
                if (size + 2 > limit) {
                    return;
                }
                data[size++] = '\\';
                data[size++] = c;
            } else if (byte < 0x20) {
                if (size + 6 > limit) {
                    return;
                }
                const char escape[6] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]};
                std::memcpy(data + size, escape, sizeof(escape));
                size += sizeof(escape);
            } else {
                if (size + 1 > limit) {
                    return;
                }
                data[size++] = byte < 0x7F ? c : '?';
            }
        }
    }

    char* data;
    size_t capacity;
    size_t size = 0;
};

bool CloseCrashFile() noexcept {
    bool ok = true;
#ifdef _WIN32
    if (crash_file != INVALID_HANDLE_VALUE) {
        ok = CloseHandle(crash_file) != 0;
        crash_file = INVALID_HANDLE_VALUE;
    }
#else
    if (crash_file >= 0) {
        ok = close(crash_file) == 0;
        crash_file = -1;
    }
#endif
    return ok;
}

bool WriteCrashPart(const char* data, size_t size) noexcept {
#ifdef _WIN32
    while (size != 0) {
        DWORD written = 0;
        if (!WriteFile(crash_file, data, static_cast<DWORD>(size), &written, nullptr) ||
            written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
#else
    while (size != 0) {
        const ssize_t written = write(crash_file, data, size);
        if (written <= 0) {
            return false;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
#endif
    return true;
}

bool FlushCrashFile() noexcept {
#ifdef _WIN32
    return FlushFileBuffers(crash_file) != 0;
#else
    return fsync(crash_file) == 0;
#endif
}

/// Moves the crash report to its destination, never over an existing file.
bool PublishCrashFile() noexcept {
#ifdef _WIN32
    return MoveFileExW(crash_temporary, crash_destination, 0) != 0;
#else
    if (link(crash_temporary, crash_destination) != 0) {
        return false;
    }
    unlink(crash_temporary);
    return true;
#endif
}

bool CrashDestinationExists() noexcept {
#ifdef _WIN32
    return GetFileAttributesW(crash_destination) != INVALID_FILE_ATTRIBUTES;
#else
    return access(crash_destination, F_OK) == 0;
#endif
}

/// Deletes a file this process created. Takes no lock and allocates nothing; an empty path (the
/// file is not ours to delete, or not known) does nothing.
void RemoveCrashFile(const CrashChar* path) noexcept {
    if (path[0] == 0) {
        return;
    }
#ifdef _WIN32
    DeleteFileW(path);
#else
    unlink(path);
#endif
}

/// Writes the crash report. False when a report was meant and could not be written; true when it
/// was written, was not armed, or the normal report is already at the destination.
bool WriteCrashReport(std::string_view detail) noexcept {
    const int index = crash_document.load(std::memory_order_acquire);
    if (index < 0) {
        return true;
    }
    int expected = CrashArmed;
    if (!crash_phase.compare_exchange_strong(expected, CrashWriting, std::memory_order_acq_rel)) {
        return true;
    }
    const CrashDocument& document = crash_documents[index];

    char middle[CrashMiddleCapacity];
    CrashText text{middle, sizeof(middle)};
    const double elapsed =
        Common::RunOutcome::Active() ? Common::RunOutcome::ElapsedSeconds() : 0.0;
    const auto milliseconds = static_cast<unsigned long long>(elapsed > 0.0 ? elapsed * 1000.0 : 0.0);
    text.AddNumber(milliseconds / 1000);
    text.Add(".");
    text.AddNumber(milliseconds % 1000 / 100);
    text.AddNumber(milliseconds % 100 / 10);
    text.AddNumber(milliseconds % 10);
    text.Add(",\"summary\":{\"frames\":");
    text.AddNumber(Common::RunOutcome::FramesPresented());
    const Common::Log::LogCounts counts = Common::Log::GetLogCounts();
    text.Add("},\"windows\":[],\"logs\":{\"scope\":\"filtered_emitted\",\"error\":");
    text.AddNumber(counts.error);
    text.Add(",\"critical\":");
    text.AddNumber(counts.critical);
    text.Add("},\"exit\":{\"reason\":\"host_crash\",\"code\":");
    text.AddNumber(static_cast<unsigned long long>(
        Common::RunOutcome::ExitCode(Common::RunOutcome::Reason::HostCrash)));
    text.Add(",\"shutdown_complete\":false,\"metrics_complete\":false,\"detail\":\"");
    text.AddEscaped(detail, 64);
    text.Add("\"}");

    // Seeking is not needed: the file was empty when it was created.
    const bool written = WriteCrashPart(document.head, document.head_size) &&
                         WriteCrashPart(middle, text.size) &&
                         WriteCrashPart(document.tail, document.tail_size) && FlushCrashFile();
    const bool closed = CloseCrashFile();
    const bool published = written && closed && PublishCrashFile();
    // The report is out (or has failed): the files this run created for it must not stay. A crash
    // temporary that was not moved to its destination, and the normal report's temporary that
    // Prepare created and the normal write never got to use.
    if (!published) {
        RemoveCrashFile(crash_temporary);
    }
    RemoveCrashFile(crash_normal_temporary);
    if (published) {
        return true;
    }
    // The normal report can have been put in place in the meantime, which is as good.
    return CrashDestinationExists();
}

/// Formats the fixed parts of the crash report from what is known now. Called with the state
/// mutex held, before the run and again when the title and the settings are known.
void RebuildCrashDocument(State& state) {
    if (crash_phase.load(std::memory_order_acquire) != CrashArmed) {
        return;
    }
    constexpr auto replace = Json::error_handler_t::replace;
    Json fixed = Json::object();
    fixed["schema_version"] = kSchemaVersion;
    fixed["build"] = BuildJson();
    fixed["title_id"] = state.title_id ? Json(fmt::format("{:016X}", *state.title_id))
                                      : Json(nullptr);
    fixed["limits"] = LimitsJson();
    // Not closed: the dynamic part (elapsed seconds on) is written after it.
    std::string head = fixed.dump(-1, ' ', false, replace);
    head.pop_back();
    head += ",\"elapsed_seconds\":";

    const auto make_tail = [&state](bool with_settings) {
        Json settings = Json::object();
        settings["initial"] = with_settings ? state.initial_settings : Json::object();
        settings["final"] = nullptr;
        Json rest = Json::object();
        rest["tunables"] = state.tunables;
        rest["settings"] = std::move(settings);
        // The members of `rest` continue the document: its opening brace is dropped, and its
        // closing one ends the document.
        std::string text = rest.dump(-1, ' ', false, replace);
        text[0] = ',';
        text += '\n';
        return text;
    };
    std::string tail = make_tail(true);
    if (tail.size() > CrashTailCapacity) {
        tail = make_tail(false);
    }
    if (head.size() > CrashHeadCapacity || tail.size() > CrashTailCapacity) {
        return;
    }
    const int current = crash_document.load(std::memory_order_relaxed);
    const int next = current == 0 ? 1 : 0;
    CrashDocument& document = crash_documents[next];
    std::memcpy(document.head, head.data(), head.size());
    document.head_size = head.size();
    std::memcpy(document.tail, tail.data(), tail.size());
    document.tail_size = tail.size();
    crash_document.store(next, std::memory_order_release);
}

/// Creates the file of the crash report (a second one, so that the normal report is written
/// without giving up the crash report) and hands the writer to RunOutcome. A path that does not
/// fit the static buffers, or a file that cannot be created, leaves the crash report off: the run
/// still ends with the host-crash exit code, without a report.
void ArmCrashReport(State& state) {
#ifdef _WIN32
    const std::wstring& temporary = state.crash_temporary.native();
    const std::wstring& destination = state.destination.native();
#else
    const std::string& temporary = state.crash_temporary.native();
    const std::string& destination = state.destination.native();
#endif
    if (temporary.size() >= CrashPathCapacity || destination.size() >= CrashPathCapacity) {
        LOG_WARNING(Frontend, "The report path is too long for a crash report: crash report off");
        return;
    }
#ifdef _WIN32
    crash_file = CreateFileW(temporary.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (crash_file == INVALID_HANDLE_VALUE) {
        LOG_WARNING(Frontend, "Cannot create the crash report file: crash report off");
        return;
    }
    std::wmemcpy(crash_temporary, temporary.c_str(), temporary.size() + 1);
    std::wmemcpy(crash_destination, destination.c_str(), destination.size() + 1);
#else
    crash_file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (crash_file < 0) {
        LOG_WARNING(Frontend, "Cannot create the crash report file: crash report off");
        return;
    }
    std::memcpy(crash_temporary, temporary.c_str(), temporary.size() + 1);
    std::memcpy(crash_destination, destination.c_str(), destination.size() + 1);
#endif
    // The temporary file of the normal report, for the crash path to remove: only when Prepare
    // created it (it did not exist before), so that nothing foreign is ever deleted.
    crash_normal_temporary[0] = 0;
    if (state.temporary_created) {
        const auto& normal = state.temporary.native();
        if (normal.size() < CrashPathCapacity) {
            for (size_t i = 0; i <= normal.size(); ++i) {
                crash_normal_temporary[i] = normal[i];
            }
        }
    }
    crash_phase.store(CrashArmed, std::memory_order_release);
    RebuildCrashDocument(state);
    if (crash_document.load(std::memory_order_acquire) < 0) {
        // The fixed text did not fit its buffers.
        crash_phase.store(CrashUnarmed, std::memory_order_release);
        CloseCrashFile();
        std::error_code ec;
        fs::remove(state.crash_temporary, ec);
        LOG_WARNING(Frontend, "The crash report text does not fit its buffer: crash report off");
        return;
    }
    Common::RunOutcome::SetCrashReportWriter(&WriteCrashReport);
}

/// The normal report is on disk (or its writing failed): the crash report is off, and its file
/// is closed and removed. False when a crash is writing its report at this moment.
bool FinishCrashReport(State& state) {
    int expected = CrashArmed;
    if (crash_phase.compare_exchange_strong(expected, CrashPublishing,
                                            std::memory_order_acq_rel)) {
        CloseCrashFile();
        std::error_code ec;
        fs::remove(state.crash_temporary, ec);
        return true;
    }
    return expected != CrashWriting;
}

} // Anonymous namespace

PrepareResult Prepare(const std::string& destination, std::string& message) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    std::error_code ec;
    const fs::path path{Common::FS::ToU8String(destination)};
    if (!path.has_filename()) {
        message = "--report needs a file name, not a directory: " + destination;
        return PrepareResult::BadDestination;
    }
    if (fs::exists(fs::symlink_status(path, ec))) {
        message = "--report destination already exists (it is never overwritten): " + destination;
        return PrepareResult::BadDestination;
    }
    fs::path parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    if (!fs::is_directory(parent, ec)) {
        message = "--report directory does not exist: " +
                  Common::FS::ToUTF8String(parent.u8string());
        return PrepareResult::BadDestination;
    }
    fs::path temporary = path;
    temporary += "." + std::to_string(ProcessId()) + ".tmp";
    fs::path crash_temporary_path = path;
    crash_temporary_path += "." + std::to_string(ProcessId()) + ".crash.tmp";
    std::error_code exists_ec;
    const bool temporary_existed = fs::exists(fs::symlink_status(temporary, exists_ec));
    {
        std::ofstream probe{temporary, std::ios::binary | std::ios::out | std::ios::trunc};
        if (!probe) {
            message = "cannot create " + Common::FS::ToUTF8String(temporary.u8string());
            return PrepareResult::IoError;
        }
    }
    state.destination = path;
    state.temporary = std::move(temporary);
    state.temporary_created = !temporary_existed;
    state.crash_temporary = std::move(crash_temporary_path);
    state.prepared = true;
    ArmCrashReport(state);
    return PrepareResult::Ready;
}

void CaptureStart() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.tunables = SetTunables();
    state.initial_settings = ReportedSettings();
    RebuildCrashDocument(state);
}

void SetTitleId(u64 program_id) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.title_id = program_id;
    RebuildCrashDocument(state);
}

bool Publish(Common::RunOutcome::Reason reason, int code, bool shutdown_complete,
             std::string_view detail) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.prepared) {
        return true;
    }
    if (state.published) {
        return state.published_ok;
    }
    state.published = true;
    if (crash_phase.load(std::memory_order_acquire) == CrashWriting) {
        // A crash is writing its own report, which is the report of this run.
        return true;
    }
    try {
        state.published_ok =
            WriteReportFile(state, Document(state, reason, code, shutdown_complete, detail));
    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove(state.temporary, ec);
        state.published_ok = Fail(e.what());
    }
    // The crash report stayed armed until now: the normal one is on disk, or has failed.
    if (!FinishCrashReport(state)) {
        return true;
    }
    return state.published_ok;
}

void ExitWithFailure(std::string_view detail) {
    int code = 1;
    if (Common::RunOutcome::Active()) {
        Common::RunOutcome::Request(Common::RunOutcome::Reason::StartupError, detail);
        const Common::RunOutcome::Reason reason = Common::RunOutcome::CurrentReason();
        code = Common::RunOutcome::ExitCode(reason);
        if (!Publish(reason, code, false, Common::RunOutcome::Detail())) {
            code = Common::RunOutcome::ExitCode(Common::RunOutcome::Reason::ReportIoError);
        }
    }
    std::exit(code);
}

} // namespace RunReport
