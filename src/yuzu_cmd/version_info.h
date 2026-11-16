// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

// What `eden-cli --version --json` prints: the build identity as one JSON object.
namespace VersionInfo {

/// One line of compact JSON ending in a newline. Keys are sorted; a value the build does not
/// record is null. Reads only compiled-in constants: no settings, no files, no user directory.
[[nodiscard]] std::string BuildInfoJson();

} // namespace VersionInfo
