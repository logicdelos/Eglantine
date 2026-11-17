// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

// `eden-cli --user-dir <path>`: the user directory of this run.
namespace UserDir {

/// Makes `text` the user directory (configuration, saves, shader cache, logs, ...) in the
/// portable layout of Common::FS::UseUserDirectory. The directory is made absolute and created
/// when missing; it must be writable. Call it before anything reads a path of the user directory.
/// Returns an empty string on success, else the reason it cannot be used.
[[nodiscard]] std::string Use(const std::string& text);

} // namespace UserDir
