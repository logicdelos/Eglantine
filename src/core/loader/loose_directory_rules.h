// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "common/common_types.h"

// How the deconstructed ExeFS loader chooses among directories whose names it reads from the
// host, as pure functions of the names in the order the host listed them. The loader and the
// content identity of the caches (yuzu_cmd/run_control.h) both call these, so that the directory
// the identity covers is the directory the loader uses.
namespace Loader {

/// Whether `name` is `romfs` in any letter case.
[[nodiscard]] inline bool IsRomFSDirectoryName(std::string_view name) {
    return name.size() == 5 && std::ranges::equal(name, std::string_view{"romfs"}, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == b;
           });
}

/// The first name that is `romfs` in some letter case; nullopt when there is none. A directory
/// with several spellings (possible only where names are case sensitive) resolves to the one
/// listed first.
[[nodiscard]] inline std::optional<size_t> FirstRomFSDirectoryIndex(
    std::span<const std::string> names) {
    for (size_t i = 0; i < names.size(); ++i) {
        if (IsRomFSDirectoryName(names[i])) {
            return i;
        }
    }
    return std::nullopt;
}

/// The update directory that stands in for an absent base RomFS: among the names that are whole
/// decimal numbers (a name such as `655360_old` is skipped), the one of the greatest value; of
/// equal values (`1` and `01`) the one listed last. nullopt when no name is a number.
[[nodiscard]] inline std::optional<size_t> NewestUpdateDirectoryIndex(
    std::span<const std::string> names) {
    std::optional<size_t> newest;
    u64 newest_version = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        u64 version = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), version);
        // from_chars stops at the first non digit and still reports success, so the whole name
        // has to be the number.
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) {
            continue;
        }
        if (version >= newest_version) {
            newest_version = version;
            newest = i;
        }
    }
    return newest;
}

} // namespace Loader
