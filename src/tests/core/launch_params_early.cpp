// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The scan of the options that act before the log and Core::System exist (--version --json and
// --user-dir) has to read the command line the way getopt does: an argument an option takes is
// never read as an option, whatever it looks like.

#include <initializer_list>
#include <string>
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
