// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <getopt.h>

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/launch_params.h"
#include "tests/core/system_test_context.h"

namespace {

/// What a command line that ends without the operand of an option must say, per option.
struct MissingOperand {
    const char* option;
    const char* message;
};

constexpr MissingOperand MissingOperands[] = {
    {"--control", "--control needs a name of 1 to 64 letters, digits, '.', '_' or '-', starting "
                  "with a letter or digit"},
    {"--set", "--set expects name=value, with a name and an equals sign"},
    {"--user-dir", "--user-dir needs a directory"},
};

/// Parses `args` as the command line of the program. getopt keeps its position in global state,
/// so it is started over for every call.
Core::LaunchParams Parse(Core::System& system, std::vector<std::string> args) {
    args.insert(args.begin(), "eden-cli");
    std::vector<char*> argv;
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
#ifdef optreset
    optreset = 1;
    optind = 1;
#else
    optind = 0;
#endif
    return Core::ParseLaunchParams(system, static_cast<int>(args.size()), argv.data(), nullptr);
}

} // Anonymous namespace

TEST_CASE("A missing operand of a new option is an argument error", "[core][launch_params]") {
    ScopedSystemTestContext context;
    Core::System system;

    for (const MissingOperand& missing : MissingOperands) {
        SECTION(std::string{missing.option} + " alone") {
            const Core::LaunchParams lp = Parse(system, {missing.option});
            REQUIRE(lp.parse_error == missing.message);
            REQUIRE(lp.unknown_option.empty());
        }
        SECTION(std::string{missing.option} + " after a game") {
            const Core::LaunchParams lp = Parse(system, {"-g", "game.nsp", missing.option});
            REQUIRE(lp.parse_error == missing.message);
            REQUIRE(lp.unknown_option.empty());
            REQUIRE(lp.filepath == "game.nsp");
        }
        SECTION(std::string{missing.option} + " after other options") {
            const Core::LaunchParams lp =
                Parse(system, {"-g", "game.nsp", "--fullscreen", "--mcp", missing.option});
            REQUIRE(lp.parse_error == missing.message);
            REQUIRE(lp.unknown_option.empty());
        }
    }
}

TEST_CASE("A missing operand of a new option sets nothing", "[core][launch_params]") {
    ScopedSystemTestContext context;
    Core::System system;

    REQUIRE_FALSE(Parse(system, {"-g", "game.nsp", "--control"}).control_name.has_value());
    REQUIRE(Parse(system, {"-g", "game.nsp", "--set"}).setting_overrides.empty());
    REQUIRE_FALSE(Parse(system, {"-g", "game.nsp", "--user-dir"}).user_dir.has_value());
}

TEST_CASE("The new options with an operand are accepted", "[core][launch_params]") {
    ScopedSystemTestContext context;
    Core::System system;

    const Core::LaunchParams lp = Parse(system, {"-g", "game.nsp", "--control", "ctl.1", "--set",
                                                 "fps=60", "--user-dir", "profile"});
    REQUIRE(lp.parse_error.empty());
    REQUIRE(lp.unknown_option.empty());
    REQUIRE(lp.control_name == "ctl.1");
    REQUIRE(lp.setting_overrides.size() == 1);
    REQUIRE(lp.setting_overrides[0].first == "fps");
    REQUIRE(lp.setting_overrides[0].second == "60");
    REQUIRE(lp.user_dir == "profile");
}

TEST_CASE("Old options keep the unknown option policy", "[core][launch_params]") {
    ScopedSystemTestContext context;
    Core::System system;

    // An unknown option, and an old option without its operand, are recorded but are not an
    // argument error; only an unattended run refuses them.
    const Core::LaunchParams unknown = Parse(system, {"-g", "game.nsp", "--no-such-option"});
    REQUIRE(unknown.parse_error.empty());
    REQUIRE_FALSE(unknown.unknown_option.empty());

    const Core::LaunchParams config = Parse(system, {"-g", "game.nsp", "--config"});
    REQUIRE(config.parse_error.empty());
    REQUIRE_FALSE(config.unknown_option.empty());
}

TEST_CASE("--baseline refuses a --set of a baseline setting unless it is declared",
          "[core][launch_params]") {
    ScopedSystemTestContext context;
    Core::System system;
    const std::string refused =
        "--baseline refused: --set resolution_setup changes a baseline setting, which takes the "
        "run off the baseline. Drop --baseline, or declare it with --baseline-allow "
        "resolution_setup";

    SECTION("refused when not declared, before or after --baseline") {
        REQUIRE(Parse(system, {"-g", "game.nsp", "--baseline", "--set", "resolution_setup=3"})
                    .parse_error == refused);
        REQUIRE(Parse(system, {"--set", "resolution_setup=3", "-g", "game.nsp", "--baseline"})
                    .parse_error == refused);
    }
    SECTION("allowed when declared") {
        const Core::LaunchParams lp =
            Parse(system, {"-g", "game.nsp", "--set", "resolution_setup=3", "--baseline",
                           "--baseline-allow", "resolution_setup"});
        REQUIRE(lp.parse_error.empty());
        REQUIRE(lp.setting_overrides.size() == 1);
        REQUIRE(lp.setting_overrides[0].first == "resolution_setup");
        REQUIRE(lp.baseline_allow == std::vector<std::string>{"resolution_setup"});
    }
    SECTION("a setting outside the baseline, or no --baseline, is not refused") {
        REQUIRE(Parse(system, {"-g", "game.nsp", "--baseline", "--set", "log_filter=*:Debug"})
                    .parse_error.empty());
        REQUIRE(Parse(system, {"-g", "game.nsp", "--set", "resolution_setup=3"})
                    .parse_error.empty());
    }
    SECTION("the same setting twice is a bad argument, declared or not") {
        REQUIRE(Parse(system, {"-g", "game.nsp", "--baseline", "--baseline-allow",
                               "resolution_setup", "--set", "resolution_setup=3", "--set",
                               "resolution_setup=2"})
                    .parse_error == "--set resolution_setup was given more than once");
        REQUIRE(Parse(system, {"-g", "game.nsp", "--baseline", "--set", "resolution_setup=3",
                               "--set", "resolution_setup=2"})
                    .parse_error == "--set resolution_setup was given more than once");
    }
}
