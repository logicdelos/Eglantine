// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>
#include "common/common_types.h"
#include "network/room.h"

namespace Core {

class System;
struct LaunchParams {
    std::string argv0{};
    std::string filepath{};
    std::string nickname{};
    std::string password{};
    std::string address{};
    std::string input_profile{};
    std::string program_args{};
    std::optional<std::string> config_path{};
    // --user-dir <path>: the user directory of this run (applied before the full parse, see
    // EarlyOptions).
    std::optional<std::string> user_dir{};
    std::optional<int> selected_user{};
    bool enable_gdbstub = false;
    std::optional<u16> override_gdb_port{};
    std::optional<std::string> log_filter{};
    // Built-in MCP server: --mcp, --mcp-port <port>, --mcp-allow-writes
    bool mcp_enable = false;
    std::optional<u16> mcp_port{};
    bool mcp_allow_writes = false;
    // --control <name>: a replying control channel on a local pipe or socket (SDL frontend).
    std::optional<std::string> control_name{};
    // --pack-nup <dump> [<out.nup>]: packs a loose dump instead of booting a game (eden-cli).
    std::optional<std::string> pack_nup_source{};
    // --unpack-nup <file.nup> [<out_dir>]: extracts a NUP back into a loose dump.
    std::optional<std::string> unpack_nup_source{};
    // --edit-nup <file.nup>: rewrites default-language metadata in place.
    std::optional<std::string> edit_nup_source{};
    std::optional<std::string> edit_nup_name{};
    std::optional<std::string> edit_nup_icon{};
    std::optional<std::string> edit_nup_version{};
    std::optional<std::string> edit_nup_publisher{};
    // Unattended runs (SDL frontend): --report <file.json>, --run-seconds <n>, --run-frames <n>.
    std::optional<std::string> report_path{};
    std::optional<double> run_seconds{};
    std::optional<u64> run_frames{};
    // --report was given twice: no destination is trustworthy, so no report is written.
    bool report_conflict = false;
    // The first problem with those options (bad value, duplicate, mixed with a command that
    // exits at once); empty when there is none. Only the SDL frontend refuses to start on it.
    std::string parse_error{};
    // The first argument getopt did not understand; refused only by unattended runs.
    std::string unknown_option{};
    // --baseline: reset every baseline setting to its default after the config loads and refuse an
    // EDEN_NX2_* switch that is not declared. --baseline-allow KEY[,KEY...] declares variations;
    // KEY=VALUE also sets the setting. --print-baseline prints the baseline and exits.
    bool baseline = false;
    std::vector<std::string> baseline_allow{};
    bool print_baseline = false;
    u16 port = Network::DefaultRoomPort;
    bool use_multiplayer = false;
    bool fullscreen = false;
    bool force_null_render = false;
    bool force_single_core = false;
    // print
    bool print_version = false;
    // --json (with --version): the build identity as one JSON object on standard output.
    bool print_json = false;
    // --list-settings: every setting with its value, type and default as JSON lines, then exit.
    bool list_settings = false;
    // --set name=value, in the order given, split at the first '='. Not checked here: the names
    // and the types belong to the settings registry.
    std::vector<std::pair<std::string, std::string>> setting_overrides{};
    bool print_help = false;
    // qt
    bool launch_hlaunch = false;
    bool launch_qlaunch = false;
    bool launch_setup = false;
};

/// The options that must act before the log, the crash handler and Core::System exist. They are
/// found by a scan of their own that does not use getopt, so it can run before ParseLaunchParams;
/// ParseLaunchParams still parses them, to report mistakes and for the callers that skip the scan.
struct EarlyOptions {
    // --version together with --json, each spelled out or abbreviated as getopt accepts it.
    bool version_json = false;
    // --user-dir <path> or --user-dir=<path> (or an abbreviation getopt accepts).
    std::optional<std::string> user_dir{};
    // The first mistake in those options (no value, given twice); empty when there is none.
    std::string error{};
};

EarlyOptions ScanEarlyOptions(int argc, char* argv[]) noexcept;

/// The message for a word getopt rejected when it is one of the options this fork added
/// (--report, --run-seconds, --run-frames, --json, --user-dir, --list-settings, --set,
/// --baseline, --baseline-allow, --print-baseline, --control) used without its argument or with
/// one it does not take; empty otherwise. ParseLaunchParams reports it as a parse error, with or
/// without an unattended run.
std::string MisusedBranchOption(std::string_view word);

/// One entry of the long-option table that getopt_long, ScanEarlyOptions and ResolveLongOption
/// all read.
struct LongOptionEntry {
    std::string_view name;
    bool takes_argument;    // a required or an optional argument
    bool requires_argument; // a required argument
    int id;                 // the value getopt_long returns for it
};

/// A copy of the long-option table, in its order.
std::vector<LongOptionEntry> LongOptionTable();

/// The full name of the long option getopt_long picks for `name` (without the leading "--"): the
/// option spelled out, or the one an abbreviation fits. Empty when no option fits or the
/// abbreviation fits options of different kinds (ambiguous).
std::string_view ResolveLongOption(std::string_view name);

LaunchParams ParseLaunchParams(Core::System& system, int argc, char *argv[], wchar_t *argv_w[]) noexcept;
void ApplyLaunchParams(LaunchParams const& lp) noexcept;

}
