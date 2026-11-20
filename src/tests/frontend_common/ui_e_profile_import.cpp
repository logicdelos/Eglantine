// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <catch2/catch_test_macros.hpp>
#ifndef SI_NO_CONVERSION
#define SI_NO_CONVERSION 1
#endif
#include <SimpleIni.h>
#include "common/fs/path_util.h"
#include "common/fs/profile_migration.h"
#include "frontend_common/profile_import.h"

namespace fs = std::filesystem;
namespace Migration = Common::FS::ProfileMigration;
using namespace FrontendCommon;

namespace {
struct Fixture {
    fs::path root = fs::temp_directory_path() /
                    ("eglantine-import-config-" + std::to_string(std::random_device{}()));
    Fixture() {
        fs::create_directories(root);
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};
} // namespace

TEST_CASE("Imported paths are remapped by path component", "[migration]") {
    const auto old_root = fs::absolute("eden");
    const auto new_root = fs::absolute("eglantine");
    CHECK(RemapImportedPath(old_root / "custom-nand", old_root, new_root) ==
          new_root / "custom-nand");
    CHECK(RemapImportedPath(old_root, old_root, new_root) == new_root);
    const auto external = old_root.parent_path() / "eden-other" / "save";
    CHECK(RemapImportedPath(external, old_root, new_root) == external);
    const auto escape = old_root / ".." / "external";
    CHECK(RemapImportedPath(escape, old_root, new_root) == escape);
    CHECK(RemapImportedPath({}, old_root, new_root).empty());
    CHECK(RemapImportedPath("relative", old_root, new_root) == "relative");
}

TEST_CASE("Move remaps every raw imported storage path before the source disappears",
          "[migration]") {
    Fixture fixture;
    const auto source = fixture.root / "eden";
    const auto destination = fixture.root / "eglantine";
    const auto config = source / "config";
    fs::create_directories(config / "custom");
    constexpr std::array keys{"nand_directory", "save_directory", "sdmc_directory",
                              "load_directory", "tas_directory",  "dump_directory"};
    for (const auto name : {"qt-config.ini", "sdl2-config.ini", "custom/game.ini"}) {
        std::ofstream file{config / name};
        file << "[Data%20Storage]\n";
        for (const auto* key : keys) {
            fs::create_directories(source / key);
            file << key << "=\"" << (source / key).generic_string() << "\"\n";
        }
        fs::create_directories(source / "custom-screenshots");
        file << "[Screenshots]\nscreenshot_path=\""
             << (source / "custom-screenshots").generic_string() << "\"\n";
    }
    const auto state = fixture.root / "eglantine.migration";
    Migration::RunImport(
        {{{source, destination}}, Migration::Strategy::Move, state, {}}, [&](const auto& staged) {
            RemapImportedConfig(staged.at(0).destination / "config", source, destination);
        });
    CHECK_FALSE(fs::exists(source));
    for (const auto name : {"qt-config.ini", "sdl2-config.ini", "custom/game.ini"}) {
        std::ifstream file{destination / "config" / name};
        const std::string contents{std::istreambuf_iterator<char>{file}, {}};
        CSimpleIniA ini;
        REQUIRE(ini.LoadData(contents) >= 0);
        for (const auto* key : keys) {
            const fs::path selected{ini.GetValue("Data%20Storage", key, "")};
            CHECK(selected == destination / key);
            CHECK(fs::is_directory(selected));
        }
        const fs::path screenshot{ini.GetValue("Screenshots", "screenshot_path", "")};
        CHECK(screenshot == destination / "custom-screenshots");
        CHECK(fs::is_directory(screenshot));
    }
    CHECK(Migration::GetState(state) == Migration::State::Complete);
}
