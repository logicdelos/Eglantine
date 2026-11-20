// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen: the command-line options of the 0.6 modes and the rules that keep their outputs private.
// Slice A implements options.cpp (parsing, validation, help, private-output checks); the frontend
// takes the options through ParseOption and exposes them to the rest of the emulator through
// SetActiveOptions / ActiveOptions, so core and video_core never read launch_params.
//
// Option table (long options only; getopt codes 0x130..0x13F are reserved for this module,
// fork-cli3 uses 0x10B-0x10E and 0x120):
//   --capture <file.egcap>          record the selected frames of this run into a capture
//   --capture-frames <first>:<n>    the selected frames: first accepted-present id (>= 1) and a
//                                   count 1..MaxSelectedFrames
//   --replay <file.egcap>           replay a capture without the game
//   --replay-images <dir>           write live/replay pictures and frames.jsonl into this dir
//   --replay-benchmark              replay for timing only: no pictures, no per-frame readback
//   --nvn-census <file.json>        write the call-census sidecar of this run
//   --nvn-census-audit              check the census against an independent boundary count
//   --frame-times <file.jsonl>      write the raw per-frame timing log of this run
// The existing --report, --run-seconds and --run-frames keep their meaning; --replay accepts
// --report and --run-frames (a prefix of the captured frames).
//
// Combination rules (ValidateOptions): --capture needs --capture-frames, a game and an unattended
// run; --replay excludes a game, --capture and --nvn-census; --replay-images and
// --replay-benchmark need --replay and exclude each other; --nvn-census-audit needs
// --nvn-census; --frame-times needs an unattended run or --replay.
//
// Private outputs. A capture, a replay image directory, a census sidecar and a timing log hold
// data derived from a game. They are never written inside a git worktree, never inside a release
// staging directory, and never over an existing file (CheckPrivateOutput). A directory that
// receives them carries the marker file PrivateMarkerName; the release tooling rejects any tree
// that contains it, and a release staging directory carries ReleaseStagingMarkerName so the
// emulator refuses to write into it.

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"

namespace VideoCore::Replay {

enum OptionCode : int {
    OptionCapture = 0x130,
    OptionCaptureFrames = 0x131,
    OptionReplay = 0x132,
    OptionReplayImages = 0x133,
    OptionReplayBenchmark = 0x134,
    OptionNvnCensus = 0x135,
    OptionNvnCensusAudit = 0x136,
    OptionFrameTimes = 0x137,
};
inline constexpr int FirstOptionCode = 0x130;
inline constexpr int LastOptionCode = 0x13F;

struct OptionSpec {
    const char* name;           // without the leading dashes
    bool takes_argument;
    int code;
    const char* argument_name;  // for the help text, nullptr when no argument
    const char* help;
};

// The table above, in declaration order. The frontend copies it into its long-option array.
[[nodiscard]] std::span<const OptionSpec> OptionSpecs() noexcept;

[[nodiscard]] constexpr bool IsReplayOptionCode(int code) noexcept {
    return code >= FirstOptionCode && code <= LastOptionCode;
}

inline constexpr std::string_view PrivateMarkerName = ".egl-private";
inline constexpr std::string_view ReleaseStagingMarkerName = ".egl-release-staging";

struct Options {
    std::string capture_path;
    u64 capture_first_frame = 0;
    u32 capture_frame_count = 0;
    bool capture_frames_given = false;
    std::string replay_path;
    std::string replay_images_dir;
    bool replay_benchmark = false;
    std::string census_path;
    bool census_audit = false;
    std::string frame_times_path;

    [[nodiscard]] bool CaptureRequested() const noexcept {
        return !capture_path.empty();
    }
    [[nodiscard]] bool ReplayRequested() const noexcept {
        return !replay_path.empty();
    }
    [[nodiscard]] bool CensusRequested() const noexcept {
        return !census_path.empty();
    }
    [[nodiscard]] bool Any() const noexcept {
        return CaptureRequested() || ReplayRequested() || CensusRequested() ||
               !frame_times_path.empty() || capture_frames_given || census_audit ||
               replay_benchmark || !replay_images_dir.empty();
    }
};

// Parses one option (`code` is the getopt value) into `options`. Returns false and a one-line
// message in `error` for an empty value, a repeated option, a malformed --capture-frames value or
// a path with the wrong extension; true otherwise. The message is printable to the operator.
[[nodiscard]] bool ParseOption(int code, const char* argument, Options& options,
                               std::string& error);

struct RunContext {
    bool has_game_path = false;
    bool unattended = false;  // a report path, --run-seconds or --run-frames was given
};

// Checks the combination rules once all options are parsed. Returns false with a message.
[[nodiscard]] bool ValidateOptions(const Options& options, const RunContext& context,
                                   std::string& error);

// One help line pair per option, ready to append to the emulator's usage text.
[[nodiscard]] std::string HelpText();

// The options of this run, set once by the frontend before the emulator starts.
void SetActiveOptions(const Options& options);
[[nodiscard]] Options ActiveOptions();

enum class OutputKind { File, Directory };

// Refuses a destination that exists already (a directory may exist when it is empty), that lies
// inside a git worktree (an ancestor holds ".git") or inside a release staging directory (an
// ancestor holds ReleaseStagingMarkerName). Creates nothing.
[[nodiscard]] bool CheckPrivateOutput(const std::filesystem::path& path, OutputKind kind,
                                      std::string& error);

// Creates the directory when missing and writes the PrivateMarkerName file into it when absent.
[[nodiscard]] bool MarkDirectoryPrivate(const std::filesystem::path& directory,
                                        std::string& error);

} // namespace VideoCore::Replay
