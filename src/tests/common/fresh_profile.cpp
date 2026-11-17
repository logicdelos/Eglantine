// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// First run on a fresh profile: a user directory that holds only the keys and the system folders.
// Nothing that the emulator writes on such a run may fail because a folder is missing.

#include <chrono>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "frontend_common/config.h"
#include "frontend_common/play_time_manager.h"

namespace {

/// A scratch directory under the system temp directory, removed at the end of the test.
class ScratchDirectory {
public:
    explicit ScratchDirectory(const char* name)
        : path{std::filesystem::temp_directory_path() /
               (std::string(name) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
    }
    ~ScratchDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    const std::filesystem::path path;
};

/// Points one user path at a directory for the length of a test and puts the old one back. The
/// path manager accepts only an existing directory, so an old path that does not exist on this
/// machine is not restored; nothing else in the test binary reads the paths changed here.
class UserPathOverride {
public:
    UserPathOverride(Common::FS::EdenPath which_, const std::filesystem::path& directory)
        : which{which_}, previous{Common::FS::GetEdenPath(which_)} {
        Common::FS::SetEdenPath(which, directory);
    }
    ~UserPathOverride() {
        Common::FS::SetEdenPath(which, previous);
    }

    UserPathOverride(const UserPathOverride&) = delete;
    UserPathOverride& operator=(const UserPathOverride&) = delete;

private:
    const Common::FS::EdenPath which;
    const std::filesystem::path previous;
};

constexpr u64 TestProgramId = 0x0100000000010000ULL;

} // namespace

TEST_CASE("Play time is saved when its folder does not exist yet", "[fresh_profile]") {
    const ScratchDirectory scratch{"fresh-profile-play-time"};
    const auto play_time_dir = scratch.path / "play_time";
    std::filesystem::create_directories(play_time_dir);
    const UserPathOverride override_path{Common::FS::EdenPath::PlayTimeDir, play_time_dir};
    // The path manager only accepts an existing directory; remove it again, as on a fresh profile.
    std::filesystem::remove_all(play_time_dir);
    REQUIRE_FALSE(std::filesystem::exists(play_time_dir));

    {
        PlayTime::PlayTimeManager manager;
        manager.SetPlayTime(TestProgramId, 42);
    } // Saved on destruction.

    CHECK(std::filesystem::is_regular_file(play_time_dir / "playtime.bin"));
    const PlayTime::PlayTimeManager reloaded;
    CHECK(reloaded.GetPlayTime(TestProgramId) == 42);
}

TEST_CASE("An empty configured user directory keeps the default path", "[fresh_profile]") {
    const ScratchDirectory scratch{"fresh-profile-paths"};
    const auto default_dir = scratch.path / "tas";
    const auto configured_dir = scratch.path / "configured";
    std::filesystem::create_directories(default_dir);
    std::filesystem::create_directories(configured_dir);
    // The TAS folder: no other test in this binary reads it.
    const UserPathOverride override_path{Common::FS::EdenPath::TASDir, default_dir};
    REQUIRE(Common::FS::GetEdenPath(Common::FS::EdenPath::TASDir) == default_dir);

    // A fresh profile has no configuration file: every stored path reads back empty.
    CHECK_FALSE(ApplyConfiguredEdenPath(Common::FS::EdenPath::TASDir, ""));
    CHECK(Common::FS::GetEdenPath(Common::FS::EdenPath::TASDir) == default_dir);

    // A stored directory still replaces the default.
    CHECK(ApplyConfiguredEdenPath(Common::FS::EdenPath::TASDir,
                                  Common::FS::PathToUTF8String(configured_dir)));
    CHECK(Common::FS::GetEdenPath(Common::FS::EdenPath::TASDir) == configured_dir);
}
