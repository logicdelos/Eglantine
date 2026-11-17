// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The scan of the options that act before the log and Core::System exist (--version --json and
// --user-dir) has to read the command line the way getopt does: an argument an option takes is
// never read as an option, whatever it looks like.

#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/launch_params.h"

namespace {

Core::EarlyOptions Scan(std::initializer_list<const char*> arguments) {
    std::vector<std::string> storage{"program"};
    for (const char* argument : arguments) {
        storage.emplace_back(argument);
    }
    std::vector<char*> argv;
    for (auto& text : storage) {
        argv.push_back(text.data());
    }
    return Core::ScanEarlyOptions(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST_CASE("--version --json is found, in either order and abbreviated", "[launch_params]") {
    CHECK(Scan({"--version", "--json"}).version_json);
    CHECK(Scan({"--json", "--version"}).version_json);
    CHECK(Scan({"-v", "--json"}).version_json);
    CHECK(Scan({"--vers", "--js"}).version_json);
    CHECK_FALSE(Scan({"--version"}).version_json);
    CHECK_FALSE(Scan({"--json"}).version_json);
    CHECK(Scan({"--version", "--json"}).error.empty());
}

TEST_CASE("The argument of an option is not read as an option", "[launch_params]") {
    // --game and --report take the next word whatever it is.
    CHECK_FALSE(Scan({"--game", "--version", "--json"}).version_json);
    CHECK_FALSE(Scan({"--report", "--json", "--version"}).version_json);
    CHECK_FALSE(Scan({"-g", "--version", "--json"}).version_json);
    CHECK_FALSE(Scan({"--baseline-allow", "--version", "--json"}).version_json);
    CHECK(Scan({"--game", "title.nsp", "--version", "--json"}).version_json);
    CHECK(Scan({"-gtitle.nsp", "--version", "--json"}).version_json);
    CHECK(Scan({"--game=--version", "--version", "--json"}).version_json);
    // --program and --filter take the next word themselves.
    CHECK_FALSE(Scan({"--program", "--version", "--json"}).version_json);
    CHECK_FALSE(Scan({"--filter", "--version", "--json"}).version_json);
    CHECK(Scan({"--filter", "*:Info", "--version", "--json"}).version_json);
    CHECK(Scan({"-x", "*:Info", "--version", "--json"}).version_json);
    // A value on an option that takes none: the option is not parsed.
    CHECK_FALSE(Scan({"--version=1", "--json"}).version_json);
    // Nothing after "--" is an option.
    CHECK_FALSE(Scan({"--", "--version", "--json"}).version_json);
}

TEST_CASE("--user-dir is read with its value, and its mistakes are errors", "[launch_params]") {
    const auto spaced = Scan({"--user-dir", "one"});
    REQUIRE(spaced.user_dir.has_value());
    CHECK(*spaced.user_dir == "one");
    CHECK(spaced.error.empty());

    const auto equals = Scan({"--user-dir=two"});
    REQUIRE(equals.user_dir.has_value());
    CHECK(*equals.user_dir == "two");

    const auto abbreviated = Scan({"--user-d", "three"});
    REQUIRE(abbreviated.user_dir.has_value());
    CHECK(*abbreviated.user_dir == "three");

    // The value is whatever follows, even a word that looks like an option.
    const auto looks_like_option = Scan({"--user-dir", "--version", "--json"});
    REQUIRE(looks_like_option.user_dir.has_value());
    CHECK(*looks_like_option.user_dir == "--version");
    CHECK_FALSE(looks_like_option.version_json);

    CHECK_FALSE(Scan({"--user-dir"}).error.empty());
    CHECK_FALSE(Scan({"--user-dir="}).error.empty());
    CHECK_FALSE(Scan({"--user-dir", "a", "--user-dir=b"}).error.empty());
    CHECK_FALSE(Scan({"--user-dir=a", "--user-dir", "b"}).error.empty());
}

TEST_CASE("The errors the scan sees come with the version request", "[launch_params]") {
    // The caller prints the build identity only when error is empty, so a bad or repeated
    // --user-dir next to --version --json is an error and not a success.
    const auto missing = Scan({"--version", "--json", "--user-dir"});
    CHECK(missing.version_json);
    CHECK_FALSE(missing.error.empty());
    const auto repeated = Scan({"--user-dir", "a", "--version", "--json", "--user-dir", "b"});
    CHECK(repeated.version_json);
    CHECK_FALSE(repeated.error.empty());
    CHECK(Scan({"--version", "--json", "--user-dir", "a"}).error.empty());
}

TEST_CASE("A fork option without its argument is a mistake with or without --report",
          "[launch_params]") {
    // What getopt rejected is reported for these words whatever else is on the command line.
    for (const char* word : {"--baseline-allow", "--user-dir", "--report", "--run-seconds",
                             "--run-frames", "--run-f", "--base"}) {
        const std::string message = Core::MisusedBranchOption(word);
        INFO(word);
        if (std::string{word} == "--base") {
            // An abbreviation shared by --baseline, --baseline-allow: ambiguous kinds, no message.
            CHECK(message.empty());
        } else {
            CHECK(message.find("needs an argument") != std::string::npos);
        }
    }
    // A value on a fork option that takes none.
    CHECK_FALSE(Core::MisusedBranchOption("--baseline=1").empty());
    CHECK_FALSE(Core::MisusedBranchOption("--print-baseline=x").empty());
    // Everything else keeps its older handling: older options, unknown words, plain values.
    CHECK(Core::MisusedBranchOption("--game").empty());
    CHECK(Core::MisusedBranchOption("--no-such-option").empty());
    CHECK(Core::MisusedBranchOption("--baseline").empty());
    CHECK(Core::MisusedBranchOption("--report=file.json").empty());
    CHECK(Core::MisusedBranchOption("-g").empty());
    CHECK(Core::MisusedBranchOption("").empty());
}

TEST_CASE("The early scan records the mistakes with the fork options", "[launch_params]") {
    // The caller prints the build identity only when error is empty, so each of these ends in
    // exit code 64 and not in a version line.
    const auto missing = Scan({"--version", "--json", "--baseline-allow"});
    CHECK(missing.version_json);
    CHECK(missing.error == "--baseline-allow needs an argument");
    const auto attached = Scan({"--version", "--json", "--baseline=1"});
    CHECK(attached.version_json);
    CHECK(attached.error == "--baseline does not take an argument");
    CHECK(Scan({"--version", "--json", "--print-baseline=x"}).error ==
          "--print-baseline does not take an argument");
    CHECK(Scan({"--version", "--json", "--json=x"}).error == "--json does not take an argument");
    CHECK(Scan({"--version", "--json", "--report"}).error == "--report needs an argument");
    CHECK(Scan({"--version", "--json", "--run-seconds"}).error == "--run-seconds needs an argument");
    CHECK(Scan({"--version", "--json", "--run-frames"}).error == "--run-frames needs an argument");
    CHECK(Scan({"--version", "--json", "--run-f"}).error == "--run-frames needs an argument");
    // The first mistake is the one that is kept.
    CHECK(Scan({"--baseline=1", "--report"}).error == "--baseline does not take an argument");

    // An argument that an option takes is not a missing one, and an option is not misused when
    // it is spelled right.
    CHECK(Scan({"--version", "--json", "--baseline-allow", "key=1"}).error.empty());
    CHECK(Scan({"--version", "--json", "--baseline-allow=key=1"}).error.empty());
    CHECK(Scan({"--version", "--json", "--report", "out.json"}).error.empty());
    CHECK(Scan({"--version", "--json", "--baseline"}).error.empty());
    CHECK(Scan({"--version", "--json", "--print-baseline"}).error.empty());
    // The word after an option that takes an argument is that argument, whatever it looks like.
    CHECK(Scan({"--report", "--baseline=1", "--version", "--json"}).error.empty());
    // After "--" nothing is an option.
    CHECK(Scan({"--", "--baseline=1"}).error.empty());
    // Older options keep their handling: a value on --version is not a mistake of this scan.
    CHECK(Scan({"--version=1", "--json"}).error.empty());
}

TEST_CASE("Every prefix of every long option resolves as getopt resolves it", "[launch_params]") {
    // The table is the one getopt_long gets. Computed here from the table alone: an exact name
    // wins; otherwise the options the prefix starts are one option, or several of the same kind
    // (argument and id), or the prefix is ambiguous.
    const std::vector<Core::LongOptionEntry> table = Core::LongOptionTable();
    REQUIRE_FALSE(table.empty());
    for (const Core::LongOptionEntry& option : table) {
        for (std::size_t length = 1; length <= option.name.size(); ++length) {
            const std::string_view prefix = option.name.substr(0, length);
            std::string_view expected;
            const Core::LongOptionEntry* first = nullptr;
            bool ambiguous = false;
            bool exact = false;
            for (const Core::LongOptionEntry& candidate : table) {
                if (candidate.name == prefix) {
                    expected = candidate.name;
                    exact = true;
                    break;
                }
                if (candidate.name.starts_with(prefix)) {
                    if (first == nullptr) {
                        first = &candidate;
                    } else if (first->takes_argument != candidate.takes_argument ||
                               first->requires_argument != candidate.requires_argument ||
                               first->id != candidate.id) {
                        ambiguous = true;
                    }
                }
            }
            if (!exact) {
                expected = (ambiguous || first == nullptr) ? std::string_view{} : first->name;
            }
            INFO("--" << std::string{prefix});
            CHECK(Core::ResolveLongOption(prefix) == expected);
        }
    }
}

TEST_CASE("No two long options share an id", "[launch_params]") {
    std::set<int> ids;
    for (const Core::LongOptionEntry& option : Core::LongOptionTable()) {
        INFO("--" << std::string{option.name});
        CHECK(ids.insert(option.id).second);
    }
    // The options of 0.3, fork-cli3 and fork-baseline are all in the one table.
    std::set<std::string_view> names;
    for (const Core::LongOptionEntry& option : Core::LongOptionTable()) {
        names.insert(option.name);
    }
    for (const char* name : {"report", "run-seconds", "run-frames", "json", "user-dir",
                             "list-settings", "set", "control", "baseline", "baseline-allow",
                             "print-baseline"}) {
        INFO(name);
        CHECK(names.contains(name));
    }
}

TEST_CASE("--baseline refuses a --set of a baseline setting that is not declared",
          "[launch_params]") {
    const std::string refused =
        "--baseline refused: --set resolution_setup changes a baseline setting, which takes the "
        "run off the baseline. Drop --baseline, or declare it with --baseline-allow "
        "resolution_setup";
    CHECK(Scan({"--baseline", "--set", "resolution_setup=3"}).error == refused);
    // The order on the command line does not matter.
    CHECK(Scan({"--set", "resolution_setup=3", "--baseline"}).error == refused);
    CHECK(Scan({"--set=resolution_setup=3", "-g", "game.nsp", "--baseline"}).error == refused);
    // Given twice, it is named once.
    CHECK(Scan({"--baseline", "--set", "resolution_setup=3", "--set", "resolution_setup=2"})
              .error == refused);
    // --version --json is refused too, so the build identity is not printed.
    const auto version = Scan({"--version", "--json", "--baseline", "--set", "resolution_setup=3"});
    CHECK(version.version_json);
    CHECK(version.error == refused);

    // Declared, in either form the baseline accepts, before or after the --set.
    CHECK(Scan({"--baseline", "--baseline-allow", "resolution_setup", "--set", "resolution_setup=3"})
              .error.empty());
    CHECK(Scan({"--set", "resolution_setup=3", "--baseline-allow=use_docked_mode,resolution_setup=2",
                "--baseline"})
              .error.empty());
    // A setting outside the baseline, and --set without --baseline, are not refused.
    CHECK(Scan({"--baseline", "--set", "log_filter=*:Debug"}).error.empty());
    CHECK(Scan({"--set", "resolution_setup=3"}).error.empty());
    CHECK(Scan({"--version", "--json", "--set", "resolution_setup=3"}).error.empty());
    // The value is never read as an option, and the name of a malformed request is not guessed.
    CHECK(Scan({"--baseline", "--set", "=3"}).error.empty());
}

TEST_CASE("A trailing --set or --control says what the full parse says", "[launch_params]") {
    CHECK(Scan({"--version", "--json", "--set"}).error ==
          "--set expects name=value, with a name and an equals sign");
    CHECK(Scan({"--version", "--json", "--control"}).error ==
          "--control needs a name of 1 to 64 letters, digits, '.', '_' or '-', starting with a "
          "letter or digit");
}
