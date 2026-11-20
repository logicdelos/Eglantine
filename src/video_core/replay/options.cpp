// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <charconv>
#include <fstream>
#include <mutex>
#include <system_error>

#include <fmt/format.h>

#include "video_core/replay/format.h"
#include "video_core/replay/options.h"

namespace VideoCore::Replay {

namespace {

constexpr std::array<OptionSpec, 8> Specs{{
    {"capture", true, OptionCapture, "FILE.egcap",
     "Record the selected frames of this run into a capture (private; needs --capture-frames)."},
    {"capture-frames", true, OptionCaptureFrames, "FIRST:COUNT",
     "First accepted-present id (>= 1) and number of frames to capture (1 to 120)."},
    {"replay", true, OptionReplay, "FILE.egcap",
     "Replay a capture without the game (excludes a game path, --capture and --nvn-census)."},
    {"replay-images", true, OptionReplayImages, "DIR",
     "Write live and replay pictures and frames.jsonl of a replay into DIR (private)."},
    {"replay-benchmark", false, OptionReplayBenchmark, nullptr,
     "Replay for timing only: no pictures and no per-frame readback."},
    {"nvn-census", true, OptionNvnCensus, "FILE.json",
     "Write the graphics-call census sidecar of this run (private)."},
    {"nvn-census-audit", false, OptionNvnCensusAudit, nullptr,
     "Check the census against an independent count at the library boundary."},
    {"frame-times", true, OptionFrameTimes, "FILE.jsonl",
     "Write the raw per-frame timing log of this run (private)."},
}};

bool HasSuffix(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool Fail(std::string& error, std::string message) {
    error = std::move(message);
    return false;
}

bool ParseUnsigned(std::string_view text, u64& value) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* const end = text.data() + text.size();
    const auto [pointer, status] = std::from_chars(text.data(), end, value);
    return status == std::errc{} && pointer == end;
}

// "<first>:<count>" with first >= 1 and 1 <= count <= MaxSelectedFrames.
bool ParseFrames(std::string_view text, u64& first, u32& count) noexcept {
    const size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    u64 parsed_count = 0;
    if (!ParseUnsigned(text.substr(0, colon), first) ||
        !ParseUnsigned(text.substr(colon + 1), parsed_count)) {
        return false;
    }
    if (first == 0 || parsed_count == 0 || parsed_count > MaxSelectedFrames) {
        return false;
    }
    count = static_cast<u32>(parsed_count);
    return true;
}

std::mutex active_mutex;
Options active_options;

} // namespace

std::span<const OptionSpec> OptionSpecs() noexcept {
    return Specs;
}

bool ParseOption(int code, const char* argument, Options& options, std::string& error) {
    const std::string_view value = argument != nullptr ? std::string_view{argument}
                                                       : std::string_view{};
    const auto repeated = [&](const char* name) {
        return Fail(error, fmt::format("--{} given more than once", name));
    };
    const auto need_value = [&](const char* name) {
        return Fail(error, fmt::format("--{} needs a non-empty value", name));
    };
    const auto wrong_extension = [&](const char* name, std::string_view extension) {
        return Fail(error, fmt::format("--{} value must end in {}", name, extension));
    };

    switch (code) {
    case OptionCapture:
        if (!options.capture_path.empty()) {
            return repeated("capture");
        }
        if (value.empty()) {
            return need_value("capture");
        }
        if (!HasSuffix(value, FinalExtension)) {
            return wrong_extension("capture", FinalExtension);
        }
        options.capture_path = std::string{value};
        return true;
    case OptionCaptureFrames:
        if (options.capture_frames_given) {
            return repeated("capture-frames");
        }
        if (!ParseFrames(value, options.capture_first_frame, options.capture_frame_count)) {
            return Fail(error, fmt::format("--capture-frames needs FIRST:COUNT with FIRST >= 1 "
                                           "and COUNT 1 to {}",
                                           MaxSelectedFrames));
        }
        options.capture_frames_given = true;
        return true;
    case OptionReplay:
        if (!options.replay_path.empty()) {
            return repeated("replay");
        }
        if (value.empty()) {
            return need_value("replay");
        }
        if (!HasSuffix(value, FinalExtension)) {
            return wrong_extension("replay", FinalExtension);
        }
        options.replay_path = std::string{value};
        return true;
    case OptionReplayImages:
        if (!options.replay_images_dir.empty()) {
            return repeated("replay-images");
        }
        if (value.empty()) {
            return need_value("replay-images");
        }
        options.replay_images_dir = std::string{value};
        return true;
    case OptionReplayBenchmark:
        if (options.replay_benchmark) {
            return repeated("replay-benchmark");
        }
        options.replay_benchmark = true;
        return true;
    case OptionNvnCensus:
        if (!options.census_path.empty()) {
            return repeated("nvn-census");
        }
        if (value.empty()) {
            return need_value("nvn-census");
        }
        if (!HasSuffix(value, ".json")) {
            return wrong_extension("nvn-census", ".json");
        }
        options.census_path = std::string{value};
        return true;
    case OptionNvnCensusAudit:
        if (options.census_audit) {
            return repeated("nvn-census-audit");
        }
        options.census_audit = true;
        return true;
    case OptionFrameTimes:
        if (!options.frame_times_path.empty()) {
            return repeated("frame-times");
        }
        if (value.empty()) {
            return need_value("frame-times");
        }
        if (!HasSuffix(value, ".jsonl")) {
            return wrong_extension("frame-times", ".jsonl");
        }
        options.frame_times_path = std::string{value};
        return true;
    default:
        return Fail(error, fmt::format("unknown replay option code {:#x}", code));
    }
}

bool ValidateOptions(const Options& options, const RunContext& context, std::string& error) {
    if (options.capture_frames_given && !options.CaptureRequested()) {
        return Fail(error, "--capture-frames needs --capture");
    }
    if (options.CaptureRequested()) {
        if (!options.capture_frames_given) {
            return Fail(error, "--capture needs --capture-frames FIRST:COUNT");
        }
        if (!context.has_game_path) {
            return Fail(error, "--capture needs a game to run");
        }
        if (!context.unattended) {
            return Fail(error, "--capture needs a bounded unattended run (--report, "
                               "--run-seconds or --run-frames)");
        }
    }
    if (options.ReplayRequested()) {
        if (context.has_game_path) {
            return Fail(error, "--replay runs without a game; remove the game path");
        }
        if (options.CaptureRequested() || options.CensusRequested() || options.census_audit) {
            return Fail(error, "--replay excludes --capture and --nvn-census");
        }
    } else {
        if (!options.replay_images_dir.empty()) {
            return Fail(error, "--replay-images needs --replay");
        }
        if (options.replay_benchmark) {
            return Fail(error, "--replay-benchmark needs --replay");
        }
    }
    if (options.replay_benchmark && !options.replay_images_dir.empty()) {
        return Fail(error, "--replay-benchmark and --replay-images exclude each other");
    }
    if (options.census_audit && !options.CensusRequested()) {
        return Fail(error, "--nvn-census-audit needs --nvn-census");
    }
    if (!options.frame_times_path.empty() && !context.unattended && !options.ReplayRequested()) {
        return Fail(error, "--frame-times needs a bounded unattended run or --replay");
    }
    return true;
}

std::string HelpText() {
    std::string text;
    for (const OptionSpec& spec : Specs) {
        const std::string usage =
            spec.takes_argument ? fmt::format("--{} {}", spec.name, spec.argument_name)
                                : fmt::format("--{}", spec.name);
        text += fmt::format("  {}\n      {}\n", usage, spec.help);
    }
    return text;
}

void SetActiveOptions(const Options& options) {
    const std::scoped_lock lock{active_mutex};
    active_options = options;
}

Options ActiveOptions() {
    const std::scoped_lock lock{active_mutex};
    return active_options;
}

bool CheckPrivateOutput(const std::filesystem::path& path, OutputKind kind, std::string& error) {
    namespace fs = std::filesystem;
    if (path.empty()) {
        return Fail(error, "the output path is empty");
    }
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) {
        return Fail(error, fmt::format("cannot inspect the output path: {}", ec.message()));
    }
    if (exists) {
        if (kind == OutputKind::File) {
            return Fail(error, "the output file exists already and is never overwritten");
        }
        if (!fs::is_directory(path, ec) || !fs::is_empty(path, ec)) {
            return Fail(error, "the output directory exists and is not an empty directory");
        }
    }
    fs::path canonical = fs::weakly_canonical(path, ec);
    if (ec) {
        return Fail(error, fmt::format("cannot resolve the output path: {}", ec.message()));
    }
    // Walk the ancestors (the path itself counts for a directory that already exists).
    for (fs::path current = canonical;; current = current.parent_path()) {
        if (!current.empty()) {
            if (fs::exists(current / ".git", ec)) {
                return Fail(error, "the output path lies inside a git worktree");
            }
            if (fs::exists(current / std::string{ReleaseStagingMarkerName}, ec)) {
                return Fail(error, "the output path lies inside a release staging directory");
            }
        }
        const fs::path parent = current.parent_path();
        if (parent == current || parent.empty()) {
            break;
        }
    }
    return true;
}

bool MarkDirectoryPrivate(const std::filesystem::path& directory, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        return Fail(error, fmt::format("cannot create the output directory: {}", ec.message()));
    }
    const fs::path marker = directory / std::string{PrivateMarkerName};
    if (fs::exists(marker, ec)) {
        return true;
    }
    std::ofstream file{marker, std::ios::binary};
    if (!file) {
        return Fail(error, "cannot write the private marker into the output directory");
    }
    file << "local-only data derived from a game; never publish\n";
    file.flush();
    if (!file) {
        return Fail(error, "cannot write the private marker into the output directory");
    }
    return true;
}

} // namespace VideoCore::Replay
