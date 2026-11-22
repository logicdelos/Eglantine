// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/read_attribution.h"

namespace RA = Common::ReadAttribution;

namespace {

RA::Row MakeRow(RA::Kind kind, std::string name, u64 reads, u64 bytes) {
    return {kind, std::move(name), reads, bytes};
}

} // namespace

TEST_CASE("Read attribution registers a source once", "[common][read_attribution]") {
    const u32 a = RA::Register(RA::Kind::NupMember, "ra_test/a.bin");
    REQUIRE(a != 0);
    REQUIRE(RA::Register(RA::Kind::NupMember, "ra_test/a.bin") == a);
    REQUIRE(RA::Register(RA::Kind::File, "ra_test/a.bin") != a);
    REQUIRE(RA::Register(RA::Kind::NupMember, "ra_test/b.bin") != a);
}

TEST_CASE("Read attribution counts reads per source and resolves a name once",
          "[common][read_attribution]") {
    static_cast<void>(RA::TakeWindowForTests());
    const u64 reads_before = RA::TotalReads();
    const u64 bytes_before = RA::TotalBytes();

    RA::Source source;
    int resolved = 0;
    const auto name = [&] {
        ++resolved;
        return std::string{"ra_test/counted.bin"};
    };
    source.Note(RA::Kind::Storage, 100, name);
    source.Note(RA::Kind::Storage, 50, name);
    source.Note(RA::Kind::Storage, 25, name);
    REQUIRE(resolved == 1);
    REQUIRE(RA::TotalReads() == reads_before + 3);
    REQUIRE(RA::TotalBytes() == bytes_before + 175);

    {
        const RA::ScopedSuppress quiet;
        source.Note(RA::Kind::Storage, 1000, name);
    }
    REQUIRE(RA::TotalReads() == reads_before + 3);

    const auto rows = RA::TakeWindowForTests();
    const auto it = std::ranges::find_if(rows, [](const RA::Row& row) {
        return row.name == "ra_test/counted.bin";
    });
    REQUIRE(it != rows.end());
    REQUIRE(it->kind == RA::Kind::Storage);
    REQUIRE(it->reads == 3);
    REQUIRE(it->bytes == 175);
    REQUIRE(RA::TakeWindowForTests().empty());
}

TEST_CASE("Read attribution reports only bursts", "[common][read_attribution]") {
    // Quiet window: nothing.
    REQUIRE_FALSE(RA::FormatBurst(10.0, std::vector{MakeRow(RA::Kind::File, "a", 499, 1 << 20)}));
    REQUIRE_FALSE(RA::FormatBurst(10.0, {}));

    // 500 reads, or 64 MiB, of one kind is a burst.
    REQUIRE(RA::FormatBurst(10.0, std::vector{MakeRow(RA::Kind::File, "a", 500, 1000)}));
    REQUIRE(RA::FormatBurst(10.0,
                            std::vector{MakeRow(RA::Kind::File, "a", 2, u64{64} << 20)}));
    // The kinds are not added together: 300 + 300 is not 500 of one kind.
    REQUIRE_FALSE(RA::FormatBurst(10.0, std::vector{MakeRow(RA::Kind::File, "a", 300, 10),
                                                    MakeRow(RA::Kind::Storage, "b", 300, 10)}));
}

TEST_CASE("Read attribution line", "[common][read_attribution]") {
    const std::vector rows{MakeRow(RA::Kind::NupMember, "a.bin", 400, u64{48} << 20),
                           MakeRow(RA::Kind::NupMember, "b.bin", 150, u64{1} << 20)};
    REQUIRE(RA::FormatBurst(10.04, rows) ==
            "Game reads in the last 10.0 s: nup 550 reads 49.0 MiB. Top sources by bytes: [nup] "
            "a.bin 400 reads 48.0 MiB; [nup] b.bin 150 reads 1.0 MiB");
}

TEST_CASE("Read attribution lists the five biggest sources", "[common][read_attribution]") {
    std::vector<RA::Row> rows;
    for (u64 i = 1; i <= 8; ++i) {
        rows.push_back(MakeRow(RA::Kind::NupMember, "m" + std::to_string(i), 100, i << 20));
    }
    const auto line = RA::FormatBurst(10.0, rows);
    REQUIRE(line);
    for (const char* listed : {"m8 ", "m7 ", "m6 ", "m5 ", "m4 "}) {
        REQUIRE(line->find(std::string{"] "} + listed) != std::string::npos);
    }
    for (const char* left_out : {"m3 ", "m2 ", "m1 "}) {
        REQUIRE(line->find(std::string{"] "} + left_out) == std::string::npos);
    }
    REQUIRE(line->find("m8 ") < line->find("m7 "));
    REQUIRE(line->find("m5 ") < line->find("m4 "));

    // A long path keeps its end.
    const std::string path(200, 'p');
    const auto shortened =
        RA::FormatBurst(10.0, std::vector{MakeRow(RA::Kind::File, path + "/file.bin", 600, 10)});
    REQUIRE(shortened);
    REQUIRE(shortened->find("...") != std::string::npos);
    REQUIRE(shortened->find("/file.bin") != std::string::npos);
    REQUIRE(shortened->size() < 250);
}
