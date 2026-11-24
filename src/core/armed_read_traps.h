// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <map>
#include <optional>

#include "common/common_types.h"

namespace Core::Memory {

/// The guest pages the core keeps read-trapped, each with the version of its latest arm.
///
/// A reader that claims a page remembers the version it saw and releases the page only if that
/// version is still current. A GPU write that re-arms the page in between bumps the version, so
/// the release is dropped and the page stays trapped. Not thread safe: the owner serializes.
class ArmedReadTraps {
public:
    /// Arms a page. Returns true when it was not armed yet; an armed page gets a new version.
    bool Arm(u64 page) {
        const auto [it, inserted] = pages.try_emplace(page, ++last_version);
        if (!inserted) {
            it->second = ++last_version;
        }
        return inserted;
    }

    /// The version of an armed page.
    [[nodiscard]] std::optional<u64> Version(u64 page) const {
        const auto it = pages.find(page);
        if (it == pages.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// Releases a page if it is armed with exactly this version. Returns whether it was released.
    bool Release(u64 page, u64 version) {
        const auto it = pages.find(page);
        if (it == pages.end() || it->second != version) {
            return false;
        }
        pages.erase(it);
        return true;
    }

    /// Calls `func(page, version)` for each armed page in [first, end).
    template <typename Func>
    void ForEachInRange(u64 first, u64 end, Func&& func) const {
        for (auto it = pages.lower_bound(first); it != pages.end() && it->first < end; ++it) {
            func(it->first, it->second);
        }
    }

    /// Calls `func(page)` for each armed page in [first, end), then forgets it.
    template <typename Func>
    void EraseEach(u64 first, u64 end, Func&& func) {
        auto it = pages.lower_bound(first);
        while (it != pages.end() && it->first < end) {
            func(it->first);
            it = pages.erase(it);
        }
    }

    /// Forgets the armed pages in [first, end). Returns how many there were.
    size_t Erase(u64 first, u64 end) {
        size_t erased = 0;
        EraseEach(first, end, [&erased](u64) { ++erased; });
        return erased;
    }

    [[nodiscard]] size_t Size() const {
        return pages.size();
    }

private:
    std::map<u64, u64> pages;
    u64 last_version = 0;
};

} // namespace Core::Memory
