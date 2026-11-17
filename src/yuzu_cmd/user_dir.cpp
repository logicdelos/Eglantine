// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <chrono>
#include <string>
#include <system_error>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "yuzu_cmd/user_dir.h"

namespace UserDir {

std::string Use(const std::string& text) {
    namespace fs = std::filesystem;

    if (text.empty()) {
        return "the directory name is empty";
    }

    std::error_code ec;
    fs::path root = fs::absolute(fs::path{Common::FS::ToU8String(text)}, ec);
    if (ec) {
        return fmt::format("cannot resolve '{}': {}", text, ec.message());
    }
    root = root.lexically_normal();
    if (!root.has_filename()) {
        // "a/b/" normalises to a path with a trailing separator.
        root = root.parent_path();
    }
    const std::string shown = Common::FS::PathToUTF8String(root);

    fs::create_directories(root, ec);
    const std::error_code create_error = ec;
    if (!fs::is_directory(root, ec)) {
        return create_error ? fmt::format("cannot create '{}': {}", shown, create_error.message())
                            : fmt::format("'{}' is not a directory", shown);
    }

    // A directory that exists can still refuse writes (read-only mount, no permission). The probe
    // is a new subdirectory: create_directory reports false when the name already exists, so a
    // file or directory of the user's is never opened, truncated or removed, only what this call
    // created is.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    bool writable = false;
    for (int attempt = 0; attempt < 8 && !writable; ++attempt) {
        const fs::path probe = root / fmt::format(".eglantine-write-test-{}-{}", stamp, attempt);
        std::error_code probe_error;
        if (fs::create_directory(probe, probe_error)) {
            writable = true;
            fs::remove(probe, probe_error);
        } else if (probe_error) {
            break;
        }
    }
    if (!writable) {
        return fmt::format("'{}' is not writable", shown);
    }

    Common::FS::UseUserDirectory(root);
    return {};
}

} // namespace UserDir
