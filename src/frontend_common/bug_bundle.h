// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace FrontendCommon::BugBundle {

/// Room kept free under a chat upload limit, so the zip can be shared as it is.
constexpr u64 DefaultSizeLimit = 19ULL << 20;

/// What goes into a bug report. Nothing is read or sent without the user's review: the dialog
/// builds this, shows Preview() of it and only then asks for WriteBundle().
struct Report {
    std::string description;
    /// "ui" for the Help menu and the hotkey.
    std::string trigger{"ui"};

    /// Filled by the dialog from the running game. All may be empty (no game running).
    std::string game_title;
    std::string game_program_id;
    std::string game_version;

    /// Host description (OS, CPU, RAM, GPU) as "key: value" lines, without user names.
    std::vector<std::pair<std::string, std::string>> system;

    /// Every setting except credentials, with this game's overrides. Redacted already.
    std::string settings;

    /// PNG bytes of the screen when the user chose to include it; empty otherwise.
    std::vector<u8> screenshot_png;
    /// The log, redacted and trimmed to its start and end when long; empty when not included.
    std::string log;
    std::string log_name;
};

struct Result {
    std::filesystem::path path;
    u64 bytes{};
    /// What was left out to fit the size limit, one line each.
    std::vector<std::string> dropped;
};

/// <log dir>/bug-reports
[[nodiscard]] std::filesystem::path DefaultDirectory();

/// The user name of the account that runs the emulator replaced by "user" in text, the way the
/// log file shows it.
[[nodiscard]] std::string Redact(std::string text);

/// Every setting as "[Category] label = value", credentials left out, redacted. A setting a game
/// overrides shows its global value too. With changed_only, only the settings that differ from
/// their defaults.
[[nodiscard]] std::string SettingsReport(bool changed_only = false);

/// The log file as text: redacted, and cut in the middle (first 1 MiB, last 6 MiB) when over
/// 8 MiB. Empty when there is no log.
[[nodiscard]] std::string ReadLog(const std::filesystem::path& path);

/// The summary the user reads before saving: what the zip will hold, with long texts shortened
/// and the screenshot only counted. The user is told to inspect the zip itself before sharing.
[[nodiscard]] std::string Preview(const Report& report);

/// Writes <directory>/eglantine-bug-<stamp>.zip with README.md, bundle.json, settings.txt,
/// the optional screenshot.png and logs/<name>, under size_limit. Parts that do not fit are
/// dropped and listed. Throws std::runtime_error. Nothing is sent anywhere.
[[nodiscard]] Result WriteBundle(const Report& report, const std::filesystem::path& directory,
                                 u64 size_limit = DefaultSizeLimit);

} // namespace FrontendCommon::BugBundle
