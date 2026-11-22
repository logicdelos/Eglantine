// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/log_retention.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <system_error>

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

} // namespace Common::Log::Retention
