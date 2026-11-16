// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    json["revision"] = Common::g_scm_rev;
    json["branch"] = Common::g_scm_branch;
    json["description"] = Common::g_scm_desc;
    json["build_id"] = Common::g_build_id;
    json["build_date"] = Common::g_build_date;
    json["compiler"] = Common::g_compiler_id;
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
    document["schema_version"] = 1;
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

bool WriteFile(State& state, const std::string& text) {
    std::error_code ec;
    {
        std::ofstream out{state.temporary, std::ios::binary | std::ios::out | std::ios::trunc};
        if (!out) {
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
    fs::rename(state.temporary, state.destination, ec);
    if (ec) {
        const std::string reason = ec.message();
        fs::remove(state.temporary, ec);
        return Fail("renaming to " + Common::FS::ToUTF8String(state.destination.u8string()) +
                    " failed: " + reason);
    }
    return true;
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
    {
        std::ofstream probe{temporary, std::ios::binary | std::ios::out | std::ios::trunc};
        if (!probe) {
            message = "cannot create " + Common::FS::ToUTF8String(temporary.u8string());
            return PrepareResult::IoError;
        }
    }
    state.destination = path;
    state.temporary = std::move(temporary);
    state.prepared = true;
    return PrepareResult::Ready;
}

void CaptureStart() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.tunables = SetTunables();
    state.initial_settings = ReportedSettings();
}

void SetTitleId(u64 program_id) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.title_id = program_id;
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
    try {
        state.published_ok =
            WriteFile(state, Document(state, reason, code, shutdown_complete, detail));
    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove(state.temporary, ec);
        state.published_ok = Fail(e.what());
    }
    return state.published_ok;
}

} // namespace RunReport
