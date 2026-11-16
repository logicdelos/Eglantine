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
    {0, 0, 0, 0},
    // clang-format on
};

/// See EarlyOptions. Walks the arguments the way getopt will, using the same tables: an option that
/// takes an operand takes the next argument whatever it says, so a path or a --set value that
/// starts with "--" is never read as an option. -p, -x and --program also take the argument after
/// the one getopt consumes (ParseLaunchParams reads argv[optind] for them). Only the spelled-out
/// --version, -v, --json and --user-dir count; an abbreviation is left to ParseLaunchParams.
EarlyOptions ScanEarlyOptions(int argc, char* argv[]) noexcept {
    bool version = false;
    bool json = false;
    EarlyOptions early{};
    const auto fail = [&early](std::string message) {
        if (early.error.empty()) {
            early.error = std::move(message);
        }
    };
    const auto take_user_dir = [&](const std::optional<std::string>& value) {
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
        int extra_operands = 0;
        if (arg.starts_with("--")) {
            const std::string_view body = arg.substr(2);
            const std::size_t equals = body.find('=');
            const std::string_view name = body.substr(0, equals);
            const bool attached = equals != std::string_view::npos;
            const struct option* found = nullptr;
            bool exact = false;
            int matches = 0;
            for (const struct option* entry = kLongOptions; entry->name != nullptr; ++entry) {
                const std::string_view candidate{entry->name};
                if (candidate == name) {
                    found = entry;
                    exact = true;
                    matches = 1;
                    break;
                }
                if (candidate.starts_with(name)) {
                    found = entry;
                    ++matches;
                }
            }
            if (matches != 1) {
                // Unknown or ambiguous: getopt reports it and takes no operand.
                continue;
            }
            std::optional<std::string> value;
            if (attached && found->has_arg != no_argument) {
                value = std::string{body.substr(equals + 1)};
            } else if (!attached && found->has_arg == required_argument && i + 1 < argc) {
                value = std::string{argv[++i]};
            }
            if (found->val == 'p' || found->val == 'x') {
                extra_operands = 1;
            }
            if (exact) {
                if (found->val == 'v') {
                    version = true;
                } else if (found->val == 0x10B) {
                    json = true;
                } else if (found->val == 0x10C) {
                    take_user_dir(value);
                }
            }
        } else if (arg.size() > 1 && arg[0] == '-') {
            for (std::size_t k = 1; k < arg.size(); ++k) {
                const char letter = arg[k];
                const char* spec = letter == ':' ? nullptr : std::strchr(kShortOptions, letter);
                if (spec == nullptr) {
                    continue;
                }
                if (letter == 'v') {
                    version = true;
                }
                if (letter == 'p' || letter == 'x') {
                    ++extra_operands;
                }
                if (spec[1] == ':') {
                    // Optional operands are attached only; a required one is the rest of this
                    // argument or else the next argument.
                    if (spec[2] != ':' && k + 1 == arg.size() && i + 1 < argc) {
                        ++i;
                    }
                    break;
                }
            }
        }
        i += extra_operands;
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
                    fail("--set expects name=value, with a name and an equals sign");
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
            case '?':
                // An unknown option, or one without its argument. Only an automated run
                // (--report, --run-seconds, --run-frames) refuses it, so nothing else changes.
                if (p.unknown_option.empty()) {
                    p.unknown_option = optind > 0 ? argv[optind - 1] : argv[0];
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
    if ((p.report_path || p.run_seconds || p.run_frames) &&
        (p.print_help || p.print_version || p.list_settings || p.pack_nup_source ||
         p.unpack_nup_source || p.edit_nup_source)) {
        fail("--report, --run-seconds and --run-frames cannot be combined with --help, "
             "--version, --list-settings, --pack-nup, --unpack-nup or --edit-nup");
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
    }
    if (lp.force_null_render) {
        Settings::values.renderer_backend = Settings::RendererBackend::Null;
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
