// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Control {

enum class PublishResult {
    Ok,
    Exists,     // the destination exists and replacing it was not asked for
    NotRegular, // the destination is not an ordinary file, or names a device
    IoError,
};

struct PublishOutcome {
    PublishResult result = PublishResult::Ok;
    std::string message;
};

/// Writes `data` to a new, exclusively created temporary file in the directory of `destination`
/// and moves it into place. Without `overwrite` the move fails when the destination exists, even
/// when it appeared after the call started; with it, an ordinary file is replaced. The destination
/// is never opened, so a special file there (a FIFO, a device) cannot block the call. Only the
/// temporary file this call created is ever removed.
[[nodiscard]] PublishOutcome PublishFile(const std::filesystem::path& destination,
                                         std::string_view data, bool overwrite);

} // namespace Control
