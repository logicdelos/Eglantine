// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "common/settings_common.h"

// `eden-cli --list-settings` and `--set name=value`, both on top of the settings registry
// (Settings::values.linkage): no table of names is kept here.
namespace CliSettings {

/// One --set request that passed the checks: the setting and the text to give its LoadString.
struct Resolved {
    Settings::BasicSetting* setting = nullptr;
    std::string text;
};

/// Checks every `name=value` request (name, value) against the registry: the name must exist and
/// the value must be valid for the type of the setting and inside its range. Nothing is changed
/// when it returns a message; the message names the setting. Enum values are a number or the name
/// shown by --list-settings, in any letter case. Does not depend on the configuration file, so it
/// can run before it is loaded.
[[nodiscard]] std::string Resolve(const std::vector<std::pair<std::string, std::string>>& requests,
                                  std::vector<Resolved>& resolved);

/// Sets each resolved setting for this run. Call it after the configuration file is loaded, and
/// never save the configuration while the overrides are in place.
void Apply(const std::vector<Resolved>& resolved);

/// Every setting of the registry with its current value, as JSON lines (one object per line, each
/// ending in a newline): name, category, type, value, default, saved, and for a ranged number
/// min and max, for an enum value_name, default_name and choices.
[[nodiscard]] std::string ListJsonLines();

} // namespace CliSettings
