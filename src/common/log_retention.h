// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// Session log retention. eden_log.txt is always the current session. At the start of the next
// session it is renamed to eden_log.<YYYYMMDD-HHMMSS>.txt (the time of its last write, local
// time), and only the newest KeepArchives of those are kept. Every archive but the newest is then
// compressed in the background to eden_log.<stamp>.txt.zst, so that the previous session stays
// plain text for a grep.
namespace Common::Log::Retention {

/// Archives kept besides the current log.
inline constexpr std::size_t KeepArchives = 5;

struct ArchiveName {
    std::string stamp; ///< YYYYMMDD-HHMMSS
    u32 sequence{};    ///< 0, or N for "<stamp>-N" when two logs ended in the same second
    bool compressed{};
};

/// `eden_log.20261122-101500.txt`, `eden_log.20261122-101500-2.txt` and the same with `.zst`;
/// nothing for any other name (including the current log and a legacy `.old.txt`).
[[nodiscard]] std::optional<ArchiveName> ParseArchiveName(std::string_view stem,
                                                          std::string_view filename);

[[nodiscard]] std::string MakeArchiveName(std::string_view stem, const ArchiveName& name);

/// The file names (of `filenames`, those of one directory) to delete so that the `keep` newest
/// archives of `stem` remain, plus any unfinished `.zst.part` of that stem.
[[nodiscard]] std::vector<std::string> SelectForDeletion(std::string_view stem,
                                                         std::span<const std::string> filenames,
                                                         std::size_t keep);

/// The names of the archives of `stem` that should still be compressed: every plain archive
/// except the newest one of all.
[[nodiscard]] std::vector<std::string> SelectForCompression(std::string_view stem,
                                                            std::span<const std::string> filenames);

/// `YYYYMMDD-HHMMSS` of a time in the local time zone.
[[nodiscard]] std::string LocalStamp(std::time_t time);
/// `YYYY-MM-DD HH:MM:SS` of a time in the local time zone.
[[nodiscard]] std::string LocalDateTime(std::time_t time);
[[nodiscard]] std::time_t ToTimeT(std::filesystem::file_time_type time);

struct RotateResult {
    /// The log was moved (or was empty or absent) and the path is free to be written; false when
    /// it could not be moved, e.g. because it is still open in another process.
    bool ok{};
    /// Where the log went, when it was archived.
    std::optional<std::filesystem::path> archived;
    /// An archive from the previous scheme (`<log>.old.txt`) that was renamed into the new one.
    std::optional<std::filesystem::path> migrated;
    /// Archives deleted to stay within the limit.
    std::vector<std::filesystem::path> deleted;
};

/// Archives `log` as described above (an empty log is simply overwritten), migrates a legacy
/// `<log>.old.txt` into an archive, and prunes to `keep`. Never throws.
RotateResult Rotate(const std::filesystem::path& log, std::size_t keep = KeepArchives) noexcept;

/// Compresses the plain archives of `log` that SelectForCompression names, one at a time, to
/// `<name>.zst` (written as `.zst.part` and renamed, the plain file removed last), checking
/// `stop` between blocks. Returns how many were compressed. Never throws.
std::size_t CompressOlderArchives(const std::filesystem::path& log, std::stop_token stop) noexcept;

} // namespace Common::Log::Retention
