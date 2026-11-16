// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/common_types.h"

// The canonical content manifest the cache identity's content stamp is the hash of. Standard
// library only, so that the unit tests can build trees in a temporary directory and compare.
namespace RunControl {

/// FNV-1a 64 over length-prefixed fields, so that two different field lists never hash the same
/// bytes by running together.
class Fnv64 {
public:
    void Bytes(const void* data, size_t size) noexcept {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            value = (value ^ bytes[i]) * 0x100000001b3ull;
        }
    }
    void U64(u64 field) noexcept {
        for (int shift = 0; shift < 64; shift += 8) {
            const auto byte = static_cast<unsigned char>(field >> shift);
            Bytes(&byte, 1);
        }
    }
    void Text(std::string_view field) noexcept {
        U64(field.size());
        Bytes(field.data(), field.size());
    }
    [[nodiscard]] u64 Value() const noexcept {
        return value;
    }

private:
    u64 value{0xcbf29ce484222325ull};
};

/// The most files and directories one manifest walks. Past it the walk stops, in a deterministic
/// place (children are visited in name order), and the manifest says so: the flag is part of the
/// hash and of the report. A stat of a file is the cost, so the cap bounds the cold start.
inline constexpr size_t MaxManifestEntries = 100'000;
inline constexpr size_t MaxManifestDepth = 32;

[[nodiscard]] inline std::string PathText(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    std::string result;
    result.reserve(text.size());
    for (const auto c : text) {
        result.push_back(static_cast<char>(c));
    }
    return result;
}

[[nodiscard]] inline std::filesystem::path PathFromText(std::string_view text) {
    std::u8string wide;
    wide.reserve(text.size());
    for (const char c : text) {
        wide.push_back(static_cast<char8_t>(c));
    }
    return std::filesystem::path{wide};
}

/// A file's size and last write time; false when it cannot be read.
[[nodiscard]] inline bool StatFile(const std::filesystem::path& path, u64& size, u64& write_time) {
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return false;
    }
    size = static_cast<u64>(file_size);
    write_time = static_cast<u64>(time.time_since_epoch().count());
    return true;
}

/// The file on the host that holds `virtual_path`: the path itself, or the longest prefix of it
/// that is a regular file (a content file inside a package is named by the package's path).
/// Empty when there is none.
[[nodiscard]] inline std::filesystem::path ExistingFilePrefix(std::string_view virtual_path) {
    std::filesystem::path path = PathFromText(virtual_path);
    std::error_code ec;
    while (!path.empty()) {
        if (std::filesystem::is_regular_file(path, ec)) {
            return path;
        }
        std::filesystem::path parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = std::move(parent);
    }
    return {};
}

class ContentManifest {
public:
    explicit ContentManifest(size_t max_entries_ = MaxManifestEntries)
        : max_entries{max_entries_} {}

    void Text(std::string_view label, std::string_view value) {
        hash.Text(label);
        hash.Text(value);
    }

    void Number(std::string_view label, u64 value) {
        hash.Text(label);
        hash.U64(value);
    }

    /// A file by its size and last write time; a file that cannot be read is a different entry
    /// from any that can.
    void File(std::string_view label, const std::filesystem::path& path) {
        hash.Text(label);
        u64 size = 0;
        u64 time = 0;
        const bool readable = StatFile(path, size, time);
        ++entries;
        hash.U64(readable ? 1 : 0);
        hash.U64(size);
        hash.U64(time);
    }

    /// Every file under `root` (relative path, size, last write time), in name order. A root that
    /// is a file is that file; one that cannot be read is an entry of its own. Contents are never
    /// read: that would cost the cold start the time the cache is there to save, and a change
    /// that keeps a file's size and time is not seen (documented trade-off).
    void Tree(std::string_view label, const std::filesystem::path& root) {
        hash.Text(label);
        std::error_code ec;
        if (std::filesystem::is_directory(root, ec)) {
            hash.U64(1);
            Walk(root, std::string{}, 0);
        } else {
            hash.U64(0);
            File("root", root);
        }
    }

    /// The hash of everything added; the over-cap flag is part of it.
    [[nodiscard]] u64 Digest() const noexcept {
        Fnv64 result = hash;
        result.U64(over_cap ? 1 : 0);
        return result.Value();
    }

    [[nodiscard]] size_t Entries() const noexcept {
        return entries;
    }

    [[nodiscard]] bool OverCap() const noexcept {
        return over_cap;
    }

private:
    struct Child {
        std::string name;
        std::filesystem::path path;
    };

    void Walk(const std::filesystem::path& directory, const std::string& prefix, size_t depth) {
        std::vector<Child> children;
        std::error_code ec;
        for (std::filesystem::directory_iterator it{
                 directory, std::filesystem::directory_options::skip_permission_denied, ec};
             !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
            children.push_back({PathText(it->path().filename()), it->path()});
        }
        std::ranges::sort(children, [](const Child& a, const Child& b) { return a.name < b.name; });
        for (const Child& child : children) {
            if (entries >= max_entries) {
                over_cap = true;
                return;
            }
            ++entries;
            const std::string relative = prefix.empty() ? child.name : prefix + "/" + child.name;
            const auto status = std::filesystem::symlink_status(child.path, ec);
            if (!ec && std::filesystem::is_directory(status)) {
                hash.Text("d");
                hash.Text(relative);
                if (depth + 1 < MaxManifestDepth) {
                    Walk(child.path, relative, depth + 1);
                } else {
                    over_cap = true;
                }
                continue;
            }
            u64 size = 0;
            u64 time = 0;
            const bool readable = !ec && std::filesystem::is_regular_file(status) &&
                                  StatFile(child.path, size, time);
            hash.Text("f");
            hash.Text(relative);
            hash.U64(readable ? 1 : 0);
            hash.U64(size);
            hash.U64(time);
        }
    }

    size_t max_entries;
    size_t entries{};
    bool over_cap{};
    Fnv64 hash;
};

} // namespace RunControl
