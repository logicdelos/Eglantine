// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include "common/common_types.h"
#include "common/env_gate.h"
#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "video_core/cache_envelope.h"
#include "video_core/nx2_spikes.h"
#include "video_core/rasterizer_interface.h"

// What both frontends (yuzu_cmd and the Qt emulation thread) do around the cold start, so that
// the two cannot drift apart: whether attribution runs, the boot phases, the identity that is
// handed to the caches before they load, and the interactive marker the lab gives through the
// live keys file. Everything here is inline except what only yuzu_cmd needs (run_control.cpp).
namespace RunControl {

namespace Spikes = VideoCommon::Nx2Spikes;

/// Attribution (long frame causes, the cold start phases) runs unless EGLANTINE_ATTRIBUTION=0.
/// The switch exists for the cost comparison of the first-minutes gate and for a run that must
/// not pay even the two clock reads of a span; a report then says attribution.enabled=false.
[[nodiscard]] inline bool AttributionRequested() {
    return Common::EnvGate("EGLANTINE_ATTRIBUTION");
}

// ---------------------------------------------------------------------------------------------
// Interactive marker
//
// The lab drives the cold start gate without a control channel: when its scene check sees the
// reference scene respond to the prescribed input, it appends one line to the live keys file
// (EDEN_NX2_DIAG_DIR/keys.txt, which the frontend already polls about 20 times a second):
//
//   interactive <scene_id> <pass|fail> [<origin>]
//
// scene_id and origin are 1..64 and 1..32 characters of [A-Za-z0-9_.-]; the origin defaults to
// "lab". The marker time is the moment the frontend takes the line from the file (the frontend's
// own NowNs clock, so it can never be backdated by the writer): the interval first present to
// interactive therefore includes the lab's detection latency, the same in both launches of a
// pair. A line that does not match the grammar exactly is ignored and logged; a second marker, a
// marker before the first host present or one with no session is refused by MarkInteractive.
// ---------------------------------------------------------------------------------------------

inline constexpr std::string_view MarkerKeyword = "interactive";
inline constexpr size_t MaxSceneIdLength = 64;
inline constexpr size_t MaxOriginLength = 32;
inline constexpr std::string_view DefaultMarkerOrigin = "lab";

namespace Detail {

[[nodiscard]] inline bool IsTokenChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.';
}

[[nodiscard]] inline bool IsValidToken(std::string_view token, size_t max_length) noexcept {
    return !token.empty() && token.size() <= max_length &&
           std::all_of(token.begin(), token.end(), IsTokenChar);
}

[[nodiscard]] inline bool IsBlank(char c) noexcept {
    return c == ' ' || c == '\t';
}

} // namespace Detail

/// The marker a line of the live keys file states, stamped `now_ns`; nullopt when the line does
/// not match the grammar above (empty, a missing or extra field, another keyword, a scene id or
/// origin of the wrong length or characters, a verdict that is not exactly pass or fail).
[[nodiscard]] inline std::optional<Spikes::InteractiveMarker> ParseMarkerLine(
    std::string_view line, u64 now_ns) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             Detail::IsBlank(line.back()))) {
        line.remove_suffix(1);
    }
    std::array<std::string_view, 5> tokens{};
    size_t count = 0;
    size_t position = 0;
    while (position < line.size()) {
        while (position < line.size() && Detail::IsBlank(line[position])) {
            ++position;
        }
        if (position == line.size()) {
            break;
        }
        const size_t start = position;
        while (position < line.size() && !Detail::IsBlank(line[position])) {
            ++position;
        }
        if (count == tokens.size()) {
            return std::nullopt;
        }
        tokens[count++] = line.substr(start, position - start);
    }
    if ((count != 3 && count != 4) || tokens[0] != MarkerKeyword ||
        !Detail::IsValidToken(tokens[1], MaxSceneIdLength) ||
        (tokens[2] != "pass" && tokens[2] != "fail")) {
        return std::nullopt;
    }
    if (count == 4 && !Detail::IsValidToken(tokens[3], MaxOriginLength)) {
        return std::nullopt;
    }
    Spikes::InteractiveMarker marker;
    marker.scene_id = std::string{tokens[1]};
    marker.origin = std::string{count == 4 ? tokens[3] : DefaultMarkerOrigin};
    marker.marker_ns = now_ns;
    marker.scene_check_passed = tokens[2] == "pass";
    return marker;
}

/// Handles one line of the live keys file that starts with the marker keyword: parses it, hands
/// it to the recorder and logs the outcome ("Nx2Startup interactive ..."). yuzu_cmd only.
void HandleMarkerLine(std::string_view line);

// ---------------------------------------------------------------------------------------------
// Cold start phases
// ---------------------------------------------------------------------------------------------

[[nodiscard]] constexpr std::string_view PhaseOutcomeName(Spikes::PhaseOutcome outcome) noexcept {
    switch (outcome) {
    case Spikes::PhaseOutcome::NotRun:
        return "not_run";
    case Spikes::PhaseOutcome::Ok:
        return "ok";
    case Spikes::PhaseOutcome::Cancelled:
        return "cancelled";
    case Spikes::PhaseOutcome::Error:
        return "error";
    }
    return "invalid";
}

/// One cold start phase: begun here, ended by End or, when the function is left some other way,
/// as an error. The end is also logged as "Nx2Startup phase=<name> outcome=<name> elapsed_ms=<n>".
class BootPhaseScope {
public:
    explicit BootPhaseScope(Spikes::BootPhase phase_)
        : phase{phase_}, begin_ns{Spikes::NowNs()} {
        Spikes::BeginBootPhase(phase);
    }
    ~BootPhaseScope() {
        End(Spikes::PhaseOutcome::Error);
    }
    BootPhaseScope(const BootPhaseScope&) = delete;
    BootPhaseScope& operator=(const BootPhaseScope&) = delete;

    void End(Spikes::PhaseOutcome outcome) {
        if (ended) {
            return;
        }
        ended = true;
        Spikes::EndBootPhase(phase, outcome);
        const double elapsed_ms = static_cast<double>(Spikes::NowNs() - begin_ns) / 1.0e6;
        LOG_INFO(Frontend, "Nx2Startup phase={} outcome={} elapsed_ms={:.1f}",
                 Spikes::BootPhaseNames[static_cast<size_t>(phase)], PhaseOutcomeName(outcome),
                 elapsed_ms);
    }

private:
    Spikes::BootPhase phase;
    u64 begin_ns;
    bool ended{};
};

/// The two phases inside RasterizerInterface::LoadDiskResources, which the frontend can only
/// observe through its progress callback: cache_validate runs until the cache reports its first
/// Build step, pipeline_replay from then until the load returns. A load that never reports a
/// Build step is all cache_validate.
class DiskLoadPhases {
public:
    DiskLoadPhases() : validate{std::in_place, Spikes::BootPhase::CacheValidate} {}

    void OnStage(VideoCore::LoadCallbackStage stage) {
        if (stage == VideoCore::LoadCallbackStage::Build && validate) {
            validate->End(Spikes::PhaseOutcome::Ok);
            validate.reset();
            replay.emplace(Spikes::BootPhase::PipelineReplay);
        }
    }

    /// The load returned (or was stopped).
    void Finish(Spikes::PhaseOutcome outcome) {
        if (validate) {
            validate->End(outcome);
            validate.reset();
        }
        if (replay) {
            replay->End(outcome);
            replay.reset();
        }
    }

private:
    std::optional<BootPhaseScope> validate;
    std::optional<BootPhaseScope> replay;
};

// ---------------------------------------------------------------------------------------------
// Cache identity
// ---------------------------------------------------------------------------------------------

/// FNV-1a 64 over length-prefixed fields, so that two different field lists never hash the same
/// bytes by running together.
class Fnv64 {
public:
    void Bytes(const void* data, size_t size) noexcept {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            value = (value ^ bytes[i]) * 0x100000001b3ull;
        }
    }
    void U64(u64 field) noexcept {
        for (int shift = 0; shift < 64; shift += 8) {
            const auto byte = static_cast<unsigned char>(field >> shift);
            Bytes(&byte, 1);
        }
    }
    void Text(std::string_view field) noexcept {
        U64(field.size());
        Bytes(field.data(), field.size());
    }
    [[nodiscard]] u64 Value() const noexcept {
        return value;
    }

private:
    u64 value{0xcbf29ce484222325ull};
};

/// What the caches are tied to, from the emulator and the title, before any cache is loaded:
///   build_id       the emulator build (revision, description, version, build id, date, compiler)
///   title_id       the program being loaded
///   game_version   the numeric version of the base game or its update
///   content_stamp  the game file (size and last write time; a loose dump is its main file), the
///                  executable's build id, the enabled add-ons (updates, DLC, mods) and the
///                  EDEN_NX2_WARM_ADDON option that changes what the warm archive scan builds
///   settings_hash  the settings that change pipeline keys and generated shaders
/// The device and the shader format version are added by the pipeline cache, which owns them.
[[nodiscard]] inline VideoCommon::CacheEnvelope::HostIdentity CollectHostIdentity(
    Core::System& system, u64 title_id) {
    VideoCommon::CacheEnvelope::HostIdentity host;
    host.title_id = title_id;

    Fnv64 build;
    build.Text(Common::g_scm_rev);
    build.Text(Common::g_scm_desc);
    build.Text(Common::g_build_version);
    build.Text(Common::g_build_id);
    build.Text(Common::g_build_date);
    build.Text(Common::g_compiler_id);
    host.build_id = build.Value();

    const FileSys::PatchManager patch_manager{title_id, system.GetFileSystemController(),
                                              system.GetContentProvider()};
    host.game_version = patch_manager.GetGameVersion().value_or(0);

    Fnv64 content;
    const std::string& file_path = system.GetCurrentApplicationFilePath();
    if (file_path.empty()) {
        content.Text("");
    } else {
        std::filesystem::path game_path = Common::FS::ToU8String(file_path);
        std::error_code ec;
        if (std::filesystem::is_directory(game_path, ec)) {
            game_path /= "main";
        }
        const auto size = std::filesystem::file_size(game_path, ec);
        content.U64(ec ? u64{0} : static_cast<u64>(size));
        const auto time = std::filesystem::last_write_time(game_path, ec);
        content.U64(ec ? u64{0} : static_cast<u64>(time.time_since_epoch().count()));
    }
    for (const u8 byte : system.GetApplicationProcessBuildID()) {
        content.Bytes(&byte, 1);
    }
    std::vector<FileSys::Patch> patches = patch_manager.GetPatches();
    std::erase_if(patches, [](const FileSys::Patch& patch) { return !patch.enabled; });
    std::ranges::sort(patches, [](const FileSys::Patch& a, const FileSys::Patch& b) {
        return std::tie(a.type, a.program_id, a.name, a.version) <
               std::tie(b.type, b.program_id, b.name, b.version);
    });
    for (const FileSys::Patch& patch : patches) {
        content.U64(static_cast<u64>(patch.type));
        content.U64(patch.program_id);
        content.U64(patch.numeric_version);
        content.Text(patch.name);
        content.Text(patch.version);
    }
    const char* const warm_addon = std::getenv("EDEN_NX2_WARM_ADDON");
    content.Text(warm_addon != nullptr ? warm_addon : "");
    host.content_stamp = content.Value();

    Fnv64 settings;
    settings.U64(static_cast<u64>(Settings::values.dyna_state.GetValue()));
    settings.U64(Settings::values.vertex_input_dynamic_state.GetValue() ? 1 : 0);
    host.settings_hash = settings.Value();
    return host;
}

/// Hands the identity of `title_id` to the caches (CacheEnvelope::SetHostIdentity). Call before
/// anything loads a cache or refreshes the warm file, for the first title and again for every
/// title change.
inline void PublishHostIdentity(Core::System& system, u64 title_id) {
    const VideoCommon::CacheEnvelope::HostIdentity host = CollectHostIdentity(system, title_id);
    VideoCommon::CacheEnvelope::SetHostIdentity(host);
    LOG_INFO(Frontend,
             "Nx2Cache identity title={:016x} version={} build={:016x} content={:016x} "
             "settings={:016x}",
             host.title_id, host.game_version, host.build_id, host.content_stamp,
             host.settings_hash);
}

// ---------------------------------------------------------------------------------------------
// Process creation time (yuzu_cmd only)
// ---------------------------------------------------------------------------------------------

/// When the operating system created this process, on the NowNs clock, and how far that can be
/// off (the clock read bracket plus the coarse tick of the OS's own creation time stamp).
struct ProcessCreation {
    u64 created_ns{};
    u64 uncertainty_ns{};
};

/// Maps the process creation time onto the NowNs clock. Call as early as possible. Windows only
/// (GetProcessTimes against the system time); nullopt elsewhere or when the time cannot be
/// mapped, and then the report says the creation time was not observed.
[[nodiscard]] std::optional<ProcessCreation> CaptureProcessCreation();

} // namespace RunControl

// The report text of schema 2 that has to be testable without run_report.cpp: implemented in
// report_schema2.cpp next to the formatters declared in run_report.h.
namespace RunReport::Schema2 {

/// What WriteMembers writes beside the attribution snapshot.
struct MembersInput {
    const VideoCommon::Nx2Spikes::AttributionSnapshot& snapshot;
    const VideoCommon::CacheEnvelope::CacheReport& cache;
    /// The long frame spool (JSONL as LongFrameJson writes it), or null when there is none.
    std::istream* spool;
    /// The shutdown finished and every producer was joined before the snapshot was taken.
    bool run_complete;
    /// The spool could not be created: long frames were not stored.
    bool spool_failed;
};

/// Appends `,"startup":...,"first_120s":...,"cache":...,"attribution":...` (each member after a
/// comma and a newline, so that the document must already hold a member and is not closed) and
/// streams the spooled long frames into the attribution object without holding them in memory.
/// attribution.complete is true only when the snapshot is complete, the run was complete, the spool
/// was usable and holds exactly the long frames the recorder counted: a last line without its
/// newline (a write cut short) is dropped. False when the stream failed.
[[nodiscard]] bool WriteMembers(std::ostream& out, const MembersInput& input);

/// The crash path, for buffers prepared before the run: no allocation, no lock. The head is
/// `,"attribution":{...,"long_frames":[` (complete is false, status host_crash), the tail
/// `]},"startup":null,"first_120s":null,"cache":null`; both return the size written, 0 when
/// `capacity` is too small. JoinSpoolLines rewrites `size` bytes of a spool excerpt in place as
/// comma separated complete lines (a last line without its newline is dropped) and returns the new
/// size.
[[nodiscard]] size_t CrashMembersHead(char* out, size_t capacity, bool enabled,
                                      u64 threshold_ns) noexcept;
[[nodiscard]] size_t CrashMembersTail(char* out, size_t capacity) noexcept;
[[nodiscard]] size_t JoinSpoolLines(char* data, size_t size) noexcept;

} // namespace RunReport::Schema2
