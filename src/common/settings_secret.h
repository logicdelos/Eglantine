// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

namespace Settings {

/// Whether the setting called `name` (its key in the settings registry) holds a credential. The
/// registry has no flag for this, so the names decide, in this one place: a name that contains
/// "token", "password", "passwd" or "secret", or ends in "_key", in any letter case. That covers
/// mcp_token and eden_token. eden_username is a public name, not a credential, and is not matched.
///
/// The value of such a setting is never written to a log, a run report or --list-settings output,
/// and is not repeated in an error message. A setting added later whose name follows the rule is
/// covered without a change here.
bool IsSecretSetting(std::string_view name);

} // namespace Settings
