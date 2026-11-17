// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/env_gate.h"
#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/loader/loose_directory_rules.h"
#include "video_core/cache_envelope.h"
#include "video_core/nx2_spikes.h"
#include "video_core/rasterizer_interface.h"
#include "yuzu_cmd/content_manifest.h"

// What both frontends (yuzu_cmd and the Qt emulation thread) do around the cold start, so that
// the two cannot drift apart: whether attribution runs, the boot phases, the identity that is
// handed to the caches before they load, and the interactive marker the lab gives through the
// live keys file. Everything here is inline except what only yuzu_cmd needs (run_control.cpp).
namespace RunControl {

namespace Spikes = VideoCommon::Nx2Spikes;
using HostIdentity = VideoCommon::CacheEnvelope::HostIdentity;

/// Attribution (long frame causes, the cold start phases) runs unless EDEN_NX2_ATTRIBUTION=0.
/// The switch exists for the cost comparison of the first-minutes gate and for a run that must
/// not pay even the two clock reads of a span; a report then says attribution.enabled=false.
[[nodiscard]] inline bool AttributionRequested() {
    return Common::EnvGate("EDEN_NX2_ATTRIBUTION");
}

// ---------------------------------------------------------------------------------------------
// Session: the credentials of an interactive marker
//
// The title and a random nonce of this run are written to <EDEN_NX2_DIAG_DIR>/session.txt before
// the first present (and again when the title changes), and are part of the report
// (startup.session). The lab reads them there and puts them into its marker, so that a line left
// over from an earlier run, or meant for another title, can never become this run's marker.
// The nonce guards against stale and misdirected lines, not against an adversary.
// ---------------------------------------------------------------------------------------------

/// Lines refused before or after they reached the recorder, by reason.
struct MarkerRejections {
    u64 malformed{};
    u64 missing_credentials{};
    u64 title_mismatch{};
    u64 nonce_mismatch{};
    u64 refused{}; ///< Well formed and current, but MarkInteractive said no (duplicate, backdated)
};

/// What the content manifest of the identity assembly cost and covered. The duration is attribution
/// data: it is not measured (empty) when attribution is off.
struct ManifestSummary {
    u64 entries{};
    bool over_cap{};
    std::optional<u64> duration_ns;
};

struct SessionInfo {
    u64 title_id{};
    u64 nonce{}; ///< 0 until a session was published (attribution off, or not yet loaded)
    MarkerRejections rejected;
    ManifestSummary manifest;
};

namespace Detail {

struct SessionState {
    std::mutex mutex;
    SessionInfo info;
};

inline SessionState& Session() {
    static SessionState state;
    return state;
}

} // namespace Detail

[[nodiscard]] inline SessionInfo ReadSession() {
    auto& state = Detail::Session();
    const std::scoped_lock lock{state.mutex};
    return state.info;
}

inline void SetSession(u64 title_id, u64 nonce) {
    auto& state = Detail::Session();
    const std::scoped_lock lock{state.mutex};
    state.info.title_id = title_id;
    state.info.nonce = nonce;
}

inline void RecordManifest(const ManifestSummary& summary) {
    auto& state = Detail::Session();
    const std::scoped_lock lock{state.mutex};
    state.info.manifest = summary;
}

/// The text of session.txt, exactly two lines.
[[nodiscard]] inline std::string SessionFileText(u64 title_id, u64 nonce) {
    return fmt::format("title {:016x}\nnonce {:016x}\n", title_id, nonce);
}

/// A new nonce: random_device, the clock and an address, mixed (never 0).
[[nodiscard]] inline u64 NewNonce() {
    std::random_device device;
    Fnv64 mix;
    mix.U64((static_cast<u64>(device()) << 32) | device());
    mix.U64(Spikes::NowNs());
    mix.U64(static_cast<u64>(std::chrono::system_clock::now().time_since_epoch().count()));
    mix.U64(static_cast<u64>(reinterpret_cast<std::uintptr_t>(&device)));
    return mix.Value() != 0 ? mix.Value() : 1;
}

/// Writes session.txt (to a temporary file, then renamed over the old one, so that the lab never
/// reads half of it). False when there is no EDEN_NX2_DIAG_DIR or the file cannot be written.
inline bool WriteSessionFile(u64 title_id, u64 nonce) {
    const char* const directory = std::getenv("EDEN_NX2_DIAG_DIR");
    if (directory == nullptr || *directory == '\0') {
        return false;
    }
    const std::filesystem::path file = PathFromText(directory) / "session.txt";
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        out << SessionFileText(title_id, nonce);
        if (!out.flush()) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, file, ec);
    return !ec;
}

/// Starts (or, for a title change, continues) the session of this run: a nonce the first time,
/// then session.txt for `title_id`. Does nothing when attribution is off: no marker can be
/// accepted then, so there is nothing to bind it to.
inline void PublishSession(u64 title_id) {
    if (!AttributionRequested()) {
        return;
    }
    u64 nonce = ReadSession().nonce;
    if (nonce == 0) {
        nonce = NewNonce();
    }
    SetSession(title_id, nonce);
    if (!WriteSessionFile(title_id, nonce)) {
        LOG_INFO(Frontend, "Nx2Startup session file not written");
    }
}

// ---------------------------------------------------------------------------------------------
// Interactive marker
//
// The lab drives the cold start gate without a control channel: when its scene check sees the
// reference scene respond to the prescribed input, it appends one line to the live keys file
// (EDEN_NX2_DIAG_DIR/keys.txt, which the frontend already polls about 20 times a second):
//
//   interactive <scene_id> <pass|fail> <title16hex> <nonce16hex> [<origin>]
//
// scene_id and origin are 1..64 and 1..32 characters of [A-Za-z0-9_.-]; the origin defaults to
// "lab"; title and nonce are exactly 16 lowercase hex digits, as session.txt gives them. The
// marker time is the moment the frontend takes the line from the file (the frontend's own NowNs
// clock, so it can never be backdated by the writer): the interval first present to interactive
// therefore includes the lab's detection latency, the same in both launches of a pair. A line
// that does not match the grammar exactly, or carries another title or nonce than this run's, is
// refused and counted in the report; a second marker or a marker before the first host present is
// refused by MarkInteractive.
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

/// Exactly 16 lowercase hex digits.
[[nodiscard]] inline std::optional<u64> ParseHex16(std::string_view text) noexcept {
    if (text.size() != 16) {
        return std::nullopt;
    }
    u64 value = 0;
    for (const char c : text) {
        u64 digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<u64>(c - 'a') + 10;
        } else {
            return std::nullopt;
        }
        value = (value << 4) | digit;
    }
    return value;
}

struct Tokens {
    std::array<std::string_view, 7> tokens{};
    size_t count{};
    bool overflow{};
};

/// The blank separated tokens of a line, its line end and trailing blanks removed.
[[nodiscard]] inline Tokens Tokenize(std::string_view line) noexcept {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || IsBlank(line.back()))) {
        line.remove_suffix(1);
    }
    Tokens result;
    size_t position = 0;
    while (position < line.size()) {
        while (position < line.size() && IsBlank(line[position])) {
            ++position;
        }
        if (position == line.size()) {
            break;
        }
        const size_t start = position;
        while (position < line.size() && !IsBlank(line[position])) {
            ++position;
        }
        if (result.count == result.tokens.size()) {
            result.overflow = true;
            return result;
        }
        result.tokens[result.count++] = line.substr(start, position - start);
    }
    return result;
}

} // namespace Detail

/// Whether a line of the live keys file is a marker line: its first token, after any leading
/// blanks, is the keyword. The same test routes the line to the parser as the parser applies, so
/// that nothing it accepts is lost on the way.
[[nodiscard]] inline bool IsMarkerLine(std::string_view line) noexcept {
    const Detail::Tokens tokens = Detail::Tokenize(line);
    return tokens.count != 0 && tokens.tokens[0] == MarkerKeyword;
}

enum class MarkerParse : u8 {
    NotMarker,
    Malformed,
    MissingCredentials, ///< A marker without title and nonce (the grammar of an earlier version)
    TitleMismatch,
    NonceMismatch,
    Valid,
};

[[nodiscard]] constexpr std::string_view MarkerParseName(MarkerParse parse) noexcept {
    switch (parse) {
    case MarkerParse::NotMarker:
        return "not_marker";
    case MarkerParse::Malformed:
        return "malformed";
    case MarkerParse::MissingCredentials:
        return "missing_credentials";
    case MarkerParse::TitleMismatch:
        return "title_mismatch";
    case MarkerParse::NonceMismatch:
        return "nonce_mismatch";
    case MarkerParse::Valid:
        return "valid";
    }
    return "invalid";
}

struct MarkerClassification {
    MarkerParse parse{MarkerParse::NotMarker};
    Spikes::InteractiveMarker marker; ///< Set when parse is Valid
};

/// Classifies one line against the credentials of this run, stamping a valid marker `now_ns`.
/// A `nonce` of 0 (no session) never matches. Title and nonce are compared only after the whole
/// line has been checked against the grammar.
[[nodiscard]] inline MarkerClassification ClassifyMarkerLine(std::string_view line, u64 title_id,
                                                              u64 nonce, u64 now_ns) {
    const Detail::Tokens parsed = Detail::Tokenize(line);
    const auto& tokens = parsed.tokens;
    MarkerClassification result;
    if (parsed.count == 0 || tokens[0] != MarkerKeyword) {
        return result;
    }
    result.parse = MarkerParse::Malformed;
    if (parsed.overflow || parsed.count < 3 || !Detail::IsValidToken(tokens[1], MaxSceneIdLength) ||
        (tokens[2] != "pass" && tokens[2] != "fail")) {
        return result;
    }
    if (parsed.count == 3 || parsed.count == 4) {
        result.parse = MarkerParse::MissingCredentials;
        return result;
    }
    if (parsed.count > 6) {
        return result;
    }
    const std::optional<u64> marker_title = Detail::ParseHex16(tokens[3]);
    const std::optional<u64> marker_nonce = Detail::ParseHex16(tokens[4]);
    if (!marker_title || !marker_nonce ||
        (parsed.count == 6 && !Detail::IsValidToken(tokens[5], MaxOriginLength))) {
        return result;
    }
    if (*marker_title != title_id) {
        result.parse = MarkerParse::TitleMismatch;
        return result;
    }
    if (nonce == 0 || *marker_nonce != nonce) {
        result.parse = MarkerParse::NonceMismatch;
        return result;
    }
    result.parse = MarkerParse::Valid;
    result.marker.scene_id = std::string{tokens[1]};
    result.marker.origin = std::string{parsed.count == 6 ? tokens[5] : DefaultMarkerOrigin};
    result.marker.marker_ns = now_ns;
    result.marker.scene_check_passed = tokens[2] == "pass";
    return result;
}

inline void CountMarkerRejection(MarkerParse parse) {
    auto& state = Detail::Session();
    const std::scoped_lock lock{state.mutex};
    MarkerRejections& counts = state.info.rejected;
    switch (parse) {
    case MarkerParse::Malformed:
        ++counts.malformed;
        break;
    case MarkerParse::MissingCredentials:
        ++counts.missing_credentials;
        break;
    case MarkerParse::TitleMismatch:
        ++counts.title_mismatch;
        break;
    case MarkerParse::NonceMismatch:
        ++counts.nonce_mismatch;
        break;
    case MarkerParse::NotMarker:
    case MarkerParse::Valid:
        break;
    }
}

inline void CountRefusedMarker() {
    auto& state = Detail::Session();
    const std::scoped_lock lock{state.mutex};
    ++state.info.rejected.refused;
}

/// What HandleMarkerLine does with a line before the recorder: the credentials of this run, the
/// classification, and the count of a refusal. Valid is the only result that goes on.
[[nodiscard]] inline MarkerClassification DispatchMarkerLine(std::string_view line, u64 now_ns) {
    const SessionInfo session = ReadSession();
    MarkerClassification classification =
        ClassifyMarkerLine(line, session.title_id, session.nonce, now_ns);
    CountMarkerRejection(classification.parse);
    return classification;
}

/// Handles one marker line of the live keys file (IsMarkerLine): refuses it, or hands it to the
/// recorder, and logs the outcome ("Nx2Startup interactive ..."). yuzu_cmd only.
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
/// With attribution off the scope is inert: no clock read, no recorder call, no log line.
class BootPhaseScope {
public:
    explicit BootPhaseScope(Spikes::BootPhase phase_)
        : phase{phase_}, active{Spikes::AttributionEnabled()} {
        if (active) {
            begin_ns = Spikes::NowNs();
            Spikes::BeginBootPhase(phase);
        }
    }
    ~BootPhaseScope() {
        End(Spikes::PhaseOutcome::Error);
    }
    BootPhaseScope(const BootPhaseScope&) = delete;
    BootPhaseScope& operator=(const BootPhaseScope&) = delete;

    [[nodiscard]] bool Active() const noexcept {
        return active && !ended;
    }

    void End(Spikes::PhaseOutcome outcome) {
        if (!Active()) {
            ended = true;
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
    bool active;
    u64 begin_ns{};
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

/// The EDEN_NX2_WARM_ADDON option, which changes what the warm archive scan builds.
[[nodiscard]] inline std::string WarmAddonsOption() {
    const char* const value = std::getenv("EDEN_NX2_WARM_ADDON");
    return value != nullptr ? value : "";
}

/// Everything the identity of one title is assembled from. The runtime fills it from the loaded
/// system (SourceFromSystem); an offline tool fills it from the game it is building for, so that
/// what it stamps is the title being built, never the process-global identity of whatever ran
/// before. The order of the lists does not matter: AssembleHostIdentity sorts them.
struct HostIdentitySource {
    u64 title_id{};
    u32 game_version{};
    /// The game file, or a loose dump directory (every file of it is in the manifest). A launch
    /// path that is the `main` of a loose ExeFS (or such an ExeFS directory) also stands for the
    /// files the deconstructed loader uses with it: see ResolveLooseContent.
    std::filesystem::path game_path;
    /// The enabled add-ons, one line each: type, program, version number, name, version text.
    std::vector<std::string> labels;
    /// Update and add-on content files: size and last write time.
    std::vector<std::filesystem::path> content_files;
    /// Enabled mod directories (and loose RomFS directories): every file's relative path, size
    /// and last write time.
    std::vector<std::filesystem::path> content_trees;
    /// The EDEN_NX2_WARM_ADDON option, which changes what the warm archive scan builds.
    std::string warm_addons;
    size_t max_manifest_entries{MaxManifestEntries};
};

/// The environment switches that change the shaders or the pipeline keys generated for the same
/// guest program. Value and presence of each go into the settings hash.
inline constexpr std::array<std::string_view, 15> ShaderGenerationSwitches{
    "EDEN_NX2_UNDEF_ZERO",
    "EDEN_NX2_LEAN_SSBO",
    "EDEN_NX2_LOWER_EXTENDED",
    "EDEN_NX2_TENSOR",
    "EDEN_NX2_WIDE_COMPUTE_SSBOS",
    "EDEN_NX2_RESETFIX",
    "EDEN_NX2_EXPERIMENTAL_GEOMETRY",
    "EDEN_NX2_GEOMETRY_GUARD",
    "EDEN_NX2_REJECT",
    "EDEN_NX2_REJECT_HASH",
    "EDEN_NATIVE_SHADER_OVERRIDE_DIR",
    "EGLANTINE_CAS_RETRY_FIX",
    "EGLANTINE_SSBO_ALIGN_FIX",
    "EGLANTINE_DEPTH_FLOAT_FIX",
    "EGLANTINE_AMD_FRAGMENT_SHUFFLE",
};

/// The emulator build: revision, description, version, build id, date and compiler.
[[nodiscard]] inline u64 HostBuildId() {
    Fnv64 build;
    build.Text(Common::g_scm_rev);
    build.Text(Common::g_scm_desc);
    build.Text(Common::g_build_version);
    build.Text(Common::g_build_id);
    build.Text(Common::g_build_date);
    build.Text(Common::g_compiler_id);
    return build.Value();
}

/// The settings and switches that change generated shaders or pipeline keys: the dynamic state
/// levels, the squashed iterated blend, what the shader recompiler reads from the settings (the
/// same ones the translation cache signature holds) and ShaderGenerationSwitches. Presentation
/// and interface settings are not in it. Device workarounds that alter the SPIR-V follow from
/// the device and driver of the cache's device identity; the one that an environment switch
/// controls (the AMD fragment shuffle) is among the switches.
[[nodiscard]] inline u64 HostSettingsHash() {
    Fnv64 settings;
    settings.U64(static_cast<u64>(Settings::values.dyna_state.GetValue()));
    settings.U64(Settings::values.vertex_input_dynamic_state.GetValue() ? 1 : 0);
    settings.U64(Settings::values.use_squashed_iterated_blend ? 1 : 0);
    const Settings::ResolutionScalingInfo& resolution = Settings::values.resolution_info;
    settings.U64(resolution.up_scale);
    settings.U64(resolution.down_shift);
    settings.U64(resolution.active ? 1 : 0);
    settings.U64(Settings::values.renderer_debug.GetValue() ? 1 : 0);
    settings.U64(Settings::values.disable_shader_loop_safety_checks.GetValue() ? 1 : 0);
    settings.U64(Settings::values.fix_bloom_effects.GetValue() ? 1 : 0);
    for (const std::string_view name : ShaderGenerationSwitches) {
        const std::string key{name};
        const char* const value = std::getenv(key.c_str());
        settings.Text(name);
        settings.U64(value != nullptr ? 1 : 0);
        settings.Text(value != nullptr ? value : "");
    }
    return settings.Value();
}

namespace Detail {

/// The subdirectories of a directory: their paths and names, in the order the host lists them and
/// with the options the loader's own listing uses (src/common/fs/fs.cpp, IterateDirEntries),
/// because the loader's choices among directories depend on that order.
struct ListedDirectories {
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> names;
};

[[nodiscard]] inline ListedDirectories ListSubdirectories(const std::filesystem::path& parent) {
    ListedDirectories listed;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{
             parent, std::filesystem::directory_options::skip_permission_denied, ec},
         end;
         !ec && it != end; it.increment(ec)) {
        std::error_code status_ec;
        if (it->status(status_ec).type() == std::filesystem::file_type::directory) {
            listed.paths.push_back(it->path());
            listed.names.push_back(PathText(it->path().filename()));
        }
    }
    return listed;
}

/// The loader's loose RomFS directory of `parent`: `romfs`, else the first directory listed
/// whose name is `romfs` in any letter case (Loader::FirstRomFSDirectoryIndex). Empty when there
/// is none.
[[nodiscard]] inline std::filesystem::path FindLooseRomFSDirectory(
    const std::filesystem::path& parent) {
    std::error_code ec;
    if (std::filesystem::path exact = parent / "romfs"; std::filesystem::is_directory(exact, ec)) {
        return exact;
    }
    ListedDirectories listed = ListSubdirectories(parent);
    if (const auto index = Loader::FirstRomFSDirectoryIndex(listed.names)) {
        return std::move(listed.paths[*index]);
    }
    return {};
}

/// The size of the `main` in an ExeFS directory; 0 when there is none.
[[nodiscard]] inline u64 MainSize(const std::filesystem::path& exefs_directory) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(exefs_directory / "main", ec);
    return ec ? 0 : static_cast<u64>(size);
}

/// The directory of `updates` the loader takes for an absent base RomFS
/// (Loader::NewestUpdateDirectoryIndex, over the listing order); empty when there is none.
[[nodiscard]] inline std::filesystem::path NewestUpdateDirectory(
    const std::filesystem::path& updates) {
    ListedDirectories listed = ListSubdirectories(updates);
    if (const auto index = Loader::NewestUpdateDirectoryIndex(listed.names)) {
        return std::move(listed.paths[*index]);
    }
    return {};
}

} // namespace Detail

/// What the deconstructed ExeFS loader reads besides the launch path, found the way the loader
/// finds it (src/core/loader/deconstructed_rom_directory.cpp, FindLooseRomFS).
struct LooseContent {
    /// The ExeFS directory of the launch path (its main, main.npdm, rtld, modules and any
    /// `.romfs` file). Empty when the launch path is a directory: the manifest walks all of it.
    std::filesystem::path exefs_tree;
    /// The loose RomFS directory in use. Empty when the RomFS is a `.romfs` file of the ExeFS
    /// directory, or when the loader finds none or refuses the update's.
    std::filesystem::path romfs_tree;
    /// The `main` of the update directory whose RomFS stands in for an absent base RomFS: the
    /// loader compares its size with the base's, so the size belongs to the identity.
    std::filesystem::path update_main;
};

/// The loose content of a launch path that is the `main` of an ExeFS directory (one that also
/// holds main.npdm), or such a directory. The loader takes the first `.romfs` file of the ExeFS
/// directory; else the `romfs` directory beside it; else, for a base without one, the `romfs`
/// of the newest numbered directory of the `updates` directory beside that, unless that
/// update's main has another size than the base's. Any other launch path has none.
[[nodiscard]] inline LooseContent ResolveLooseContent(const std::filesystem::path& game_path) {
    std::error_code ec;
    const bool directory = std::filesystem::is_directory(game_path, ec);
    if (!directory && game_path.filename() != "main") {
        return {};
    }
    const std::filesystem::path exefs = directory ? game_path : game_path.parent_path();
    if (!std::filesystem::is_regular_file(exefs / "main", ec) ||
        !std::filesystem::is_regular_file(exefs / "main.npdm", ec)) {
        return {};
    }
    LooseContent loose;
    if (!directory) {
        loose.exefs_tree = exefs;
    }
    for (std::filesystem::directory_iterator it{exefs, ec}, end; !ec && it != end;
         it.increment(ec)) {
        std::error_code status_ec;
        if (it->is_regular_file(status_ec) && it->path().extension() == ".romfs") {
            return loose;
        }
    }

    const std::filesystem::path title = exefs.parent_path();
    loose.romfs_tree = Detail::FindLooseRomFSDirectory(title);
    if (!loose.romfs_tree.empty()) {
        return loose;
    }
    const std::filesystem::path update =
        Detail::NewestUpdateDirectory(title.parent_path() / "updates");
    if (update.empty()) {
        return loose;
    }
    const std::filesystem::path update_romfs = Detail::FindLooseRomFSDirectory(update);
    if (update_romfs.empty()) {
        return loose;
    }
    loose.update_main = update / "exefs" / "main";
    const u64 base_size = Detail::MainSize(exefs);
    const u64 update_size = Detail::MainSize(update / "exefs");
    if (base_size == 0 || update_size == 0 || base_size == update_size) {
        loose.romfs_tree = update_romfs;
    }
    return loose;
}

/// What the caches are tied to, from the emulator and the title, before any cache is loaded:
///   build_id       the emulator build
///   title_id       the program
///   game_version   the numeric version of the base game or its update
///   content_stamp  the hash of the content manifest: the game file (size and last write time; a
///                  loose dump: every file's relative path, size and time; a loose ExeFS main:
///                  also its ExeFS directory and the loose RomFS the loader uses with it, see
///                  ResolveLooseContent), the enabled add-ons by name and version, the update
///                  and add-on files (size and time), every file of every enabled mod and loose
///                  RomFS directory (relative path, size, time),
///                  the EDEN_NX2_WARM_ADDON option and whether the walk hit its cap. The
///                  contents of files are not read; the executable's build id is not in it
///                  because only a running guest knows it, and an offline build must stamp the
///                  same value: the game file's own size and time cover the executable.
///   settings_hash  HostSettingsHash
/// The device and the shader format version are added by the pipeline cache, which owns them.
/// The walk is a disk read of the cold start: it is a disk span of the attribution, and its
/// duration, entry count and cap flag are in `summary` (and in the report). The duration is read
/// from `now_ns` only while attribution runs: with attribution off the walk is done (the
/// caches need the identity) but no clock is read for it and `summary->duration_ns` stays empty.
[[nodiscard]] inline HostIdentity AssembleHostIdentity(const HostIdentitySource& source,
                                                       ManifestSummary* summary = nullptr,
                                                       u64 (*now_ns)() = &Spikes::NowNs) {
    const bool timed = summary != nullptr && Spikes::AttributionEnabled();
    const u64 begin_ns = timed ? now_ns() : 0;
    Spikes::SpanScope disk{Spikes::Cause::Disk};

    HostIdentity host;
    host.title_id = source.title_id;
    host.game_version = source.game_version;
    host.build_id = HostBuildId();
    host.settings_hash = HostSettingsHash();

    ContentManifest manifest{source.max_manifest_entries};
    manifest.Number("title", source.title_id);
    manifest.Number("version", source.game_version);
    std::error_code ec;
    if (source.game_path.empty()) {
        manifest.Text("game", "");
    } else if (std::filesystem::is_directory(source.game_path, ec)) {
        manifest.Tree("game_directory", source.game_path);
    } else {
        manifest.File("game", source.game_path);
    }
    const LooseContent loose = ResolveLooseContent(source.game_path);
    if (!loose.exefs_tree.empty()) {
        manifest.Tree("loose_exefs", loose.exefs_tree);
    }
    if (!loose.romfs_tree.empty()) {
        manifest.Tree("loose_romfs", loose.romfs_tree);
    }
    if (!loose.update_main.empty()) {
        manifest.File("loose_update_main", loose.update_main);
    }

    std::vector<std::string> labels = source.labels;
    std::ranges::sort(labels);
    labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
    for (const std::string& label : labels) {
        manifest.Text("addon", label);
    }

    const auto sorted_paths = [](const std::vector<std::filesystem::path>& paths) {
        std::vector<std::string> texts;
        texts.reserve(paths.size());
        for (const std::filesystem::path& path : paths) {
            texts.push_back(PathText(path));
        }
        std::ranges::sort(texts);
        texts.erase(std::unique(texts.begin(), texts.end()), texts.end());
        return texts;
    };
    for (const std::string& path : sorted_paths(source.content_files)) {
        manifest.File("content_file", PathFromText(path));
    }
    for (const std::string& path : sorted_paths(source.content_trees)) {
        manifest.Tree("content_tree", PathFromText(path));
    }
    manifest.Text("warm_addons", source.warm_addons);
    host.content_stamp = manifest.Digest();

    if (summary != nullptr) {
        summary->entries = manifest.Entries();
        summary->over_cap = manifest.OverCap();
        summary->duration_ns.reset();
        if (timed) {
            summary->duration_ns = now_ns() - begin_ns;
        }
    }
    return host;
}

/// The host file of a file the content provider holds (an update or add-on package), as an entry
/// of the source: the package's own file, or its name and size when it has no host file.
inline void AddProvidedFile(HostIdentitySource& source, const FileSys::VirtualFile& file) {
    if (!file) {
        return;
    }
    std::filesystem::path host = ExistingFilePrefix(file->GetFullPath());
    if (!host.empty()) {
        source.content_files.push_back(std::move(host));
    } else {
        source.labels.push_back(fmt::format("file|{}|{}", file->GetName(), file->GetSize()));
    }
}

/// The identity source of `title_id` as the running system knows it. `game_path` is the title's
/// game file or loose directory: the caller gives it, so that a tool can describe a title other
/// than the one that is loaded.
[[nodiscard]] inline HostIdentitySource SourceFromSystem(Core::System& system, u64 title_id,
                                                         std::filesystem::path game_path) {
    HostIdentitySource source;
    source.title_id = title_id;
    source.game_path = std::move(game_path);

    const FileSys::PatchManager patch_manager{title_id, system.GetFileSystemController(),
                                              system.GetContentProvider()};
    source.game_version = patch_manager.GetGameVersion().value_or(0);
    for (const FileSys::Patch& patch : patch_manager.GetPatches()) {
        if (!patch.enabled) {
            continue;
        }
        source.labels.push_back(fmt::format("{}|{:016x}|{}|{}|{}", static_cast<int>(patch.type),
                                            patch.program_id, patch.numeric_version, patch.name,
                                            patch.version));
        switch (patch.type) {
        case FileSys::PatchType::Update:
            AddProvidedFile(source, system.GetContentProvider().GetEntryRaw(
                                        patch.title_id, FileSys::ContentRecordType::Program));
            break;
        case FileSys::PatchType::DLC:
            AddProvidedFile(source, system.GetContentProvider().GetEntryRaw(
                                        patch.title_id, FileSys::ContentRecordType::Data));
            break;
        case FileSys::PatchType::Mod:
            if (!patch.location.empty()) {
                source.content_trees.push_back(PathFromText(patch.location));
            }
            break;
        }
    }
    source.warm_addons = WarmAddonsOption();
    return source;
}

/// Hands the identity of `title_id` to the caches (CacheEnvelope::SetHostIdentity) and starts the
/// session of the interactive marker. Call before anything loads a cache or refreshes the warm
/// file, for the first title and again for every title change.
inline void PublishHostIdentity(Core::System& system, u64 title_id) {
    ManifestSummary summary;
    const HostIdentity host = AssembleHostIdentity(
        SourceFromSystem(system, title_id, PathFromText(system.GetCurrentApplicationFilePath())),
        &summary);
    VideoCommon::CacheEnvelope::SetHostIdentity(host);
    RecordManifest(summary);
    const std::string manifest_ms =
        summary.duration_ns
            ? fmt::format("{:.1f}", static_cast<double>(*summary.duration_ns) / 1.0e6)
            : std::string{"unavailable"};
    LOG_INFO(Frontend,
             "Nx2Cache identity title={:016x} version={} build={:016x} content={:016x} "
             "settings={:016x} manifest_entries={} manifest_over_cap={} manifest_ms={}",
             host.title_id, host.game_version, host.build_id, host.content_stamp,
             host.settings_hash, summary.entries, summary.over_cap, manifest_ms);
    PublishSession(title_id);
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
    const RunControl::SessionInfo& session;
    /// The long frame spool (JSONL as LongFrameJson writes it), or null when there is none.
    std::istream* spool;
    /// The shutdown finished and every producer was joined before the snapshot was taken.
    bool run_complete;
    /// The spool could not be created: long frames were not stored.
    bool spool_failed;
};

/// `{"title_id":..,"nonce":..,"markers_rejected":{..},"content_manifest":{..}}`: the session of
/// the interactive marker and what the content manifest covered; it is a member of `startup`.
[[nodiscard]] std::string SessionJson(const RunControl::SessionInfo& session);

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
