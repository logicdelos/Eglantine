// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

namespace FrontendCommon {

/// Where the staged import keeps its state: next to the user directory, never inside it.
std::filesystem::path ProfileImportStateDirectory();

/// True when a legacy emulator profile can still be imported (or an import was interrupted) into
/// the standard per-user directory. Never true for a portable or --user-dir profile. Importing is
/// always optional: nothing is read or changed unless the user asks for it.
bool NeedsLegacyProfileImport();

/// `value` moved from below `source` to the same place below `destination`; any other path is
/// returned unchanged.
std::filesystem::path RemapImportedPath(const std::filesystem::path& value,
                                        const std::filesystem::path& source,
                                        const std::filesystem::path& destination);

/// Points the data-storage and screenshot paths of every .ini below `config_directory` that
/// lie inside `source` at `destination`.
void RemapImportedConfig(const std::filesystem::path& config_directory,
                         const std::filesystem::path& source,
                         const std::filesystem::path& destination);

} // namespace FrontendCommon
