// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Loads real config files through the shared config loader and checks what ends up in the
// settings: a new install and an old config both land on the baseline, and a stored choice is
// kept and reported as a deviation.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/baseline_settings.h"
#include "common/settings.h"
#include "frontend_common/config.h"
#include "common/fs/path_util.h"

namespace {

/// The shared loader without a front end: nothing to read or write beyond the generic values.
class TestConfig final : public Config {
public:
    explicit TestConfig(const std::string& path) : Config(ConfigType::GlobalConfig) {
        Initialize(std::optional<std::string>{path});
    }

    void ReloadAllValues() override {
        Reload();
    }
    void SaveAllValues() override {
        SaveValues();
    }

protected:
    void ReadHidbusValues() override {}
    void ReadDebugControlValues() override {}
    void ReadPathValues() override {}
    void ReadShortcutValues() override {}
    void ReadUIValues() override {}
    void ReadUIGamelistValues() override {}
    void ReadUILayoutValues() override {}
    void ReadMultiplayerValues() override {}

    void SaveHidbusValues() override {}
    void SaveDebugControlValues() override {}
    void SavePathValues() override {}
    void SaveShortcutValues() override {}
    void SaveUIValues() override {}
    void SaveUIGamelistValues() override {}
    void SaveUILayoutValues() override {}
    void SaveMultiplayerValues() override {}

    std::vector<Settings::BasicSetting*>& FindRelevantList(Settings::Category category) override {
        return Settings::values.linkage.by_category[category];
    }
};

/// A scratch directory under the system temp directory, removed at the end of the test.
class ScratchDirectory {
public:
    explicit ScratchDirectory(const char* name)
        : path{std::filesystem::temp_directory_path() / name} {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~ScratchDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    [[nodiscard]] std::filesystem::path File(const char* name) const {
        return path / name;
    }

private:
    std::filesystem::path path;
};

void WriteFile(const std::filesystem::path& file, const std::string& text) {
    std::ofstream stream{file, std::ios::binary | std::ios::trunc};
    stream << text;
}

/// "[Section]\nkey\default=<flag>\nkey=<value>\n" for one setting, in the layout the loader reads.
std::string Stored(const Settings::BasicSetting& setting, bool is_default,
                   const std::string& value) {
    return std::string{"["} + Settings::TranslateCategory(setting.GetCategory()) + "]\n" +
           std::string{setting.GetLabel()} + "\\default=" + (is_default ? "true" : "false") +
           "\n" + std::string{setting.GetLabel()} + "=" + value + "\n";
}

const std::vector<Settings::Baseline::EnvironmentSwitch> kNoSwitches{};

} // namespace

TEST_CASE("A new install starts on the baseline", "[baseline_config]") {
    const ScratchDirectory scratch{"eglantine_baseline_config_new"};
    Settings::Baseline::ResetToBaseline();
    Settings::Baseline::ClearNotedSources();

    // Whatever a previous session left in memory, a config that does not exist loads the defaults.
    Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
    Settings::values.anti_aliasing.SetValue(Settings::AntiAliasing::None);
    Settings::values.scaling_filter.SetValue(Settings::ScalingFilter::Bilinear);

    {
        const TestConfig config{Common::FS::PathToUTF8String(scratch.File("new-config.ini"))};
        CHECK(Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Docked);
        CHECK(Settings::values.anti_aliasing.GetValue() == Settings::AntiAliasing::Smaa);
        CHECK(Settings::values.scaling_filter.GetValue() == Settings::ScalingFilter::Buf);
    }

    const auto deviations = Settings::Baseline::Deviations(Settings::values, kNoSwitches);
    CHECK(deviations.empty());
    CHECK(Settings::Baseline::OnBaseline(deviations, {}));
    Settings::Baseline::ResetToBaseline();
}

TEST_CASE("A config that kept the old defaults follows the new ones", "[baseline_config]") {
    const ScratchDirectory scratch{"eglantine_baseline_config_old"};
    Settings::Baseline::ResetToBaseline();
    Settings::Baseline::ClearNotedSources();

    // What an earlier version wrote: the values of its defaults, flagged as defaults.
    WriteFile(scratch.File("old-config.ini"),
              Stored(Settings::values.use_docked_mode, true, "0") +
                  Stored(Settings::values.anti_aliasing, true, "1") +
                  Stored(Settings::values.scaling_filter, true, "1"));

    {
        const TestConfig config{Common::FS::PathToUTF8String(scratch.File("old-config.ini"))};
        CHECK(Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Docked);
        CHECK(Settings::values.anti_aliasing.GetValue() == Settings::AntiAliasing::Smaa);
        CHECK(Settings::values.scaling_filter.GetValue() == Settings::ScalingFilter::Buf);
    }

    CHECK(Settings::Baseline::Deviations(Settings::values, kNoSwitches).empty());
    Settings::Baseline::ResetToBaseline();
}

TEST_CASE("A stored choice is kept and reported", "[baseline_config]") {
    const ScratchDirectory scratch{"eglantine_baseline_config_choice"};
    Settings::Baseline::ResetToBaseline();
    Settings::Baseline::ClearNotedSources();

    WriteFile(scratch.File("choice-config.ini"),
              Stored(Settings::values.use_docked_mode, false, "0") +
                  Stored(Settings::values.anti_aliasing, true, "1"));

    {
        const TestConfig config{Common::FS::PathToUTF8String(scratch.File("choice-config.ini"))};
        CHECK(Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Handheld);
        CHECK(Settings::values.anti_aliasing.GetValue() == Settings::AntiAliasing::Smaa);
    }

    const auto deviations = Settings::Baseline::Deviations(Settings::values, kNoSwitches);
    REQUIRE(deviations.size() == 1);
    CHECK(deviations[0].key == "use_docked_mode");
    CHECK(deviations[0].baseline == "1");
    CHECK(deviations[0].effective == "0");
    CHECK(deviations[0].source == "global config");
    CHECK_FALSE(Settings::Baseline::OnBaseline(deviations, {}));
    CHECK(Settings::Baseline::OnBaseline(deviations, {"use_docked_mode"}));
    Settings::Baseline::ResetToBaseline();
}

TEST_CASE("Debug settings that change a run are in the baseline and are reset",
          "[baseline_config]") {
    const ScratchDirectory scratch{"eglantine_baseline_config_debug"};
    Settings::Baseline::ResetToBaseline();
    Settings::Baseline::ClearNotedSources();

    const std::vector<std::string> keys{
        "debug_knobs",          "disable_macro_hle", "disable_macro_jit",
        "disable_web_applet",   "enable_all_controllers", "perform_vulkan_check",
        "quest_flag",           "use_auto_stub",     "use_dev_keys",
    };
    // Settings of the same categories that only write logs, open servers or hold secrets.
    const std::vector<std::string> left_out{"use_gdbstub", "mcp_token", "dump_nso", "extended_logging",
                                            "gpu_log_level", "serial_unit"};

    const auto entries = Settings::Baseline::Entries();
    const auto listed = [&entries](const std::string& key) {
        return std::any_of(entries.begin(), entries.end(),
                           [&key](const Settings::Baseline::Entry& entry) {
                               return entry.key == key;
                           });
    };
    for (const auto& key : keys) {
        CHECK(listed(key));
    }
    for (const auto& key : left_out) {
        CHECK_FALSE(listed(key));
    }

    // A stored value that is not the default, marked as a choice (default=false).
    std::string text;
    for (const auto& key : keys) {
        const auto found = Settings::values.linkage.by_key.find(key);
        REQUIRE(found != Settings::values.linkage.by_key.end());
        const std::string standard = found->second->DefaultToString();
        text += Stored(*found->second, false,
                       standard == "true" ? "false" : (standard == "false" ? "true" : "1"));
    }
    WriteFile(scratch.File("debug-config.ini"), text);

    {
        const TestConfig config{Common::FS::PathToUTF8String(scratch.File("debug-config.ini"))};
        const auto deviations = Settings::Baseline::Deviations(Settings::values, kNoSwitches);
        CHECK(deviations.size() == keys.size());
        for (const auto& deviation : deviations) {
            CHECK(deviation.source == "global config");
        }
        CHECK_FALSE(Settings::Baseline::OnBaseline(deviations, {}));

        // The reset that --baseline does after the config loads covers each of them.
        Settings::Baseline::ResetToBaseline();
        const auto after = Settings::Baseline::Deviations(Settings::values, kNoSwitches);
        CHECK(after.empty());
        CHECK(Settings::Baseline::OnBaseline(after, {}));
    }
    Settings::Baseline::ResetToBaseline();
}
