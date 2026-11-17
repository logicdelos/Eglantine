// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <regex>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <utility>
#include <string_view>

#include "common/assert.h"
#include "common/baseline_settings.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/scm_rev.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/launch_params.h"

#undef _UNICODE
#include <getopt.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif

namespace Core {

/// Parses a whole decimal TCP port (1-65535); nullopt for anything else.
static std::optional<u16> ParsePort(const char* text) {
    char* end = nullptr;
    const unsigned long port = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || port == 0 || port > 65535) {
        return std::nullopt;
    }
    return static_cast<u16>(port);
}

/// Parses a whole positive, finite number of seconds; nullopt for anything else.
static std::optional<double> ParseSeconds(const char* text) {
    if (text[0] == '\0' || std::isspace(static_cast<unsigned char>(text[0])) != 0) {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    const double seconds = std::strtod(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE || !std::isfinite(seconds) ||
        seconds <= 0.0) {
        return std::nullopt;
    }
    return seconds;
}

/// Parses a whole positive decimal count that fits in 64 bits; nullopt for anything else.
static std::optional<u64> ParseFrames(const char* text) {
    if (text[0] == '\0') {
        return std::nullopt;
    }
    for (const char* c = text; *c != '\0'; ++c) {
        if (std::isdigit(static_cast<unsigned char>(*c)) == 0) {
            return std::nullopt;
        }
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long frames = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || frames == 0) {
        return std::nullopt;
    }
    return static_cast<u64>(frames);
}

/// The options of the command line, shared by ParseLaunchParams and ScanEarlyOptions so that both
/// see the same operand boundaries.
static constexpr char kShortOptions[] = "d::c:fhg:m:p:u:vi:nsx";
static struct option kLongOptions[] = {
    // clang-format off
    {"debug", optional_argument, 0, 'd'},
    {"config", required_argument, 0, 'c'},
    {"fullscreen", no_argument, 0, 'f'},
    {"help", no_argument, 0, 'h'},
    {"game", required_argument, 0, 'g'},
    {"multiplayer", required_argument, 0, 'm'},
    {"program", optional_argument, 0, 'p'},
    {"user", required_argument, 0, 'u'},
    {"version", no_argument, 0, 'v'},
    {"input-profile", required_argument, 0, 'i'},
    {"null-render", no_argument, 0, 'n'},
    {"singlecore", no_argument, 0, 's'},
    {"filter", no_argument, 0, 'x'},
    {"control", required_argument, 0, 0x120},
    {"mcp", no_argument, 0, 'M'},
    {"mcp-port", required_argument, 0, 'P'},
    {"mcp-allow-writes", no_argument, 0, 'W'},
    {"pack-nup", required_argument, 0, 'N'},
    {"unpack-nup", required_argument, 0, 'U'},
    {"edit-nup", required_argument, 0, 'E'},
    {"nup-name", required_argument, 0, 0x100},
    {"nup-icon", required_argument, 0, 0x101},
    {"nup-version", required_argument, 0, 0x102},
    {"nup-publisher", required_argument, 0, 0x103},
    {"clear-nup-name", no_argument, 0, 0x104},
    {"clear-nup-icon", no_argument, 0, 0x105},
    {"clear-nup-version", no_argument, 0, 0x106},
    {"clear-nup-publisher", no_argument, 0, 0x107},
    {"report", required_argument, 0, 0x108},
    {"run-seconds", required_argument, 0, 0x109},
    {"run-frames", required_argument, 0, 0x10A},
    {"json", no_argument, 0, 0x10B},
    {"user-dir", required_argument, 0, 0x10C},
    {"list-settings", no_argument, 0, 0x10D},
    {"set", required_argument, 0, 0x10E},
    {"baseline", no_argument, 0, 0x110},
    {"baseline-allow", required_argument, 0, 0x111},
    {"print-baseline", no_argument, 0, 0x112},
    {0, 0, 0, 0},
    // clang-format on
};

/// What --set and --control say when their operand is missing or not usable, whichever of
/// ScanEarlyOptions and ParseLaunchParams sees it first.
static constexpr std::string_view kSetOperandMessage =
    "--set expects name=value, with a name and an equals sign";
static constexpr std::string_view kControlOperandMessage =
    "--control needs a name of 1 to 64 letters, digits, '.', '_' or '-', starting with a letter "
    "or digit";

/// Under --baseline, a --set of a baseline setting that no --baseline-allow item declares takes
/// the run off the baseline: the message for the first one, empty when there is none.
static std::string BaselineOverrideRefusal(const std::vector<std::string>& set_names,
                                           const std::vector<std::string>& allow_items) {
    const std::vector<std::string> undeclared =
        Settings::Baseline::UndeclaredOverrides(Settings::values, set_names, allow_items);
    if (undeclared.empty()) {
        return {};
    }
    return "--baseline refused: --set " + undeclared.front() +
           " changes a baseline setting, which takes the run off the baseline. Drop --baseline, "
           "or declare it with --baseline-allow " + undeclared.front();
}

/// The long option getopt_long picks for `name`: spelled out in full, or an abbreviation that
/// only one option (or several of the same kind) starts with. Null when there is none.
static const struct option* FindLongOption(std::string_view name) {
    const struct option* abbreviated = nullptr;
    bool ambiguous = false;
    for (const struct option* entry = kLongOptions; entry->name != nullptr; ++entry) {
        const std::string_view full{entry->name};
        if (full == name) {
            return entry;
        }
        if (!name.empty() && full.starts_with(name)) {
            if (abbreviated == nullptr) {
                abbreviated = entry;
            } else if (abbreviated->has_arg != entry->has_arg || abbreviated->val != entry->val) {
                ambiguous = true;
            }
        }
    }
    return ambiguous ? nullptr : abbreviated;
}

std::vector<LongOptionEntry> LongOptionTable() {
    std::vector<LongOptionEntry> table;
    for (const struct option* entry = kLongOptions; entry->name != nullptr; ++entry) {
        table.push_back({entry->name, entry->has_arg != no_argument,
                         entry->has_arg == required_argument, entry->val});
    }
    return table;
}

std::string_view ResolveLongOption(std::string_view name) {
    const struct option* const found = FindLongOption(name);
    return found == nullptr ? std::string_view{} : std::string_view{found->name};
}

/// What getopt rejected, as a mistake with the options this fork added (--report, --run-seconds,
/// --run-frames, --json, --user-dir, --list-settings, --set, --baseline, --baseline-allow,
/// --print-baseline, --control: every option id from 0x108 up): such an option without its
/// argument, or with one it does not take, is a bad command line whether or not the run is
/// unattended. Empty for any other word, which keeps its older handling.
std::string MisusedBranchOption(std::string_view word) {
    if (!word.starts_with("--")) {
        return {};
    }
    const std::string_view body = word.substr(2);
    const size_t equals = body.find('=');
    const struct option* const found = FindLongOption(body.substr(0, equals));
    if (found == nullptr || found->val < 0x108) {
        return {};
    }
    if (equals == std::string_view::npos && found->has_arg == required_argument) {
        if (found->val == 0x10E) {
            return std::string{kSetOperandMessage};
        }
        if (found->val == 0x120) {
            return std::string{kControlOperandMessage};
        }
        return std::string{"--"} + found->name + " needs an argument";
    }
    if (equals != std::string_view::npos && found->has_arg == no_argument) {
        return std::string{"--"} + found->name + " does not take an argument";
    }
    return {};
}

/// See EarlyOptions. Follows getopt over the same option tables as ParseLaunchParams: which
/// arguments an option takes (the next one whatever it looks like, or the text after "=" or in
/// the same word), abbreviations, and the arguments -p and -x take on their own. An argument that
/// an option takes is never read as an option.
EarlyOptions ScanEarlyOptions(int argc, char* argv[]) noexcept {
    bool version = false;
    bool json = false;
    bool baseline_requested = false;
    std::vector<std::string> allow_items;
    std::vector<std::string> set_names;
    EarlyOptions early{};
    const auto fail = [&early](std::string message) {
        if (early.error.empty()) {
            early.error = std::move(message);
        }
    };
    const auto user_dir = [&early, &fail](const std::optional<std::string>& value) {
        if (!value || value->empty()) {
            fail("--user-dir needs a directory");
        } else if (early.user_dir) {
            fail("--user-dir was given more than once");
        } else {
            early.user_dir = *value;
        }
    };
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--") {
            break;
        }
        if (arg.starts_with("--")) {
            const std::string_view body = arg.substr(2);
            const size_t equals = body.find('=');
            const struct option* const found = FindLongOption(body.substr(0, equals));
            // An unknown option is left to ParseLaunchParams. A value given to an option that
            // takes none makes getopt return an error and the option is not parsed: for the
            // options this fork added that is a bad command line, here as there.
            if (found == nullptr) {
                continue;
            }
            if (equals != std::string_view::npos && found->has_arg == no_argument) {
                if (std::string misuse = MisusedBranchOption(arg); !misuse.empty()) {
                    fail(std::move(misuse));
                }
                continue;
            }
            std::optional<std::string> value;
            if (equals != std::string_view::npos) {
                value = std::string{body.substr(equals + 1)};
            } else if (found->has_arg == required_argument && i + 1 < argc) {
                value = argv[++i];
            } else if (found->has_arg == required_argument && found->val != 0x10C) {
                // The last word, and the option needs an argument (--user-dir says it itself).
                if (std::string misuse = MisusedBranchOption(arg); !misuse.empty()) {
                    fail(std::move(misuse));
                }
            }
            switch (found->val) {
            case 'v':
                version = true;
                break;
            case 0x10B:
                json = true;
                break;
            case 0x10C:
                user_dir(value);
                break;
            case 0x10E:
                // Only the name: the value may be a credential. A malformed request is left to
                // ParseLaunchParams, which says what is wrong with it.
                if (value) {
                    if (const size_t name_end = value->find('=');
                        name_end != std::string::npos && name_end != 0) {
                        set_names.push_back(value->substr(0, name_end));
                    }
                }
                break;
            case 0x110:
                baseline_requested = true;
                break;
            case 0x111:
                if (value) {
                    for (auto& item : Settings::Baseline::SplitAllowList(*value)) {
                        allow_items.push_back(std::move(item));
                    }
                }
                break;
            default:
                break;
            }
            // --program and --filter take the next argument themselves (see ParseLaunchParams).
            if ((found->val == 'p' || found->val == 'x') && i + 1 < argc) {
                ++i;
            }
        } else if (arg.size() > 1 && arg.front() == '-') {
            bool takes_next = false;
            for (size_t k = 1; k < arg.size(); ++k) {
                const char letter = arg[k];
                const char* const spec = letter == ':' ? nullptr : std::strchr(kShortOptions, letter);
                if (spec == nullptr) {
                    continue;
                }
                if (letter == 'v') {
                    version = true;
                }
                if (letter == 'p' || letter == 'x') {
                    takes_next = true;
                }
                if (spec[1] == ':') {
                    // The rest of the word is the argument; a required one without it is the next.
                    if (spec[2] != ':' && k + 1 == arg.size() && i + 1 < argc) {
                        ++i;
                    }
                    break;
                }
            }
            if (takes_next && i + 1 < argc) {
                ++i;
            }
        }
    }
    // Refused here, before the user directory is touched and before --version --json prints.
    if (baseline_requested) {
        if (std::string refusal = BaselineOverrideRefusal(set_names, allow_items); !refusal.empty()) {
            fail(std::move(refusal));
        }
    }
    early.version_json = version && json;
    return early;
}

/// Parses the command line options into a LaunchParams.
LaunchParams ParseLaunchParams(Core::System& system, int argc, char *argv[], wchar_t *argv_w[]) noexcept {
    LaunchParams p{};

    int option_index = 0;


    // The first problem with the automation options is the one reported (see LaunchParams).
    const auto fail = [&p](std::string message) {
        if (p.parse_error.empty()) {
            p.parse_error = std::move(message);
        }
    };

    while (optind < argc) {
        // -d takes an optional port (-d1234); -i takes the input profile. Both read optarg but
        // were declared without arguments, so optarg was null and they crashed.
        if (int arg = getopt_long(argc, argv, kShortOptions, kLongOptions, &option_index); arg != -1) {
            switch (arg) {
            case 'd':
                // Without a port, the configured one is used (the config is not loaded yet).
                p.enable_gdbstub = true;
                if (optarg != nullptr) {
                    if (const auto port = ParsePort(optarg)) {
                        p.override_gdb_port = *port;
                    } else {
                        LOG_ERROR(Frontend, "Invalid --debug port '{}', using the configured port",
                                  optarg);
                    }
                }
                break;
            case 'c':
                p.config_path = optarg;
                break;
            case 'f':
                p.fullscreen = true;
                LOG_INFO(Frontend, "Starting in fullscreen mode...");
                break;
            case 'h':
                p.print_help = true;
                break;
            case 'g':
                p.filepath = std::string(optarg);
                break;
            case 'M':
                p.mcp_enable = true;
                break;
            case 'P':
                p.mcp_enable = true;
                if (const auto port = ParsePort(optarg)) {
                    p.mcp_port = *port;
                } else {
                    LOG_ERROR(Frontend, "Invalid --mcp-port '{}', using the configured port",
                              optarg);
                }
                break;
            case 'W':
                p.mcp_allow_writes = true;
                break;
            case 0x120: {
                // The name becomes part of a pipe name or a socket file name.
                const std::string name{optarg};
                bool valid = !name.empty() && name.size() <= 64 && std::isalnum(static_cast<unsigned char>(name[0])) != 0 &&
                             static_cast<unsigned char>(name[0]) < 0x80;
                for (const char c : name) {
                    valid = valid && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                      (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-');
                }
                if (p.control_name) {
                    fail("--control was given more than once");
                } else if (!valid) {
                    fail(std::string{kControlOperandMessage});
                } else {
                    p.control_name = name;
                }
                break;
            }
            case 'N':
                p.pack_nup_source = std::string(optarg);
                break;
            case 'U':
                p.unpack_nup_source = std::string(optarg);
                break;
            case 'E':
                p.edit_nup_source = std::string(optarg);
                break;
            case 0x100:
                p.edit_nup_name = std::string(optarg);
                break;
            case 0x101:
                p.edit_nup_icon = std::string(optarg);
                break;
            case 0x102:
                p.edit_nup_version = std::string(optarg);
                break;
            case 0x103:
                p.edit_nup_publisher = std::string(optarg);
                break;
            case 0x104:
                p.edit_nup_name = std::string{};
                break;
            case 0x105:
                p.edit_nup_icon = std::string{};
                break;
            case 0x106:
                p.edit_nup_version = std::string{};
                break;
            case 0x107:
                p.edit_nup_publisher = std::string{};
                break;
            case 'i': {
                p.input_profile = std::string(optarg);
                break;
            }
            case 'm': {
                p.use_multiplayer = true;
                const std::string str_arg(optarg);
                // regex to check if the format is nickname:password@ip:port
                // with optional :password
                const std::regex re("^([^:]+)(?::(.+))?@([^:]+)(?::([0-9]+))?$");
                if (std::regex_match(str_arg, re)) {
                    std::smatch match;
                    std::regex_search(str_arg, match, re);
                    ASSERT(match.size() == 5);
                    p.nickname = match[1];
                    p.password = match[2];
                    p.address = match[3];
                    if (!match[4].str().empty()) {
                        p.port = u16(std::strtoul(match[4].str().c_str(), nullptr, 0));
                    }
                    std::regex nickname_re("^[a-zA-Z0-9._\\- ]+$");
                    ASSERT(std::regex_match(p.nickname, nickname_re) && "Nickname is not valid. Must be 4 to 20 alphanumeric characters");
                    ASSERT(!p.address.empty() && "Address is empty");
                }
                break;
            }
            case 'p':
                p.program_args = argv[optind];
                ++optind;
                break;
            case 'u': {
                // Launch game with a specific user
                bool argument_ok = isdigit(optarg[0]) != 0;
                p.selected_user = atoi(optarg);
                if (!argument_ok) {
                    // try to look it up by username, only finds the first username that matches.
                    auto const user_idx = system.GetProfileManager().GetUserIndex(optarg);
                    if (user_idx != std::nullopt) {
                        p.selected_user = user_idx.value();
                    } else {
                        LOG_ERROR(Frontend, "Invalid user argument '{}'", optarg);
                        break;
                    }
                }
                if (system.GetProfileManager().UserExistsIndex(*p.selected_user)) {
                    Settings::values.current_user = s32(*p.selected_user);
                } else {
                    LOG_ERROR(Frontend, "Selected user {} doesn't exist", *p.selected_user);
                }
                break;
            }
            case 'v':
                p.print_version = true;
                break;
            case 'n':
                p.force_null_render = true;
                break;
            case 's':
                p.force_single_core = true;
                break;
            case 'x':
                p.log_filter = argv[optind];
                ++optind;
                break;
            case 0x108:
                if (optarg[0] == '\0') {
                    fail("--report needs a file name");
                } else if (p.report_path.has_value()) {
                    p.report_conflict = true;
                    fail("--report was given more than once");
                } else {
                    p.report_path = std::string(optarg);
                }
                break;
            case 0x109:
                if (p.run_seconds.has_value()) {
                    fail("--run-seconds was given more than once");
                } else if (const auto seconds = ParseSeconds(optarg)) {
                    p.run_seconds = *seconds;
                } else {
                    fail(std::string("invalid --run-seconds '") + optarg +
                         "': expected a positive number of seconds");
                }
                break;
            case 0x10A:
                if (p.run_frames.has_value()) {
                    fail("--run-frames was given more than once");
                } else if (const auto frames = ParseFrames(optarg)) {
                    p.run_frames = *frames;
                } else {
                    fail(std::string("invalid --run-frames '") + optarg +
                         "': expected a positive whole number");
                }
                break;
            case 0x10B:
                p.print_json = true;
                break;
            case 0x10C:
                if (optarg[0] == '\0') {
                    fail("--user-dir needs a directory");
                } else if (p.user_dir.has_value()) {
                    fail("--user-dir was given more than once");
                } else {
                    p.user_dir = std::string(optarg);
                }
                break;
            case 0x10D:
                p.list_settings = true;
                break;
            case 0x10E: {
                const std::string_view request{optarg};
                const size_t equals = request.find('=');
                if (equals == std::string_view::npos || equals == 0) {
                    // The text is not repeated: it may be a value, and the value may be a credential.
                    fail(std::string{kSetOperandMessage});
                    break;
                }
                std::string name{request.substr(0, equals)};
                const bool repeated = std::any_of(
                    p.setting_overrides.begin(), p.setting_overrides.end(),
                    [&name](const auto& earlier) { return earlier.first == name; });
                if (repeated) {
                    fail("--set " + name + " was given more than once");
                } else {
                    p.setting_overrides.emplace_back(std::move(name),
                                                     std::string{request.substr(equals + 1)});
                }
                break;
            }
            case 0x110:
                p.baseline = true;
                break;
            case 0x111:
                for (const auto& token : Settings::Baseline::SplitAllowList(optarg)) {
                    if (token.empty() || token.front() == '=') {
                        fail(std::string("invalid --baseline-allow '") + optarg +
                             "': expected KEY[,KEY...], each KEY or KEY=VALUE");
                    } else {
                        p.baseline_allow.push_back(token);
                    }
                }
                break;
            case 0x112:
                p.print_baseline = true;
                break;
            case '?':
                // getopt leaves the value of the option in optopt when its operand is missing at
                // the end of the command line; it is 0 for an unknown long option. The options of
                // this fork that take an operand refuse that always.
                if (optopt == 0x10C) {
                    fail("--user-dir needs a directory");
                } else if (optopt == 0x10E) {
                    fail(std::string{kSetOperandMessage});
                } else if (optopt == 0x120) {
                    fail(std::string{kControlOperandMessage});
                } else {
                    // An unknown option, or an old one without its argument: only an automated
                    // run (--report, --run-seconds, --run-frames) refuses that. A misused option
                    // of this fork's is refused whatever the run is.
                    if (p.unknown_option.empty()) {
                        p.unknown_option = optind > 0 ? argv[optind - 1] : argv[0];
                    }
                    if (const std::string misuse =
                            MisusedBranchOption(optind > 0 ? argv[optind - 1] : argv[0]);
                        !misuse.empty()) {
                        fail(misuse);
                    }
                }
                break;
            }
        } else {
            // only kept due to shortcuts made by Qt frontend which use "-qlaunch"
            if (strcmp(argv[optind], "-hlaunch") == 0) {
                p.launch_hlaunch = true;
            } else if (strcmp(argv[optind], "-qlaunch") == 0) {
                p.launch_qlaunch = true;
            } else if (strcmp(argv[optind], "-setup") == 0) {
                p.launch_setup = true;
            } else {
                // qt feeds utf8 data, sdl frontend feeds raw utf16 data
#ifdef _WIN32
                p.filepath = argv_w != nullptr
                    ? Common::UTF16ToUTF8(argv_w[optind])
                    : argv[optind];
#else
                p.filepath = argv[optind];
#endif
            }
            optind++;
        }
    }
    if (p.print_json && !p.print_version) {
        fail("--json is only valid together with --version");
    }
    if (!p.baseline && !p.baseline_allow.empty()) {
        fail("--baseline-allow needs --baseline");
    }
    if (p.baseline) {
        // The same refusal as ScanEarlyOptions, wherever --set and --baseline stand.
        std::vector<std::string> set_names;
        for (const auto& request : p.setting_overrides) {
            set_names.push_back(request.first);
        }
        if (std::string refusal = BaselineOverrideRefusal(set_names, p.baseline_allow);
            !refusal.empty()) {
            fail(std::move(refusal));
        }
    }
    if ((p.report_path || p.run_seconds || p.run_frames) &&
        (p.print_help || p.print_version || p.list_settings || p.print_baseline ||
         p.pack_nup_source || p.unpack_nup_source || p.edit_nup_source)) {
        fail("--report, --run-seconds and --run-frames cannot be combined with --help, "
             "--version, --list-settings, --print-baseline, --pack-nup, --unpack-nup or "
             "--edit-nup");
    }
    p.argv0 = argv[0];
    return p;
}

/// Applies the parsed command line options to the log filter and settings.
void ApplyLaunchParams(LaunchParams const& lp) noexcept {
    // apply the log_filter setting
    // the logger was initialized before and doesn't pick up the filter on its own
    Common::Log::Filter filter{};
    filter.ParseFilterString(lp.log_filter.value_or(Settings::values.log_filter.GetValue()));
    Common::Log::SetGlobalFilter(filter);

    if (!lp.program_args.empty()) {
        Settings::values.program_args = lp.program_args;
    }
    if (!lp.input_profile.empty()) {
        auto& players = Settings::values.players.GetValue();
        players[0].profile_name = lp.input_profile;
    }
    if (lp.selected_user.has_value()) {
        Settings::values.current_user = std::clamp(*lp.selected_user, 0, 7);
    }
    if (lp.enable_gdbstub) {
        Settings::values.use_gdbstub = true;
    }
    if (lp.override_gdb_port.has_value()) {
        Settings::values.gdbstub_port = *lp.override_gdb_port;
    }
    if (lp.force_single_core) {
        Settings::values.use_multi_core = false;
        Settings::Baseline::NoteSource("use_multi_core", "command line");
    }
    if (lp.force_null_render) {
        Settings::values.renderer_backend = Settings::RendererBackend::Null;
        Settings::Baseline::NoteSource("backend", "command line");
    }

    if (lp.print_version && !lp.print_json) {
        LOG_INFO(Frontend, "{} | commit {}", Common::BuildDisplayName(), Common::g_scm_rev);
    }
    if (lp.print_help) {
        LOG_INFO(Frontend,
            "Usage: {}"
            " [options] <filename>\n"
            "-c, --config          Load the specified configuration file\n"
            "-f, --fullscreen      Start in fullscreen mode\n"
            "-g, --game            File path of the game to load\n"
            "    --control <name>  Open a control channel for one local client: the pipe\n"
            "                      \\\\.\\pipe\\<name> on Windows, <name>.sock in the runtime\n"
            "                      directory elsewhere. One JSON request per line; commands:\n"
            "                      status, press, release, screenshot, get, set, stop\n"
            "-h, --help            Display this help and exit\n"
            "-m, --multiplayer=nick:password@address:port"
            " Nickname, password, address and port for multiplayer\n"
            "-p, --program         Pass following string as arguments to executable\n"
            "-u, --user            Select a specific user profile from 0 to 7\n"
            "--user-dir <path>     Use this directory as the user directory (configuration, saves,\n"
            "                      shader cache, logs) for this run; created when missing. Spell it out in full\n"
            "-d, --debug           Run the GDB stub on a port from 1 to 65535\n"
            "-v, --version         Output version information and exit\n"
            "--version --json      Print the build identity (version, revision, compiler, options) as one\n"
            "                      JSON object on standard output and exit; no window, no user directory\n"
            "--pack-nup <dump> [<out.nup>]"
            " Pack a loose dump with all its updates and DLC into a NUP file and exit\n"
            "--unpack-nup <file.nup> [<out_dir>]"
            " Extract a NUP back into a loose dump and exit\n"
            "--edit-nup <file.nup> [--nup-name NAME --nup-icon PATH --nup-version VER"
            " --nup-publisher NAME --clear-nup-name --clear-nup-icon --clear-nup-version"
            " --clear-nup-publisher]"
            " Rewrite default-language NUP metadata in place and exit\n"
            "--report <file.json>  Write one JSON report (frame rate, thread time, waits, errors,\n"
            "                      build, settings, outcome) when the run ends; the file must not exist\n"
            "--run-seconds <n>     End the run after n seconds (exit code 124)\n"
            "--run-frames <n>      End the run after n frames (exit code 0)\n"
            "--list-settings       Print every setting (name, type, value, default, range) as JSON lines and exit\n"
            "--set <name=value>    Set one setting for this run only, after the configuration file is read;\n"
            "                      never saved. Can be repeated. An unknown name or an invalid value is a\n"
            "                      bad argument (exit code 64). The other command line options win over it\n"
            "--baseline            Run on the baseline settings: after the config loads, every baseline\n"
            "                      setting is reset to its default; refuses an EDEN_NX2_* switch, and a\n"
            "                      --set of a baseline setting, that --baseline-allow does not name (exit 64)\n"
            "                      The configuration file is read and never written; a missing one is\n"
            "                      not created and the defaults are used\n"
            "--baseline-allow KEY[,KEY...]  Declare a variation from the baseline; KEY=VALUE also sets\n"
            "                      the setting (for example resolution_setup=6)\n"
            "--print-baseline      Print the baseline version, hash and settings, and exit\n"
            "Exit codes: 0 normal or frame limit, 1 startup failure, 64 bad arguments, 70 guest crash,\n"
            "71 device lost, 72 shader or pipeline failure, 73 emulator crash, 74 report not written,\n"
            "75 restart, 124 --run-seconds reached, 130 interrupted. The codes after 1 apply only to\n"
            "runs that use --report, --run-seconds or --run-frames. After an emulator crash the report\n"
            "has exit.reason host_crash and only the frame count of the summary.\n",
            lp.argv0
        );
    }
}

}
