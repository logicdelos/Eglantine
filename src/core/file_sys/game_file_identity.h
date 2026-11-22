// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <ctime>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// The one log line that says which game file a session ran: path, size, modification time, kind,
// the add-ons the host identity counted, and the cache content stamp. A bug report that says "it
// broke after I replaced the .nup" can then be checked against the log alone.
namespace FileSys::GameFileIdentity {

struct FileFacts {
    std::string path;
    bool exists{};
    bool directory{};
    u64 size{}; ///< bytes of a file; 0 for a directory
    std::optional<std::time_t> modified;
};

struct AddonSummary {
    std::size_t updates{};
    std::size_t dlcs{};
    std::size_t mods{};
    std::vector<std::string> entries; ///< `update "name" v1.2.0`, in label order
};

/// Stats `path` (UTF-8 text in the result). Never throws; a missing path gives exists=false.
[[nodiscard]] FileFacts Inspect(const std::filesystem::path& path);

/// "nup", "nsp", "xci", ... from the extension, "directory" for a loose tree, "unknown" without one.
[[nodiscard]] std::string KindOf(const FileFacts& facts);

/// Reads the `type|program_id|numeric_version|name|version` labels of the host identity
/// (type 0 update, 1 DLC, 2 mod). Labels of another shape (`file|...`) are skipped.
[[nodiscard]] AddonSummary SummarizeAddons(std::span<const std::string> labels);

/// "14.20 GiB" / "512.0 MiB" / "900 B".
[[nodiscard]] std::string FormatSize(u64 bytes);

[[nodiscard]] std::string FormatIdentity(const FileFacts& facts, u64 title_id, u64 game_version,
                                         const AddonSummary& addons, u64 content_stamp);

} // namespace FileSys::GameFileIdentity
