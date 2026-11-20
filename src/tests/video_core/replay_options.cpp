// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "video_core/replay/options.h"

namespace R = VideoCore::Replay;

namespace {

bool Parse(R::Options& options, int code, const char* argument, std::string& error) {
    error.clear();
    return R::ParseOption(code, argument, options, error);
}

std::filesystem::path ScratchDirectory(const char* name) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

} // namespace

TEST_CASE("Replay options: codes are inside the reserved range", "[video_core][replay]") {
    for (const R::OptionSpec& spec : R::OptionSpecs()) {
        REQUIRE(R::IsReplayOptionCode(spec.code));
        REQUIRE((spec.argument_name != nullptr) == spec.takes_argument);
    }
    REQUIRE_FALSE(R::IsReplayOptionCode(0x12F));
    REQUIRE_FALSE(R::IsReplayOptionCode(0x140));
}

TEST_CASE("Replay options: parsing", "[video_core][replay]") {
    R::Options options;
    std::string error;
    REQUIRE(Parse(options, R::OptionCapture, "frames.egcap", error));
    REQUIRE(Parse(options, R::OptionCaptureFrames, "120:3", error));
    REQUIRE(options.capture_first_frame == 120);
    REQUIRE(options.capture_frame_count == 3);
    REQUIRE(options.CaptureRequested());

    // Repeats and bad values are refused with a message.
    REQUIRE_FALSE(Parse(options, R::OptionCapture, "other.egcap", error));
    REQUIRE_FALSE(error.empty());

    R::Options bad;
    REQUIRE_FALSE(Parse(bad, R::OptionCapture, "frames.bin", error));
    REQUIRE_FALSE(Parse(bad, R::OptionCapture, "", error));
    REQUIRE_FALSE(Parse(bad, R::OptionCapture, nullptr, error));
    REQUIRE_FALSE(Parse(bad, R::OptionCaptureFrames, "0:3", error));
    REQUIRE_FALSE(Parse(bad, R::OptionCaptureFrames, "5:0", error));
    REQUIRE_FALSE(Parse(bad, R::OptionCaptureFrames, "5:121", error));
    REQUIRE_FALSE(Parse(bad, R::OptionCaptureFrames, "5", error));
    REQUIRE_FALSE(Parse(bad, R::OptionCaptureFrames, "5:3x", error));
    REQUIRE_FALSE(Parse(bad, R::OptionNvnCensus, "census.txt", error));
    REQUIRE_FALSE(Parse(bad, R::OptionFrameTimes, "times.json", error));
    REQUIRE_FALSE(Parse(bad, 0x13F, "x", error));
    REQUIRE_FALSE(bad.Any());

    REQUIRE(Parse(bad, R::OptionNvnCensus, "census.json", error));
    REQUIRE(Parse(bad, R::OptionNvnCensusAudit, nullptr, error));
    REQUIRE(Parse(bad, R::OptionFrameTimes, "times.jsonl", error));
    REQUIRE(bad.Any());
}

TEST_CASE("Replay options: validation", "[video_core][replay]") {
    std::string error;
    const R::RunContext live_unattended{.has_game_path = true, .unattended = true};
    const R::RunContext no_game{.has_game_path = false, .unattended = false};
    const R::RunContext live_attended{.has_game_path = true, .unattended = false};

    R::Options capture;
    REQUIRE(Parse(capture, R::OptionCapture, "a.egcap", error));
    REQUIRE_FALSE(R::ValidateOptions(capture, live_unattended, error));  // frames missing
    REQUIRE(Parse(capture, R::OptionCaptureFrames, "10:2", error));
    REQUIRE(R::ValidateOptions(capture, live_unattended, error));
    REQUIRE_FALSE(R::ValidateOptions(capture, live_attended, error));
    REQUIRE_FALSE(R::ValidateOptions(capture, no_game, error));

    R::Options replay;
    REQUIRE(Parse(replay, R::OptionReplay, "a.egcap", error));
    REQUIRE(R::ValidateOptions(replay, no_game, error));
    REQUIRE_FALSE(R::ValidateOptions(replay, live_unattended, error));  // replay needs no game

    R::Options both = replay;
    REQUIRE(Parse(both, R::OptionNvnCensus, "c.json", error));
    REQUIRE_FALSE(R::ValidateOptions(both, no_game, error));

    R::Options images = replay;
    REQUIRE(Parse(images, R::OptionReplayImages, "pictures", error));
    REQUIRE(R::ValidateOptions(images, no_game, error));
    REQUIRE(Parse(images, R::OptionReplayBenchmark, nullptr, error));
    REQUIRE_FALSE(R::ValidateOptions(images, no_game, error));

    R::Options stray;
    REQUIRE(Parse(stray, R::OptionReplayImages, "pictures", error));
    REQUIRE_FALSE(R::ValidateOptions(stray, no_game, error));

    R::Options audit;
    REQUIRE(Parse(audit, R::OptionNvnCensusAudit, nullptr, error));
    REQUIRE_FALSE(R::ValidateOptions(audit, live_unattended, error));

    R::Options times;
    REQUIRE(Parse(times, R::OptionFrameTimes, "t.jsonl", error));
    REQUIRE_FALSE(R::ValidateOptions(times, live_attended, error));
    REQUIRE(R::ValidateOptions(times, live_unattended, error));

    REQUIRE(R::ValidateOptions(R::Options{}, live_attended, error));
}

TEST_CASE("Replay options: active options and help", "[video_core][replay]") {
    R::Options options;
    std::string error;
    REQUIRE(Parse(options, R::OptionFrameTimes, "t.jsonl", error));
    R::SetActiveOptions(options);
    REQUIRE(R::ActiveOptions().frame_times_path == "t.jsonl");
    R::SetActiveOptions(R::Options{});
    REQUIRE_FALSE(R::ActiveOptions().Any());

    const std::string help = R::HelpText();
    for (const R::OptionSpec& spec : R::OptionSpecs()) {
        REQUIRE(help.find(spec.name) != std::string::npos);
    }
}

TEST_CASE("Replay options: private output checks", "[video_core][replay]") {
    namespace fs = std::filesystem;
    std::string error;
    const fs::path root = ScratchDirectory("egl_replay_private_test");

    SECTION("a new file and a new directory are accepted") {
        REQUIRE(R::CheckPrivateOutput(root / "new.egcap", R::OutputKind::File, error));
        REQUIRE(R::CheckPrivateOutput(root / "pictures", R::OutputKind::Directory, error));
    }
    SECTION("an existing file is never overwritten") {
        std::ofstream{root / "old.egcap"} << "x";
        REQUIRE_FALSE(R::CheckPrivateOutput(root / "old.egcap", R::OutputKind::File, error));
    }
    SECTION("a non-empty directory is refused") {
        fs::create_directories(root / "full");
        std::ofstream{root / "full" / "f"} << "x";
        REQUIRE_FALSE(R::CheckPrivateOutput(root / "full", R::OutputKind::Directory, error));
    }
    SECTION("a git worktree and a release staging directory are refused") {
        fs::create_directories(root / "work" / ".git");
        REQUIRE_FALSE(R::CheckPrivateOutput(root / "work" / "a.egcap", R::OutputKind::File, error));
        fs::create_directories(root / "stage");
        std::ofstream{root / "stage" / std::string{R::ReleaseStagingMarkerName}} << "x";
        REQUIRE_FALSE(R::CheckPrivateOutput(root / "stage" / "a.egcap", R::OutputKind::File, error));
    }
    SECTION("marking a directory private is idempotent") {
        REQUIRE(R::MarkDirectoryPrivate(root / "marked", error));
        REQUIRE(fs::exists(root / "marked" / std::string{R::PrivateMarkerName}));
        REQUIRE(R::MarkDirectoryPrivate(root / "marked", error));
    }

    fs::remove_all(root);
}
