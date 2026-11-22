// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/log_retention.h"
#include "core/file_sys/game_file_identity.h"

namespace FileSys::GameFileIdentity {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t MaxListedAddons = 8;

} // namespace

FileFacts Inspect(const fs::path& path) {
    FileFacts facts;
    facts.path = Common::FS::PathToUTF8String(path);
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (ec || !fs::exists(status)) {
        return facts;
    }
    facts.exists = true;
    facts.directory = fs::is_directory(status);
    if (!facts.directory) {
        const auto size = fs::file_size(path, ec);
        facts.size = ec ? 0 : static_cast<u64>(size);
    }
    const auto time = fs::last_write_time(path, ec);
    if (!ec) {
        facts.modified = Common::Log::Retention::ToTimeT(time);
    }
    return facts;
}

std::string KindOf(const FileFacts& facts) {
    if (facts.directory) {
        return "directory";
    }
    const std::size_t separator = facts.path.find_last_of("/\\");
    const std::size_t dot = facts.path.find_last_of('.');
    if (dot == std::string::npos || (separator != std::string::npos && dot < separator) ||
        dot + 1 == facts.path.size()) {
        return "unknown";
    }
    std::string extension = facts.path.substr(dot + 1);
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

AddonSummary SummarizeAddons(std::span<const std::string> labels) {
    AddonSummary summary;
    for (const std::string& label : labels) {
        // type|program_id|numeric_version|name|version, where name or version may hold a '|'.
        std::array<std::size_t, 3> bars{};
        std::size_t from = 0;
        bool shaped = true;
        for (std::size_t& bar : bars) {
            bar = label.find('|', from);
            if (bar == std::string::npos) {
                shaped = false;
                break;
            }
            from = bar + 1;
        }
        if (!shaped || bars[0] != 1 || (label[0] != '0' && label[0] != '1' && label[0] != '2')) {
            continue;
        }
        const std::string_view rest = std::string_view{label}.substr(bars[2] + 1);
        const std::size_t split = rest.rfind('|');
        const std::string_view name = split == std::string_view::npos ? rest : rest.substr(0, split);
        const std::string_view version =
            split == std::string_view::npos ? std::string_view{} : rest.substr(split + 1);
        const char* type = "update";
        switch (label[0]) {
        case '0':
            ++summary.updates;
            break;
        case '1':
            ++summary.dlcs;
            type = "dlc";
            break;
        default:
            ++summary.mods;
            type = "mod";
            break;
        }
        summary.entries.push_back(version.empty()
                                      ? fmt::format("{} \"{}\"", type, name)
                                      : fmt::format("{} \"{}\" v{}", type, name, version));
    }
    return summary;
}

std::string FormatSize(u64 bytes) {
    constexpr double MiB = 1024.0 * 1024.0;
    if (bytes >= (u64{1} << 30)) {
        return fmt::format("{:.2f} GiB", static_cast<double>(bytes) / (MiB * 1024.0));
    }
    if (bytes >= (u64{1} << 20)) {
        return fmt::format("{:.1f} MiB", static_cast<double>(bytes) / MiB);
    }
    return fmt::format("{} B", bytes);
}

std::string FormatIdentity(const FileFacts& facts, u64 title_id, u64 game_version,
                           const AddonSummary& addons, u64 content_stamp) {
    std::string line = fmt::format("Game file: title={:016X} version={} kind={} path=\"{}\"",
                                   title_id, game_version, KindOf(facts), facts.path);
    if (!facts.exists) {
        line += " exists=no";
    } else {
        if (!facts.directory) {
            line += fmt::format(" size={} ({} bytes)", FormatSize(facts.size), facts.size);
        }
        if (facts.modified) {
            line += fmt::format(" modified={}", Common::Log::Retention::LocalDateTime(*facts.modified));
        }
    }
    line += fmt::format(" addons={} ({} update, {} dlc, {} mod", addons.entries.size(),
                        addons.updates, addons.dlcs, addons.mods);
    if (!addons.entries.empty()) {
        line += ": ";
        const std::size_t shown = std::min(addons.entries.size(), MaxListedAddons);
        for (std::size_t i = 0; i < shown; ++i) {
            line += i == 0 ? "" : ", ";
            line += addons.entries[i];
        }
        if (shown < addons.entries.size()) {
            line += fmt::format(", +{} more", addons.entries.size() - shown);
        }
    }
    line += fmt::format(") content={:016x}", content_stamp);
    return line;
}

} // namespace FileSys::GameFileIdentity
