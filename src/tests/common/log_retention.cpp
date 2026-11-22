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

#include <catch2/catch_test_macros.hpp>

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
