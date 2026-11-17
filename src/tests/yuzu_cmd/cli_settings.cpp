// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/settings_common.h"
#include "common/settings_enums.h"
#include "common/settings_setting.h"
#include "yuzu_cmd/cli_settings.h"

namespace {

enum class Flavor : u32 { Plain, Sweet, Sour };

} // Anonymous namespace

namespace Settings {

template <>
inline std::vector<std::pair<std::string_view, Flavor>> EnumMetadata<Flavor>::Canonicalizations() {
    return {{"Plain", Flavor::Plain}, {"Sweet", Flavor::Sweet}, {"Sour", Flavor::Sour}};
}
template <>
inline u32 EnumMetadata<Flavor>::Index() {
    return 0;
}
template <>
inline Flavor EnumMetadata<Flavor>::GetFirst() {
    return Flavor::Plain;
}
template <>
inline Flavor EnumMetadata<Flavor>::GetLast() {
    return Flavor::Sour;
}

} // namespace Settings

namespace {

/// An enum setting that counts every write, so a test can prove that a read-only request never
/// changes the value, not even for a moment.
class CountingSetting final : public Settings::Setting<Flavor, true> {
public:
    explicit CountingSetting(Settings::Linkage& linkage_)
        : Settings::Setting<Flavor, true>{linkage_, Flavor::Sweet, "flavor",
                                          Settings::Category::Core} {}

    void SetValue(const Flavor& next) override {
        ++writes;
        Settings::Setting<Flavor, true>::SetValue(next);
    }

    int writes = 0;
};

} // Anonymous namespace

TEST_CASE("Describing an enum setting does not write it", "[yuzu_cmd][cli_settings]") {
    Settings::Linkage linkage;
    CountingSetting setting{linkage};

    const nlohmann::json described = CliSettings::Describe("flavor", setting);

    REQUIRE(setting.writes == 0);
    REQUIRE(described["type"] == "enum");
    REQUIRE(described["value"] == 1);
    REQUIRE(described["value_name"] == "Sweet");
    REQUIRE(described["default"] == 1);
    REQUIRE(described["default_name"] == "Sweet");
    REQUIRE(described["choices"].size() == 3);
    const char* const names[] = {"Plain", "Sweet", "Sour"};
    for (u32 index = 0; index < 3; ++index) {
        REQUIRE(described["choices"][index]["value"] == index);
        REQUIRE(described["choices"][index]["name"] == names[index]);
    }
    REQUIRE(setting.GetValue() == Flavor::Sweet);
}

TEST_CASE("The choices of an enum setting come from its metadata", "[yuzu_cmd][cli_settings]") {
    Settings::Linkage linkage;
    CountingSetting setting{linkage};

    const std::vector<std::pair<u32, std::string>> choices = setting.EnumChoices();

    REQUIRE(setting.writes == 0);
    REQUIRE(choices.size() == 3);
    REQUIRE(choices[0] == std::pair<u32, std::string>{0, "Plain"});
    REQUIRE(choices[1] == std::pair<u32, std::string>{1, "Sweet"});
    REQUIRE(choices[2] == std::pair<u32, std::string>{2, "Sour"});
}

TEST_CASE("A rejected value for an enum setting writes nothing", "[yuzu_cmd][cli_settings]") {
    Settings::Linkage linkage;
    CountingSetting setting{linkage};
    std::string reason;

    REQUIRE_FALSE(CliSettings::CanonicalText(setting, "Salty", reason).has_value());
    REQUIRE(reason == "expected one of: Plain (0), Sweet (1), Sour (2)");
    REQUIRE_FALSE(CliSettings::CanonicalText(setting, "3", reason).has_value());
    REQUIRE_FALSE(CliSettings::CanonicalText(setting, "", reason).has_value());

    REQUIRE(setting.writes == 0);
    REQUIRE(setting.GetValue() == Flavor::Sweet);
}

TEST_CASE("An accepted enum value is resolved without being written", "[yuzu_cmd][cli_settings]") {
    Settings::Linkage linkage;
    CountingSetting setting{linkage};
    std::string reason;

    REQUIRE(CliSettings::CanonicalText(setting, "sour", reason) == std::optional<std::string>{"2"});
    REQUIRE(CliSettings::CanonicalText(setting, "0", reason) == std::optional<std::string>{"0"});

    REQUIRE(setting.writes == 0);
    REQUIRE(setting.GetValue() == Flavor::Sweet);
}
