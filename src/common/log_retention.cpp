// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/log_retention.h"

#include "common/env_gate.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef COMMON_HAS_ZLIB
#include <zlib.h>
#endif
#include <zstd.h>

namespace Common::Log::Retention {

namespace fs = std::filesystem;

namespace {

constexpr int CompressionLevel = 6;
constexpr std::string_view PartSuffix = ".zst.part";

struct Item {
    ArchiveName archive;
    std::string name;
};

bool IsDigits(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

/// Newest first.
bool Newer(const Item& a, const Item& b) {
    if (a.archive.stamp != b.archive.stamp) {
        return a.archive.stamp > b.archive.stamp;
    }
    if (a.archive.sequence != b.archive.sequence) {
        return a.archive.sequence > b.archive.sequence;
    }
    return a.name > b.name;
}

std::vector<Item> ParseAll(std::string_view stem, std::span<const std::string> filenames) {
    std::vector<Item> items;
    for (const std::string& name : filenames) {
        if (auto archive = ParseArchiveName(stem, name)) {
            items.push_back({std::move(*archive), name});
        }
    }
    std::ranges::sort(items, Newer);
    return items;
}

std::vector<std::string> ListNames(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
        names.push_back(it->path().filename().string());
    }
    return names;
}

/// A name for `stamp` that no plain or compressed archive of `stem` has yet.
std::optional<fs::path> FreeArchivePath(const fs::path& dir, const std::string& stem,
                                        const std::string& stamp) {
    for (u32 sequence = 0; sequence < 1000; ++sequence) {
        ArchiveName name{stamp, sequence, false};
        const fs::path plain = dir / MakeArchiveName(stem, name);
        name.compressed = true;
        const fs::path packed = dir / MakeArchiveName(stem, name);
        std::error_code ec;
        if (!fs::exists(plain, ec) && !ec && !fs::exists(packed, ec) && !ec) {
            return plain;
        }
    }
    return std::nullopt;
}

std::string StampOf(const fs::path& file) {
    std::error_code ec;
    const auto time = fs::last_write_time(file, ec);
    return LocalStamp(ec ? std::time(nullptr) : ToTimeT(time));
}

bool CompressFile(const fs::path& source, const fs::path& target, std::stop_token stop) {
    std::ifstream in{source, std::ios::binary};
    std::ofstream out{target, std::ios::binary | std::ios::trunc};
    if (!in || !out) {
        return false;
    }
    const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context{ZSTD_createCCtx(),
                                                                       &ZSTD_freeCCtx};
    if (!context ||
        ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel,
                                            CompressionLevel)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 1))) {
        return false;
    }
    std::vector<char> in_buffer(ZSTD_CStreamInSize());
    std::vector<char> out_buffer(ZSTD_CStreamOutSize());
    for (;;) {
        if (stop.stop_requested()) {
            return false;
        }
        in.read(in_buffer.data(), static_cast<std::streamsize>(in_buffer.size()));
        if (in.bad()) {
            return false;
        }
        const auto read = static_cast<std::size_t>(in.gcount());
        const bool last = read < in_buffer.size();
        ZSTD_inBuffer input{in_buffer.data(), read, 0};
        bool finished = false;
        do {
            ZSTD_outBuffer output{out_buffer.data(), out_buffer.size(), 0};
            const std::size_t remaining = ZSTD_compressStream2(
                context.get(), &output, &input, last ? ZSTD_e_end : ZSTD_e_continue);
            if (ZSTD_isError(remaining)) {
                return false;
            }
            out.write(out_buffer.data(), static_cast<std::streamsize>(output.pos));
            finished = last ? remaining == 0 : input.pos == input.size;
        } while (!finished);
        if (last) {
            break;
        }
    }
    out.flush();
    return static_cast<bool>(out);
}

/// Anything at or past this does not fit a 32-bit zip field (0xFFFFFFFF marks zip64).
constexpr u64 ZipLimit = 0xFFFFFFFFull;

std::time_t ModifiedTime(const fs::path& file) {
    std::error_code ec;
    const auto time = fs::last_write_time(file, ec);
    return ec ? std::time(nullptr) : ToTimeT(time);
}

std::optional<std::time_t> CreationTime([[maybe_unused]] const fs::path& file) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }
    constexpr u64 UnixEpochTicks = 116444736000000000ull; // 1601 to 1970, in 100 ns
    const u64 ticks = (u64{data.ftCreationTime.dwHighDateTime} << 32) |
                      u64{data.ftCreationTime.dwLowDateTime};
    if (ticks < UnixEpochTicks) {
        return std::nullopt;
    }
    return static_cast<std::time_t>((ticks - UnixEpochTicks) / 10000000ull);
#else
    return std::nullopt;
#endif
}

#ifdef COMMON_HAS_ZLIB
constexpr u64 ZipLocalHeader = 30;
constexpr u64 ZipCentralHeader = 46;
constexpr u64 ZipEndRecord = 22;
constexpr std::size_t ZipChunk = 256 * 1024;

struct ZipRecord {
    std::string name;
    u32 crc{};
    u32 compressed{};
    u32 size{};
    u32 offset{};
    u16 dos_time{};
    u16 dos_date{};
};

struct DeflateEnd {
    void operator()(z_stream* stream) const noexcept {
        deflateEnd(stream);
    }
};

void Put16(std::string& out, u32 value) {
    out += static_cast<char>(value & 0xff);
    out += static_cast<char>((value >> 8) & 0xff);
}

void Put32(std::string& out, u32 value) {
    Put16(out, value & 0xffff);
    Put16(out, value >> 16);
}

void DosStamp(std::time_t time, u16& dos_date, u16& dos_time) {
    std::tm local{};
#ifdef _WIN32
    const bool ok = localtime_s(&local, &time) == 0;
#else
    const bool ok = localtime_r(&time, &local) != nullptr;
#endif
    if (!ok || local.tm_year < 80) {
        dos_date = (1 << 5) | 1; // 1980-01-01
        dos_time = 0;
        return;
    }
    dos_time = static_cast<u16>((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
    dos_date = static_cast<u16>(((std::min)(local.tm_year - 80, 127) << 9) |
                                ((local.tm_mon + 1) << 5) | local.tm_mday);
}

/// One deflated entry: a local header with the CRC and the compressed size left at zero, the
/// data streamed in chunks, then the two fields written over.
bool AddZipEntry(std::ofstream& out, const ZipInput& input, u64& offset,
                 std::vector<ZipRecord>& records, std::vector<char>& in_buffer,
                 std::vector<char>& out_buffer, std::stop_token stop) {
    const u64 header_size = ZipLocalHeader + input.name.size();
    if (input.name.empty() || input.name.size() > 0xFFFF || input.size >= ZipLimit ||
        records.size() >= 0xFFFF || offset + header_size >= ZipLimit) {
        return false;
    }
    std::ifstream in{input.path, std::ios::binary};
    if (!in) {
        return false;
    }
    ZipRecord record;
    record.name = input.name;
    record.size = static_cast<u32>(input.size);
    record.offset = static_cast<u32>(offset);
    DosStamp(input.modified, record.dos_date, record.dos_time);

    std::string header;
    Put32(header, 0x04034b50);
    Put16(header, 20);
    Put16(header, 0);
    Put16(header, 8);
    Put16(header, record.dos_time);
    Put16(header, record.dos_date);
    Put32(header, 0);
    Put32(header, 0);
    Put32(header, record.size);
    Put16(header, static_cast<u32>(input.name.size()));
    Put16(header, 0);
    header += input.name;
    out.write(header.data(), static_cast<std::streamsize>(header.size()));

    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    const std::unique_ptr<z_stream, DeflateEnd> guard{&stream};
    uLong crc = crc32_z(0, nullptr, 0);
    u64 remaining = input.size;
    u64 compressed = 0;
    for (;;) {
        if (stop.stop_requested()) {
            return false;
        }
        const auto want = static_cast<std::size_t>((std::min<u64>)(remaining, in_buffer.size()));
        in.read(in_buffer.data(), static_cast<std::streamsize>(want));
        if (static_cast<std::size_t>(in.gcount()) != want) {
            return false; // the file got shorter than its snapshot
        }
        remaining -= want;
        const bool last = remaining == 0;
        crc = crc32_z(crc, reinterpret_cast<const Bytef*>(in_buffer.data()), want);
        stream.next_in = reinterpret_cast<Bytef*>(in_buffer.data());
        stream.avail_in = static_cast<uInt>(want);
        int result = Z_OK;
        do {
            stream.next_out = reinterpret_cast<Bytef*>(out_buffer.data());
            stream.avail_out = static_cast<uInt>(out_buffer.size());
            result = deflate(&stream, last ? Z_FINISH : Z_NO_FLUSH);
            if (result == Z_STREAM_ERROR) {
                return false;
            }
            const std::size_t produced = out_buffer.size() - stream.avail_out;
            out.write(out_buffer.data(), static_cast<std::streamsize>(produced));
            compressed += produced;
            if (offset + header_size + compressed >= ZipLimit || !out) {
                return false;
            }
        } while (stream.avail_out == 0);
        if (last) {
            if (result != Z_STREAM_END) {
                return false;
            }
            break;
        }
    }
    record.crc = static_cast<u32>(crc);
    record.compressed = static_cast<u32>(compressed);

    std::string patch;
    Put32(patch, record.crc);
    Put32(patch, record.compressed);
    out.seekp(static_cast<std::streamoff>(offset + 14));
    out.write(patch.data(), static_cast<std::streamsize>(patch.size()));
    out.seekp(0, std::ios::end);
    if (!out) {
        return false;
    }
    offset += header_size + compressed;
    records.push_back(std::move(record));
    return true;
}

bool FinishZip(std::ofstream& out, u64 offset, const std::vector<ZipRecord>& records) {
    std::string central;
    for (const ZipRecord& record : records) {
        Put32(central, 0x02014b50);
        Put16(central, 20);
        Put16(central, 20);
        Put16(central, 0);
        Put16(central, 8);
        Put16(central, record.dos_time);
        Put16(central, record.dos_date);
        Put32(central, record.crc);
        Put32(central, record.compressed);
        Put32(central, record.size);
        Put16(central, static_cast<u32>(record.name.size()));
        Put16(central, 0); // extra
        Put16(central, 0); // comment
        Put16(central, 0); // disk number
        Put16(central, 0); // internal attributes
        Put32(central, 0); // external attributes
        Put32(central, record.offset);
        central += record.name;
    }
    if (offset + central.size() + ZipEndRecord >= ZipLimit) {
        return false;
    }
    const auto count = static_cast<u32>(records.size());
    const auto central_size = static_cast<u32>(central.size());
    Put32(central, 0x06054b50);
    Put16(central, 0);
    Put16(central, 0);
    Put16(central, count);
    Put16(central, count);
    Put32(central, central_size);
    Put32(central, static_cast<u32>(offset));
    Put16(central, 0);
    out.write(central.data(), static_cast<std::streamsize>(central.size()));
    out.flush();
    return static_cast<bool>(out);
}
#endif

} // Anonymous namespace

std::optional<ArchiveName> ParseArchiveName(std::string_view stem, std::string_view filename) {
    if (!filename.starts_with(stem)) {
        return std::nullopt;
    }
    std::string_view rest = filename.substr(stem.size());
    if (rest.size() < 1 + 15 || rest[0] != '.') {
        return std::nullopt;
    }
    rest.remove_prefix(1);
    const std::string_view date = rest.substr(0, 8);
    const std::string_view time = rest.substr(9, 6);
    if (!IsDigits(date) || rest[8] != '-' || !IsDigits(time)) {
        return std::nullopt;
    }
    ArchiveName name;
    name.stamp = std::string{rest.substr(0, 15)};
    rest.remove_prefix(15);
    if (rest.starts_with('-')) {
        rest.remove_prefix(1);
        std::size_t digits = 0;
        while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') {
            ++digits;
        }
        if (digits == 0 || digits > 6) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < digits; ++i) {
            name.sequence = name.sequence * 10 + static_cast<u32>(rest[i] - '0');
        }
        rest.remove_prefix(digits);
    }
    if (rest == ".txt") {
        return name;
    }
    if (rest == ".txt.zst") {
        name.compressed = true;
        return name;
    }
    return std::nullopt;
}

std::string MakeArchiveName(std::string_view stem, const ArchiveName& name) {
    std::string text{stem};
    text += '.';
    text += name.stamp;
    if (name.sequence != 0) {
        text += '-';
        text += std::to_string(name.sequence);
    }
    text += ".txt";
    if (name.compressed) {
        text += ".zst";
    }
    return text;
}

std::vector<std::string> SelectForDeletion(std::string_view stem,
                                           std::span<const std::string> filenames,
                                           std::size_t keep) {
    std::vector<std::string> doomed;
    const std::string prefix = std::string{stem} + '.';
    for (const std::string& name : filenames) {
        if (name.starts_with(prefix) && name.ends_with(PartSuffix)) {
            doomed.push_back(name);
        }
    }
    const std::vector<Item> items = ParseAll(stem, filenames);
    for (std::size_t i = keep; i < items.size(); ++i) {
        doomed.push_back(items[i].name);
    }
    return doomed;
}

std::vector<std::string> SelectForCompression(std::string_view stem,
                                              std::span<const std::string> filenames) {
    std::vector<std::string> names;
    const std::vector<Item> items = ParseAll(stem, filenames);
    for (std::size_t i = 1; i < items.size(); ++i) {
        if (!items[i].archive.compressed) {
            names.push_back(items[i].name);
        }
    }
    return names;
}

std::vector<std::string> CensoredNames(std::span<const std::string_view> candidates) {
    std::vector<std::string> names;
    for (const std::string_view candidate : candidates) {
        if (!candidate.empty() && std::ranges::find(names, candidate) == names.end()) {
            names.emplace_back(candidate);
        }
    }
    std::ranges::stable_sort(names, [](const std::string& a, const std::string& b) {
        return a.size() > b.size();
    });
    return names;
}

std::string_view PathLeaf(std::string_view path) noexcept {
    constexpr std::string_view separators = "/\\";
    const std::size_t last = path.find_last_not_of(separators);
    if (last == std::string_view::npos) {
        return {};
    }
    path = path.substr(0, last + 1);
    const std::size_t separator = path.find_last_of(separators);
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

namespace {
std::string FormatLocal(std::time_t time, const char* format, const char* fallback) {
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time) != 0) {
        return fallback;
    }
#else
    if (localtime_r(&time, &local) == nullptr) {
        return fallback;
    }
#endif
    std::array<char, 32> buffer{};
    const std::size_t length = std::strftime(buffer.data(), buffer.size(), format, &local);
    return std::string{buffer.data(), length};
}
} // namespace

std::string LocalStamp(std::time_t time) {
    return FormatLocal(time, "%Y%m%d-%H%M%S", "00000000-000000");
}

std::string LocalDateTime(std::time_t time) {
    return FormatLocal(time, "%Y-%m-%d %H:%M:%S", "unknown");
}

std::time_t ToTimeT(fs::file_time_type time) {
    const auto offset = time - fs::file_time_type::clock::now();
    const auto system_time = std::chrono::system_clock::now() + offset;
    return std::chrono::system_clock::to_time_t(
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(system_time));
}

RotateResult Rotate(const fs::path& log, std::size_t keep) noexcept {
    RotateResult result;
    try {
        const fs::path dir = log.parent_path();
        const std::string stem = log.stem().string();
        std::error_code ec;

        // An archive of the previous scheme becomes one of the new.
        fs::path legacy = log;
        legacy += ".old.txt";
        if (fs::exists(legacy, ec) && !ec) {
            if (const auto target = FreeArchivePath(dir, stem, StampOf(legacy))) {
                fs::rename(legacy, *target, ec);
                if (!ec) {
                    result.migrated = *target;
                }
            }
        }

        const bool exists = fs::exists(log, ec);
        if (ec) {
            return result;
        }
        if (exists) {
            const auto size = fs::file_size(log, ec);
            if (ec || size != 0) {
                const auto target = FreeArchivePath(dir, stem, StampOf(log));
                if (!target) {
                    return result;
                }
                fs::rename(log, *target, ec);
                if (ec) {
                    return result;
                }
                result.archived = *target;
            }
        }
        result.ok = true;

        const std::vector<std::string> names = ListNames(dir);
        for (const std::string& name : SelectForDeletion(stem, names, keep)) {
            const fs::path path = dir / name;
            fs::remove(path, ec);
            if (!ec) {
                result.deleted.push_back(path);
            }
        }
    } catch (...) {
        // Retention is housekeeping: a failure to do it must not stop the log being written.
    }
    return result;
}

std::size_t CompressOlderArchives(const fs::path& log, std::stop_token stop) noexcept {
    std::size_t compressed = 0;
    try {
        const fs::path dir = log.parent_path();
        const std::string stem = log.stem().string();
        for (const std::string& name : SelectForCompression(stem, ListNames(dir))) {
            if (stop.stop_requested()) {
                break;
            }
            const fs::path source = dir / name;
            fs::path target = source;
            target += ".zst";
            fs::path part = target;
            part += ".part";
            std::error_code ec;
            const auto modified = fs::last_write_time(source, ec);
            const bool have_time = !ec;
            if (!CompressFile(source, part, stop)) {
                fs::remove(part, ec);
                continue;
            }
            fs::rename(part, target, ec);
            if (ec) {
                fs::remove(part, ec);
                continue;
            }
            if (have_time) {
                fs::last_write_time(target, modified, ec);
            }
            fs::remove(source, ec);
            ++compressed;
        }
    } catch (...) {
        // Compression is housekeeping too; the plain archive stays if it did not work.
    }
    return compressed;
}

bool SessionZipEnabled() noexcept {
    return EnvGate("EDEN_NX2_SESSION_ZIP");
}

std::optional<std::string> SelectArchiveForCrashZip(
    std::string_view stem, std::span<const FileTime> files,
    std::optional<fs::file_time_type> session_zip_modified,
    std::optional<fs::file_time_type> crash_zip_modified) {
    std::vector<std::string> names;
    names.reserve(files.size());
    for (const FileTime& file : files) {
        names.push_back(file.name);
    }
    const std::vector<Item> items = ParseAll(stem, names);
    if (items.empty() || items.front().archive.compressed) {
        return std::nullopt;
    }
    const std::string& newest = items.front().name;
    const auto found = std::ranges::find(files, newest, &FileTime::name);
    if (found == files.end()) {
        return std::nullopt;
    }
    // A clean exit wrote the session zip after the last line of the log; without that, or with
    // a zip from an earlier session, this one ended abnormally.
    if (session_zip_modified && !(found->modified > *session_zip_modified)) {
        return std::nullopt;
    }
    if (crash_zip_modified && !(found->modified > *crash_zip_modified)) {
        return std::nullopt;
    }
    return newest;
}

bool DumpBelongsToSession(std::time_t dump_modified, std::optional<std::time_t> session_start) {
    return session_start && dump_modified >= *session_start;
}

std::time_t ArchiveSessionStart(std::optional<std::time_t> created, std::time_t last_write) {
    const std::time_t earliest = last_write - static_cast<std::time_t>(CrashDumpWindow.count());
    return created ? (std::max)(*created, earliest) : earliest;
}

bool WriteZip(const fs::path& target, std::span<const ZipInput> inputs,
              std::stop_token stop) noexcept {
#ifdef COMMON_HAS_ZLIB
    fs::path part = target;
    part += ".part";
    try {
        bool ok = false;
        if (!inputs.empty()) {
            std::ofstream out{part, std::ios::binary | std::ios::trunc};
            if (out) {
                std::vector<char> in_buffer(ZipChunk);
                std::vector<char> out_buffer(ZipChunk);
                std::vector<ZipRecord> records;
                u64 offset = 0;
                ok = true;
                for (const ZipInput& input : inputs) {
                    if (!AddZipEntry(out, input, offset, records, in_buffer, out_buffer, stop)) {
                        ok = false;
                        break;
                    }
                }
                ok = ok && FinishZip(out, offset, records);
                out.close();
                ok = ok && !out.fail();
            }
        }
        std::error_code ec;
        if (ok) {
            fs::rename(part, target, ec);
            ok = !ec;
        }
        if (!ok) {
            fs::remove(part, ec);
        }
        return ok;
    } catch (...) {
        std::error_code ec;
        fs::remove(part, ec);
        return false;
    }
#else
    // Only the static room build has no zlib, and it never writes a session zip.
    return false;
#endif
}

namespace {

bool WriteLogZip(const fs::path& log, std::string_view zip_name,
                 std::optional<std::time_t> session_start, std::stop_token stop) noexcept {
    try {
        // The size is taken from an open handle once: the file may be open for writing in this
        // process, and a path lookup can lag behind what it has written.
        std::ifstream probe{log, std::ios::binary};
        if (!probe) {
            return false;
        }
        probe.seekg(0, std::ios::end);
        const std::streamoff end = probe.tellg();
        if (!probe || end < 0) {
            return false;
        }
        probe.close();

        std::vector<ZipInput> inputs;
        inputs.push_back({log, std::string{SessionZipLogEntry}, static_cast<u64>(end),
                          ModifiedTime(log)});
        const fs::path dir = log.parent_path();
        const fs::path dump = dir / fs::path{CrashDumpName};
        std::error_code ec;
        if (fs::is_regular_file(dump, ec) && !ec) {
            const u64 dump_size = fs::file_size(dump, ec);
            const std::time_t dump_time = ModifiedTime(dump);
            if (!ec && dump_size < ZipLimit && DumpBelongsToSession(dump_time, session_start)) {
                inputs.push_back({dump, std::string{CrashDumpName}, dump_size, dump_time});
            }
        }
        return WriteZip(dir / fs::path{zip_name}, inputs, stop);
    } catch (...) {
        return false;
    }
}

} // namespace

bool WriteSessionZip(const fs::path& log, std::optional<std::time_t> session_start,
                     std::stop_token stop) noexcept {
    return WriteLogZip(log, SessionZipName, session_start, stop);
}

bool WriteCrashZipFromArchive(const fs::path& log, std::stop_token stop) noexcept {
    try {
        const fs::path dir = log.parent_path();
        const std::string stem = log.stem().string();
        std::vector<FileTime> files;
        std::error_code ec;
        for (fs::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
            std::error_code time_ec;
            const auto modified = it->last_write_time(time_ec);
            if (!time_ec) {
                files.push_back({it->path().filename().string(), modified});
            }
        }
        std::optional<fs::file_time_type> session_zip_modified;
        const auto session_zip_time = fs::last_write_time(dir / fs::path{SessionZipName}, ec);
        if (!ec) {
            session_zip_modified = session_zip_time;
        }
        std::optional<fs::file_time_type> crash_zip_modified;
        const auto crash_zip_time = fs::last_write_time(dir / fs::path{CrashZipName}, ec);
        if (!ec) {
            crash_zip_modified = crash_zip_time;
        }
        const auto archive_name =
            SelectArchiveForCrashZip(stem, files, session_zip_modified, crash_zip_modified);
        if (!archive_name) {
            return false;
        }
        const fs::path archive = dir / *archive_name;
        const std::time_t last_write = ModifiedTime(archive);
        return WriteLogZip(archive, CrashZipName,
                           ArchiveSessionStart(CreationTime(archive), last_write), stop);
    } catch (...) {
        return false;
    }
}

} // namespace Common::Log::Retention
