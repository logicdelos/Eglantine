// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stop_token>
#include <string>
#include <vector>
#include <cstdint>
#include <ctime>
#include <optional>

#include <catch2/catch_test_macros.hpp>
#include <zlib.h>

#include "common/log_retention.h"

namespace {

namespace fs = std::filesystem;
namespace Retention = Common::Log::Retention;

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("log-retention-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Write(const fs::path& file, const std::string& text, int hours_ago) {
    {
        std::ofstream out{file, std::ios::binary | std::ios::trunc};
        out << text;
    }
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::hours{hours_ago});
}

std::set<std::string> Names(const fs::path& dir) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) {
        names.insert(entry.path().filename().string());
    }
    return names;
}

std::string Read(const fs::path& file) {
    std::ifstream in{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("Session log archive names", "[common][log_retention]") {
    const auto plain = Retention::ParseArchiveName("eden_log", "eden_log.20261122-101500.txt");
    REQUIRE(plain);
    REQUIRE(plain->stamp == "20261122-101500");
    REQUIRE(plain->sequence == 0);
    REQUIRE_FALSE(plain->compressed);

    const auto numbered =
        Retention::ParseArchiveName("eden_log", "eden_log.20261122-101500-12.txt.zst");
    REQUIRE(numbered);
    REQUIRE(numbered->sequence == 12);
    REQUIRE(numbered->compressed);
    REQUIRE(Retention::MakeArchiveName("eden_log", *numbered) ==
            "eden_log.20261122-101500-12.txt.zst");

    REQUIRE_FALSE(Retention::ParseArchiveName("eden_log", "eden_log.txt"));
    REQUIRE_FALSE(Retention::ParseArchiveName("eden_log", "eden_log.txt.old.txt"));
    REQUIRE_FALSE(Retention::ParseArchiveName("eden_log", "eden_log_2.20261122-101500.txt"));
    REQUIRE_FALSE(Retention::ParseArchiveName("eden_log", "eden_log.20261122-101500.txt.zst.part"));
    REQUIRE_FALSE(Retention::ParseArchiveName("eden_log", "eden_log.2026-11-22.txt"));
    REQUIRE_FALSE(Retention::ParseArchiveName("eden_log", "eden_log.20261122-101500-.txt"));
}

TEST_CASE("Session log retention selects the oldest for deletion", "[common][log_retention]") {
    const std::vector<std::string> names{
        "eden_log.txt",
        "eden_log.20260101-000000.txt.zst",
        "eden_log.20260103-000000.txt.zst",
        "eden_log.20260102-000000.txt.zst",
        "eden_log.20260105-000000.txt",
        "eden_log.20260105-000000-1.txt",
        "eden_log.20260104-000000.txt.zst.part",
        "eden_log_2.20250101-000000.txt",
        "eden_gpu.log.old.txt",
    };
    auto doomed = Retention::SelectForDeletion("eden_log", names, 3);
    std::ranges::sort(doomed);
    REQUIRE(doomed == std::vector<std::string>{"eden_log.20260101-000000.txt.zst",
                                               "eden_log.20260102-000000.txt.zst",
                                               "eden_log.20260104-000000.txt.zst.part"});
    REQUIRE(Retention::SelectForDeletion("eden_log", names, 10) ==
            std::vector<std::string>{"eden_log.20260104-000000.txt.zst.part"});
}

TEST_CASE("Session log retention compresses all but the newest", "[common][log_retention]") {
    const std::vector<std::string> names{
        "eden_log.20260105-000000.txt",
        "eden_log.20260105-000000-1.txt",
        "eden_log.20260104-000000.txt",
        "eden_log.20260103-000000.txt.zst",
        "eden_log.20260102-000000.txt",
    };
    auto picked = Retention::SelectForCompression("eden_log", names);
    std::ranges::sort(picked);
    REQUIRE(picked == std::vector<std::string>{"eden_log.20260102-000000.txt",
                                               "eden_log.20260104-000000.txt",
                                               "eden_log.20260105-000000.txt"});
    REQUIRE(Retention::SelectForCompression("eden_log", {}).empty());
}

TEST_CASE("Session log rotation keeps the newest five archives", "[common][log_retention]") {
    TempDir dir;
    const fs::path log = dir.path / "eden_log.txt";
    for (int session = 0; session < 8; ++session) {
        Write(log, "session " + std::to_string(session), (9 - session) * 24);
        const auto result = Retention::Rotate(log);
        REQUIRE(result.ok);
        REQUIRE(result.archived);
        REQUIRE_FALSE(fs::exists(log));
    }
    const auto names = Names(dir.path);
    REQUIRE(names.size() == 5);
    std::vector<std::string> contents;
    for (const auto& name : names) {
        REQUIRE(Retention::ParseArchiveName("eden_log", name));
        contents.push_back(Read(dir.path / name));
    }
    // Sorted by stamp, which is by age: sessions 3..7 survive.
    REQUIRE(contents == std::vector<std::string>{"session 3", "session 4", "session 5",
                                                 "session 6", "session 7"});
}

TEST_CASE("Session log rotation edge cases", "[common][log_retention]") {
    TempDir dir;
    const fs::path log = dir.path / "eden_log.txt";

    SECTION("no log at all") {
        const auto result = Retention::Rotate(log);
        REQUIRE(result.ok);
        REQUIRE_FALSE(result.archived);
        REQUIRE(Names(dir.path).empty());
    }

    SECTION("an empty log is overwritten, not archived") {
        Write(log, "", 1);
        const auto result = Retention::Rotate(log);
        REQUIRE(result.ok);
        REQUIRE_FALSE(result.archived);
        REQUIRE(Names(dir.path) == std::set<std::string>{"eden_log.txt"});
    }

    SECTION("a legacy .old.txt becomes an archive") {
        Write(dir.path / "eden_log.txt.old.txt", "legacy", 48);
        Write(log, "current", 1);
        const auto result = Retention::Rotate(log);
        REQUIRE(result.ok);
        REQUIRE(result.migrated);
        REQUIRE(result.archived);
        REQUIRE(Read(*result.migrated) == "legacy");
        REQUIRE(Read(*result.archived) == "current");
        REQUIRE(Names(dir.path).size() == 2);
        REQUIRE_FALSE(fs::exists(dir.path / "eden_log.txt.old.txt"));
    }

    SECTION("two logs that ended in the same second do not overwrite each other") {
        const auto when = fs::file_time_type::clock::now() - std::chrono::hours{5};
        Write(log, "first", 0);
        fs::last_write_time(log, when);
        REQUIRE(Retention::Rotate(log).archived);
        Write(log, "second", 0);
        fs::last_write_time(log, when);
        REQUIRE(Retention::Rotate(log).archived);
        const auto names = Names(dir.path);
        REQUIRE(names.size() == 2);
        std::set<std::string> texts;
        for (const auto& name : names) {
            texts.insert(Read(dir.path / name));
        }
        REQUIRE(texts == std::set<std::string>{"first", "second"});
    }
}

TEST_CASE("Session log compression keeps the newest archive plain", "[common][log_retention]") {
    TempDir dir;
    const fs::path log = dir.path / "eden_log.txt";
    const std::string text(100000, 'x');
    for (int session = 0; session < 3; ++session) {
        Write(log, text + std::to_string(session), (4 - session) * 24);
        REQUIRE(Retention::Rotate(log).ok);
    }
    REQUIRE(Retention::CompressOlderArchives(log, std::stop_token{}) == 2);

    std::size_t plain = 0;
    std::size_t packed = 0;
    for (const auto& name : Names(dir.path)) {
        const auto parsed = Retention::ParseArchiveName("eden_log", name);
        REQUIRE(parsed);
        (parsed->compressed ? packed : plain) += 1;
        if (parsed->compressed) {
            REQUIRE(fs::file_size(dir.path / name) < 1000);
        }
    }
    REQUIRE(plain == 1);
    REQUIRE(packed == 2);
    REQUIRE(Retention::CompressOlderArchives(log, std::stop_token{}) == 0);
}

namespace {

struct ZipEntry {
    std::string name;
    std::string data;
    std::uint32_t crc{};
};

std::uint32_t Le(const std::string& bytes, std::size_t at, std::size_t width) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i])) << (8 * i);
    }
    return value;
}

std::uint32_t Crc(const std::string& data) {
    return static_cast<std::uint32_t>(crc32_z(crc32_z(0, nullptr, 0),
                                              reinterpret_cast<const Bytef*>(data.data()),
                                              data.size()));
}

/// Reads a zip the way an extractor does: the end record from the back, then the central
/// directory, then each local header and its deflate data, checking the fields that Explorer and
/// Python's zipfile rely on.
std::vector<ZipEntry> ReadZip(const fs::path& zip) {
    const std::string bytes = Read(zip);
    REQUIRE(bytes.size() >= 22);
    const std::size_t end = bytes.size() - 22;
    REQUIRE(Le(bytes, end, 4) == 0x06054b50);
    REQUIRE(Le(bytes, end + 4, 2) == 0); // this disk
    REQUIRE(Le(bytes, end + 6, 2) == 0); // disk of the central directory
    const std::size_t count = Le(bytes, end + 8, 2);
    REQUIRE(Le(bytes, end + 10, 2) == count);
    const std::size_t central_size = Le(bytes, end + 12, 4);
    const std::size_t central_offset = Le(bytes, end + 16, 4);
    REQUIRE(Le(bytes, end + 20, 2) == 0); // no comment
    REQUIRE(central_offset + central_size == end);

    std::vector<ZipEntry> entries;
    std::size_t at = central_offset;
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(Le(bytes, at, 4) == 0x02014b50);
        REQUIRE(Le(bytes, at + 10, 2) == 8); // deflate
        const std::uint32_t crc = Le(bytes, at + 16, 4);
        const std::size_t packed = Le(bytes, at + 20, 4);
        const std::size_t size = Le(bytes, at + 24, 4);
        const std::size_t name_length = Le(bytes, at + 28, 2);
        const std::size_t extra_length = Le(bytes, at + 30, 2);
        const std::size_t comment_length = Le(bytes, at + 32, 2);
        const std::size_t local = Le(bytes, at + 42, 4);
        ZipEntry entry;
        entry.name = bytes.substr(at + 46, name_length);
        entry.crc = crc;

        REQUIRE(Le(bytes, local, 4) == 0x04034b50);
        REQUIRE(Le(bytes, local + 8, 2) == 8);
        REQUIRE(Le(bytes, local + 14, 4) == crc);
        REQUIRE(Le(bytes, local + 18, 4) == packed);
        REQUIRE(Le(bytes, local + 22, 4) == size);
        REQUIRE(Le(bytes, local + 26, 2) == name_length);
        REQUIRE(bytes.substr(local + 30, name_length) == entry.name);
        const std::size_t data = local + 30 + name_length + Le(bytes, local + 28, 2);
        REQUIRE(data + packed <= central_offset);

        z_stream stream{};
        REQUIRE(inflateInit2(&stream, -MAX_WBITS) == Z_OK);
        entry.data.assign(size + 1, '\0');
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data() + data));
        stream.avail_in = static_cast<uInt>(packed);
        stream.next_out = reinterpret_cast<Bytef*>(entry.data.data());
        stream.avail_out = static_cast<uInt>(entry.data.size());
        const int result = inflate(&stream, Z_FINISH);
        const std::size_t produced = stream.total_out;
        inflateEnd(&stream);
        REQUIRE(result == Z_STREAM_END);
        REQUIRE(produced == size);
        entry.data.resize(produced);
        REQUIRE(Crc(entry.data) == crc);
        entries.push_back(std::move(entry));
        at += 46 + name_length + extra_length + comment_length;
    }
    REQUIRE(at == end);
    return entries;
}

/// More than a few of the writer's chunks, and not a repeat of one byte.
std::string LogLines(int count) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        text += "[" + std::to_string(i) + "] Log <Info> core/x.cpp:" +
                std::to_string((i * 7919) % 100003) + ": line\n";
    }
    return text;
}

fs::file_time_type Ago(int minutes) {
    return fs::file_time_type::clock::now() - std::chrono::minutes{minutes};
}

} // namespace

TEST_CASE("Session zip round trip", "[common][log_retention]") {
    TempDir dir;
    const fs::path log = dir.path / "eden_log.txt";
    const fs::path zip = dir.path / std::string{Retention::SessionZipName};

    SECTION("a log of several chunks") {
        const std::string text = LogLines(20000);
        REQUIRE(text.size() > 3 * 256 * 1024);
        Write(log, text, 1);
        REQUIRE(Retention::WriteSessionZip(log, std::nullopt, std::stop_token{}));
        const auto entries = ReadZip(zip);
        REQUIRE(entries.size() == 1);
        REQUIRE(entries[0].name == "eden_log.txt");
        REQUIRE(entries[0].data == text);
        REQUIRE(entries[0].crc == Crc(text));
        REQUIRE(fs::file_size(zip) < text.size() / 2);
    }

    SECTION("an empty log") {
        Write(log, "", 1);
        REQUIRE(Retention::WriteSessionZip(log, std::nullopt, std::stop_token{}));
        const auto entries = ReadZip(zip);
        REQUIRE(entries.size() == 1);
        REQUIRE(entries[0].data.empty());
    }

    SECTION("the log of another name is stored as eden_log.txt, even while it is open") {
        const fs::path second = dir.path / "eden_log_2.txt";
        std::ofstream live{second, std::ios::binary};
        live << "written by this process\n";
        live.flush();
        REQUIRE(Retention::WriteSessionZip(second, std::nullopt, std::stop_token{}));
        const auto entries = ReadZip(zip);
        REQUIRE(entries.size() == 1);
        REQUIRE(entries[0].name == "eden_log.txt");
        REQUIRE(entries[0].data == "written by this process\n");
    }

    SECTION("no log, no zip") {
        REQUIRE_FALSE(Retention::WriteSessionZip(log, std::nullopt, std::stop_token{}));
        REQUIRE(Names(dir.path).empty());
    }
}

TEST_CASE("Session zip holds exactly the snapshot of a growing log", "[common][log_retention]") {
    TempDir dir;
    const fs::path log = dir.path / "eden_log.txt";
    const fs::path zip = dir.path / "out.zip";
    const std::string text = LogLines(15000);
    Write(log, text, 1);
    const std::vector<Retention::ZipInput> inputs{
        {log, "eden_log.txt", fs::file_size(log), Retention::ToTimeT(Ago(60))}};
    {
        // The log grows after the size was taken.
        std::ofstream more{log, std::ios::binary | std::ios::app};
        more << LogLines(5000);
    }
    REQUIRE(fs::file_size(log) > text.size());
    REQUIRE(Retention::WriteZip(zip, inputs, std::stop_token{}));
    const auto entries = ReadZip(zip);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].data == text);
    REQUIRE(entries[0].crc == Crc(text));

    SECTION("a file that got shorter than its snapshot fails and leaves nothing") {
        fs::remove(zip);
        Write(log, "short", 1);
        REQUIRE_FALSE(Retention::WriteZip(zip, inputs, std::stop_token{}));
        REQUIRE(Names(dir.path) == std::set<std::string>{"eden_log.txt"});
    }

    SECTION("a stop request leaves nothing") {
        fs::remove(zip);
        std::stop_source source;
        source.request_stop();
        REQUIRE_FALSE(Retention::WriteZip(zip, inputs, source.get_token()));
        REQUIRE(Names(dir.path) == std::set<std::string>{"eden_log.txt"});
    }
}

TEST_CASE("Session zip is written from an archive only when it is missing or older",
          "[common][log_retention]") {
    const std::string newest = "eden_log.20260105-000000.txt";
    std::vector<Retention::FileTime> files{
        {newest, Ago(10)},
        {"eden_log.20260104-000000.txt", Ago(100)},
        {"eden_log.txt", Ago(0)},
        {"eglantine-last-session.zip", Ago(5)},
        {"eglantine-last-session.zip.part", Ago(0)},
    };
    const auto pick = [&](std::optional<fs::file_time_type> zip) {
        return Retention::SelectArchiveForSessionZip("eden_log", files, zip);
    };

    REQUIRE(pick(std::nullopt) == newest);   // no zip yet
    REQUIRE(pick(Ago(20)) == newest);        // the zip is older than the archive
    REQUIRE_FALSE(pick(Ago(5)));             // the zip is newer
    REQUIRE_FALSE(pick(files[0].modified));  // the same time is not newer

    SECTION("the newest archive of all is the one, by name") {
        files.push_back({"eden_log.20260105-000000-1.txt", Ago(50)});
        REQUIRE(pick(Ago(60)) == "eden_log.20260105-000000-1.txt");
        REQUIRE_FALSE(pick(Ago(40)));
    }

    SECTION("no archive") {
        files.erase(files.begin(), files.begin() + 2);
        REQUIRE_FALSE(pick(std::nullopt));
        REQUIRE_FALSE(pick(Ago(20)));
    }

    SECTION("the newest archive is already compressed") {
        files.push_back({"eden_log.20260106-000000.txt.zst", Ago(1)});
        REQUIRE_FALSE(pick(std::nullopt));
    }

    SECTION("another log's archives are not this log's") {
        files.erase(files.begin(), files.begin() + 2);
        files.push_back({"eden_log_2.20260105-000000.txt", Ago(10)});
        REQUIRE_FALSE(pick(std::nullopt));
    }
}

TEST_CASE("Session zip is written from the rotated log after a crash", "[common][log_retention]") {
    TempDir dir;
    const fs::path log = dir.path / "eden_log.txt";
    const fs::path zip = dir.path / std::string{Retention::SessionZipName};
    const fs::path archive = dir.path / "eden_log.20260105-000000.txt";
    Write(archive, "crashed session\n", 120);

    REQUIRE(Retention::WriteSessionZipFromArchive(log, std::stop_token{}));
    auto entries = ReadZip(zip);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].name == "eden_log.txt");
    REQUIRE(entries[0].data == "crashed session\n");

    // The zip is newer than the log it was written from: nothing to do at the next start.
    REQUIRE_FALSE(Retention::WriteSessionZipFromArchive(log, std::stop_token{}));

    // A later session ended without a zip (killed): the existing zip is replaced.
    fs::last_write_time(zip, Ago(60));
    Write(archive, "killed session\n", 0);
    REQUIRE(Retention::WriteSessionZipFromArchive(log, std::stop_token{}));
    entries = ReadZip(zip);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].data == "killed session\n");
    REQUIRE(Names(dir.path) ==
            std::set<std::string>{"eden_log.20260105-000000.txt", "eglantine-last-session.zip"});
}

TEST_CASE("Session zip replaces an existing one without leaving a part", "[common][log_retention]") {
    TempDir dir;
    const fs::path log = dir.path / "eden_log.txt";
    const fs::path zip = dir.path / std::string{Retention::SessionZipName};
    Write(zip, "not a zip", 24);
    Write(log, "this session\n", 1);
    REQUIRE(Retention::WriteSessionZip(log, std::nullopt, std::stop_token{}));
    const auto entries = ReadZip(zip);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].data == "this session\n");
    REQUIRE(Names(dir.path) == std::set<std::string>{"eden_log.txt", "eglantine-last-session.zip"});
}

TEST_CASE("Session zip takes the crash dump of the same session only", "[common][log_retention]") {
    REQUIRE(Retention::DumpBelongsToSession(100, 100));
    REQUIRE(Retention::DumpBelongsToSession(101, 100));
    REQUIRE_FALSE(Retention::DumpBelongsToSession(99, 100));
    REQUIRE_FALSE(Retention::DumpBelongsToSession(100, std::nullopt));

    // Without a creation time the start is a minute before the last write; a creation time
    // earlier than that does not move it (it may be inherited from a renamed file).
    REQUIRE(Retention::ArchiveSessionStart(std::nullopt, 1000) == 940);
    REQUIRE(Retention::ArchiveSessionStart(100, 1000) == 940);
    REQUIRE(Retention::ArchiveSessionStart(990, 1000) == 990);

    TempDir dir;
    const fs::path log = dir.path / "eden_log.txt";
    const fs::path zip = dir.path / std::string{Retention::SessionZipName};
    const fs::path dump = dir.path / "eden_crash.dmp";
    Write(log, "session\n", 0);
    const std::time_t start = Retention::ToTimeT(Ago(60));
    const std::string dump_bytes = LogLines(300); // stands in for the minidump

    SECTION("written during the session") {
        Write(dump, dump_bytes, 0);
        fs::last_write_time(dump, Ago(30));
        REQUIRE(Retention::WriteSessionZip(log, start, std::stop_token{}));
        const auto entries = ReadZip(zip);
        REQUIRE(entries.size() == 2);
        REQUIRE(entries[0].name == "eden_log.txt");
        REQUIRE(entries[1].name == "eden_crash.dmp");
        REQUIRE(entries[1].data == dump_bytes);
    }

    SECTION("left by an earlier session") {
        Write(dump, dump_bytes, 0);
        fs::last_write_time(dump, Ago(180));
        REQUIRE(Retention::WriteSessionZip(log, start, std::stop_token{}));
        const auto entries = ReadZip(zip);
        REQUIRE(entries.size() == 1);
        REQUIRE(entries[0].name == "eden_log.txt");
    }

    SECTION("without a start time") {
        Write(dump, dump_bytes, 0);
        REQUIRE(Retention::WriteSessionZip(log, std::nullopt, std::stop_token{}));
        REQUIRE(ReadZip(zip).size() == 1);
    }

    SECTION("no dump") {
        REQUIRE(Retention::WriteSessionZip(log, start, std::stop_token{}));
        REQUIRE(ReadZip(zip).size() == 1);
    }
}

TEST_CASE("The session zip is not a session log archive", "[common][log_retention]") {
    REQUIRE_FALSE(Retention::ParseArchiveName("eden_log", "eglantine-last-session.zip"));
    REQUIRE_FALSE(Retention::ParseArchiveName("eden_log", "eglantine-last-session.zip.part"));

    const std::vector<std::string> names{
        "eden_log.20260105-000000.txt",
        "eden_log.20260104-000000.txt",
        "eglantine-last-session.zip",
        "eglantine-last-session.zip.part",
    };
    REQUIRE(Retention::SelectForDeletion("eden_log", names, 5).empty());
    REQUIRE(Retention::SelectForCompression("eden_log", names) ==
            std::vector<std::string>{"eden_log.20260104-000000.txt"});
}
