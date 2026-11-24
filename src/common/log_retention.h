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

/// The strings to replace when a log line is censored: the distinct non-empty ones of
/// `candidates` (an unset or empty name is allowed), longest first, so that a name is replaced
/// before a shorter one that is its prefix. Case-sensitive.
[[nodiscard]] std::vector<std::string> CensoredNames(std::span<const std::string_view> candidates);

/// The last component of a path, with `/` or `\` as separator and trailing separators ignored;
/// empty for an empty path or one of separators only. A view into `path`.
[[nodiscard]] std::string_view PathLeaf(std::string_view path) noexcept;

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

// The last session's log as a zip. GitHub accepts .zip (up to 25 MB) for an issue attachment and
// does not accept .zst or a 100 MiB text file. Two files, each always the same name and replaced
// every time:
// - eglantine-last-session.zip: a clean exit writes it from the live log.
// - eglantine-last-crash.zip: the next start writes it from the rotated log when the previous
//   session did not end with a clean exit (the newest archive is newer than the session zip, or
//   there is no session zip). A later clean exit never touches it, so a normal session after a
//   crash does not replace the evidence.
// EDEN_NX2_SESSION_ZIP=0 turns both off.

inline constexpr std::string_view SessionZipName = "eglantine-last-session.zip";
inline constexpr std::string_view CrashZipName = "eglantine-last-crash.zip";
/// The name of the log inside the zip, whichever file it was read from.
inline constexpr std::string_view SessionZipLogEntry = "eden_log.txt";
/// Written next to the log by common/windows/crash_handler.cpp.
inline constexpr std::string_view CrashDumpName = "eden_crash.dmp";
/// A crash dump is written within seconds of the last line of the crashed session's log (the
/// watchdog ends the process 15 s after the crash), so a dump older than this before the end of
/// a log is from an earlier session.
inline constexpr std::chrono::seconds CrashDumpWindow{60};

/// False when the environment variable EDEN_NX2_SESSION_ZIP is "0".
[[nodiscard]] bool SessionZipEnabled() noexcept;

struct FileTime {
    std::string name;
    std::filesystem::file_time_type modified;
};

/// The archive of `stem` (a name of `files`, those of one directory) to write the crash zip
/// from, when that is needed: the newest archive, if it is plain text, the previous session
/// ended abnormally (the session zip is missing, `session_zip_modified` empty, or older than the
/// archive) and the crash zip is missing (`crash_zip_modified` empty) or older than the archive.
/// Nothing otherwise.
[[nodiscard]] std::optional<std::string> SelectArchiveForCrashZip(
    std::string_view stem, std::span<const FileTime> files,
    std::optional<std::filesystem::file_time_type> session_zip_modified,
    std::optional<std::filesystem::file_time_type> crash_zip_modified);

/// Whether a crash dump last written at `dump_modified` is from the session that began at
/// `session_start`. Without a start time it is not attached.
[[nodiscard]] bool DumpBelongsToSession(std::time_t dump_modified,
                                        std::optional<std::time_t> session_start);

/// When the session that wrote an archive began, as far as the files say: the creation time of
/// the file, but not earlier than CrashDumpWindow before its last write. The creation time alone
/// is not trusted, since Windows can give a log the creation time of the file that was renamed
/// away just before it was created.
[[nodiscard]] std::time_t ArchiveSessionStart(std::optional<std::time_t> created,
                                              std::time_t last_write);

struct ZipInput {
    std::filesystem::path path;
    std::string name;       ///< name of the entry
    u64 size{};             ///< bytes read from the start of the file; the entry holds no more
    std::time_t modified{}; ///< the time stored with the entry (local time)
};

/// Writes `inputs` as deflate-compressed entries of a zip to `<target>.part` and renames it over
/// `target`. Every file is streamed in chunks and read up to `size` bytes only, so a file that is
/// still growing yields exactly the snapshot; a file that is shorter than that fails. No zip64:
/// an entry, an offset or the entry count that does not fit fails, as does a stop request.
/// Returns false (and leaves no `.part`) on any failure. Never throws.
[[nodiscard]] bool WriteZip(const std::filesystem::path& target, std::span<const ZipInput> inputs,
                            std::stop_token stop) noexcept;

/// Writes `<dir of log>/eglantine-last-session.zip` with `log` as `eden_log.txt` and, when
/// `session_start` says it is from the same session, the crash dump next to it. The size of `log`
/// is taken once; it may be open for writing in this process. Never throws.
bool WriteSessionZip(const std::filesystem::path& log, std::optional<std::time_t> session_start,
                     std::stop_token stop) noexcept;

/// The start-up counterpart: writes `<dir of log>/eglantine-last-crash.zip` from the newest
/// archive of `log` (same contents as WriteSessionZip) when SelectArchiveForCrashZip asks for
/// it. Returns whether it wrote the zip. Never throws.
bool WriteCrashZipFromArchive(const std::filesystem::path& log, std::stop_token stop) noexcept;

} // namespace Common::Log::Retention
