// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/arm/dynarmic/jit_prewarm.h"
#include "core/hardware_properties.h"

namespace {

using namespace Core::JitPrewarm;

constexpr u64 SplitMix(u64 x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

Entry MakeEntry(u64 core, u64 index) {
    return Entry{0x0000007100000000ULL + core * 0x1000000ULL + index * 4,
                 SplitMix((core << 40) | index)};
}

std::vector<Entry> MakeList(u64 core, std::size_t count) {
    std::vector<Entry> list;
    for (std::size_t i = 0; i < count; ++i) {
        list.push_back(MakeEntry(core, i));
    }
    return list;
}

std::vector<std::vector<Entry>> MakeLists(std::initializer_list<std::size_t> sizes) {
    std::vector<std::vector<Entry>> lists;
    for (const std::size_t size : sizes) {
        lists.push_back(MakeList(lists.size(), size));
    }
    return lists;
}

Identity MakeIdentity(u32 cores) {
    return Identity{0x1122334455667788ULL, 0x99aabbccddeeff00ULL, 0x0123456789abcdefULL,
                    0x00010002, cores};
}

// The format, restated independently of the production code.
u64 Fnv1a(const u8* data, std::size_t size) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash = (hash ^ data[i]) * 0x100000001b3ULL;
    }
    return hash;
}

u64 Get(const std::vector<u8>& bytes, std::size_t at, std::size_t width) {
    u64 value = 0;
    for (std::size_t i = width; i-- > 0;) {
        value = (value << 8) | bytes[at + i];
    }
    return value;
}

void Put(std::vector<u8>& bytes, std::size_t at, u64 value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes[at + i] = static_cast<u8>(value >> (8 * i));
    }
}

/// Makes the body size, the body checksum and the header checksum agree with the bytes again, so a
/// test can reach the checks behind them.
void Seal(std::vector<u8>& bytes) {
    Put(bytes, 48, bytes.size() - HeaderBytes, 8);
    Put(bytes, 56, Fnv1a(bytes.data() + HeaderBytes, bytes.size() - HeaderBytes), 8);
    Put(bytes, 72, Fnv1a(bytes.data(), 72), 8);
}

Parsed ParseVector(const std::vector<u8>& bytes, const Identity& expected) {
    return Parse(std::span<const u8>{bytes}, expected);
}

class ScratchDirectory {
public:
    ScratchDirectory()
        : path{std::filesystem::temp_directory_path() /
               ("jit-prewarm-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
    }
    ~ScratchDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    const std::filesystem::path path;
};

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

class ScopedEnv {
public:
    ScopedEnv(const char* name_, const char* value) : name{name_} {
        if (const char* old = std::getenv(name_)) {
            saved = std::string{old};
        }
        SetEnv(name_, value);
    }
    ~ScopedEnv() {
        SetEnv(name, saved ? saved->c_str() : nullptr);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name;
    std::optional<std::string> saved;
};

void ExpectTranslate(Cursor& cursor, u64 now_ms, u32 index) {
    const Cursor::Step step = cursor.Next(now_ms);
    REQUIRE(step.kind == Cursor::Kind::Translate);
    REQUIRE(step.index == index);
}

void ExpectWait(Cursor& cursor, u64 now_ms, u64 until_ms) {
    const Cursor::Step step = cursor.Next(now_ms);
    REQUIRE(step.kind == Cursor::Kind::Wait);
    REQUIRE(step.wait_until_ms == until_ms);
}

/// Entries {i, 3 * i + 1}: the location of a call says which entry it was.
std::vector<Entry> IndexedList(std::size_t count) {
    std::vector<Entry> list;
    for (u64 i = 0; i < count; ++i) {
        list.push_back(Entry{i, 3 * i + 1});
    }
    return list;
}

} // namespace

// --- The file ---

TEST_CASE("JitPrewarm: the file layout is the documented one", "[jit_prewarm]") {
    const Identity identity = MakeIdentity(1);
    const std::vector<std::vector<Entry>> lists{{Entry{0x7100001234ULL, 0xdeadbeefcafef00dULL}}};
    const std::vector<u8> bytes = Serialize(identity, lists);

    REQUIRE(bytes.size() == HeaderBytes + 8 + 16);
    CHECK(std::memcmp(bytes.data(), "EGLJITH1", 8) == 0);
    CHECK(Get(bytes, 8, 4) == 1);
    CHECK(Get(bytes, 12, 4) == 1);
    CHECK(Get(bytes, 16, 8) == identity.build_id);
    CHECK(Get(bytes, 24, 8) == identity.content_stamp);
    CHECK(Get(bytes, 32, 8) == identity.title_id);
    CHECK(Get(bytes, 40, 4) == identity.game_version);
    CHECK(Get(bytes, 44, 4) == 0);
    CHECK(Get(bytes, 48, 8) == 24);
    CHECK(Get(bytes, 56, 8) == Fnv1a(bytes.data() + HeaderBytes, 24));
    CHECK(Get(bytes, 64, 8) == 1);
    CHECK(Get(bytes, 72, 8) == Fnv1a(bytes.data(), 72));
    CHECK(Get(bytes, 80, 8) == 1);
    CHECK(Get(bytes, 88, 8) == 0x7100001234ULL);
    CHECK(Get(bytes, 96, 8) == 0xdeadbeefcafef00dULL);
}

TEST_CASE("JitPrewarm: a list survives a round trip", "[jit_prewarm]") {
    const auto lists = MakeLists({0, 1, 5, 40, 3, 17});
    const Identity identity = MakeIdentity(6);
    const std::vector<u8> bytes = Serialize(identity, lists);

    std::size_t body = 6 * 8;
    for (const auto& list : lists) {
        body += list.size() * 16;
    }
    CHECK(bytes.size() == HeaderBytes + body);

    const Parsed parsed = ParseVector(bytes, identity);
    CHECK(parsed.reject == Reject::None);
    CHECK(parsed.per_core == lists);
}

TEST_CASE("JitPrewarm: the core count of the identity is the core count of the lists",
          "[jit_prewarm]") {
    const auto lists = MakeLists({2, 2, 2});
    const std::vector<u8> bytes = Serialize(MakeIdentity(99), lists);
    CHECK(Get(bytes, 12, 4) == 3);
    CHECK(ParseVector(bytes, MakeIdentity(3)).per_core == lists);
}

TEST_CASE("JitPrewarm: a rejected file yields no lists", "[jit_prewarm]") {
    std::vector<u8> bytes = Serialize(MakeIdentity(2), MakeLists({4, 4}));
    bytes[0] ^= 1;
    const Parsed parsed = ParseVector(bytes, MakeIdentity(2));
    CHECK(parsed.reject == Reject::BadMagic);
    CHECK(parsed.per_core.empty());
}

TEST_CASE("JitPrewarm: every reason to reject a file is told apart", "[jit_prewarm]") {
    const Identity identity = MakeIdentity(2);
    const std::vector<u8> good = Serialize(identity, MakeLists({3, 2}));
    REQUIRE(ParseVector(good, identity).reject == Reject::None);

    SECTION("too short") {
        std::vector<u8> bytes = good;
        bytes.resize(HeaderBytes - 1);
        CHECK(ParseVector(bytes, identity).reject == Reject::TooShort);
        CHECK(ParseVector({}, identity).reject == Reject::TooShort);
    }
    SECTION("magic") {
        std::vector<u8> bytes = good;
        bytes[7] = '2';
        CHECK(ParseVector(bytes, identity).reject == Reject::BadMagic);
    }
    SECTION("version") {
        std::vector<u8> bytes = good;
        Put(bytes, 8, 2, 4);
        CHECK(ParseVector(bytes, identity).reject == Reject::BadVersion);
    }
    SECTION("header checksum") {
        std::vector<u8> bytes = good;
        bytes[44] ^= 0x10; // the reserved field is covered too
        CHECK(ParseVector(bytes, identity).reject == Reject::HeaderChecksum);
        bytes = good;
        bytes[72] ^= 1;
        CHECK(ParseVector(bytes, identity).reject == Reject::HeaderChecksum);
    }
    SECTION("build") {
        Identity other = identity;
        other.build_id ^= 1;
        CHECK(ParseVector(good, other).reject == Reject::Build);
    }
    SECTION("content") {
        Identity other = identity;
        other.content_stamp += 1;
        CHECK(ParseVector(good, other).reject == Reject::Content);
    }
    SECTION("title") {
        Identity other = identity;
        other.title_id += 1;
        CHECK(ParseVector(good, other).reject == Reject::Title);
    }
    SECTION("game version") {
        Identity other = identity;
        other.game_version += 1;
        CHECK(ParseVector(good, other).reject == Reject::GameVersion);
    }
    SECTION("core count") {
        CHECK(ParseVector(good, MakeIdentity(3)).reject == Reject::CoreCount);
        CHECK(ParseVector(good, MakeIdentity(0)).reject == Reject::CoreCount);
        // More cores than the format allows, even when the file and the host agree.
        const std::vector<std::vector<Entry>> many(MaxCores + 1);
        const std::vector<u8> bytes = Serialize(MakeIdentity(0), many);
        CHECK(ParseVector(bytes, MakeIdentity(MaxCores + 1)).reject == Reject::CoreCount);
        // None at all.
        const std::vector<u8> none = Serialize(MakeIdentity(0), {});
        CHECK(ParseVector(none, MakeIdentity(0)).reject == Reject::CoreCount);
    }
    SECTION("body size") {
        std::vector<u8> shorter = good;
        shorter.pop_back();
        CHECK(ParseVector(shorter, identity).reject == Reject::BodySize);
        std::vector<u8> longer = good;
        longer.push_back(0);
        CHECK(ParseVector(longer, identity).reject == Reject::BodySize);
    }
    SECTION("body checksum") {
        std::vector<u8> bytes = good;
        bytes[HeaderBytes + 8 + 3] ^= 0x80;
        CHECK(ParseVector(bytes, identity).reject == Reject::BodyChecksum);
    }
}

TEST_CASE("JitPrewarm: a file whose checksums are right but whose lists are not is rejected",
          "[jit_prewarm]") {
    const Identity identity = MakeIdentity(2);
    const std::vector<u8> good = Serialize(identity, MakeLists({2, 1}));
    REQUIRE(ParseVector(good, identity).reject == Reject::None);

    SECTION("a list longer than the format allows") {
        std::vector<u8> bytes = good;
        Put(bytes, HeaderBytes, MaxEntriesPerCore + 1, 8);
        Seal(bytes);
        CHECK(ParseVector(bytes, identity).reject == Reject::TooManyEntries);
    }
    SECTION("a list that claims more entries than the file holds") {
        std::vector<u8> bytes = good;
        Put(bytes, HeaderBytes, 4, 8); // four entries are 64 bytes; the body has 56 after the count
        Seal(bytes);
        CHECK(ParseVector(bytes, identity).reject == Reject::BodySize);
        bytes = good;
        Put(bytes, HeaderBytes, std::size_t{1} << 62, 8);
        Seal(bytes);
        CHECK(ParseVector(bytes, identity).reject == Reject::TooManyEntries);
    }
    SECTION("a total that does not add up") {
        std::vector<u8> bytes = good;
        Put(bytes, 64, 4, 8);
        Seal(bytes);
        CHECK(ParseVector(bytes, identity).reject == Reject::BodySize);
    }
    SECTION("bytes after the last list") {
        std::vector<u8> bytes = good;
        bytes.resize(bytes.size() + 16);
        Seal(bytes);
        CHECK(ParseVector(bytes, identity).reject == Reject::BodySize);
    }
    SECTION("a core without a list") {
        std::vector<u8> bytes = Serialize(MakeIdentity(1), MakeLists({2}));
        Put(bytes, 12, 2, 4);
        Seal(bytes);
        CHECK(ParseVector(bytes, identity).reject == Reject::BodySize);
    }
    SECTION("a body that ends inside an entry") {
        std::vector<u8> bytes = good;
        bytes.resize(bytes.size() - 8);
        Seal(bytes);
        CHECK(ParseVector(bytes, identity).reject == Reject::BodySize);
    }
}

TEST_CASE("JitPrewarm: the first defect in the order of the checks is the one reported",
          "[jit_prewarm]") {
    const Identity identity = MakeIdentity(2);
    std::vector<u8> bytes = Serialize(identity, MakeLists({3, 3}));
    Identity other = identity;
    other.build_id += 1;
    other.title_id += 1;
    CHECK(ParseVector(bytes, other).reject == Reject::Build);
    bytes[HeaderBytes + 9] ^= 1; // a corrupt body too
    CHECK(ParseVector(bytes, other).reject == Reject::Build);
    CHECK(ParseVector(bytes, identity).reject == Reject::BodyChecksum);
    bytes[8] = 9; // and a wrong version, which is looked at before the checksum
    CHECK(ParseVector(bytes, identity).reject == Reject::BadVersion);
    bytes[0] = 0;
    CHECK(ParseVector(bytes, identity).reject == Reject::BadMagic);
}

TEST_CASE("JitPrewarm: no flipped byte of a file is accepted", "[jit_prewarm]") {
    const Identity identity = MakeIdentity(3);
    const std::vector<u8> good = Serialize(identity, MakeLists({2, 0, 3}));
    REQUIRE(ParseVector(good, identity).reject == Reject::None);
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < good.size(); ++i) {
        std::vector<u8> bytes = good;
        bytes[i] ^= 1;
        if (ParseVector(bytes, identity).reject == Reject::None) {
            ++accepted;
        }
    }
    CHECK(accepted == 0);
}

TEST_CASE("JitPrewarm: reasons have distinct stable names", "[jit_prewarm]") {
    const std::vector<std::pair<Reject, std::string>> names{
        {Reject::None, "none"},
        {Reject::TooShort, "too_short"},
        {Reject::BadMagic, "bad_magic"},
        {Reject::BadVersion, "bad_version"},
        {Reject::HeaderChecksum, "header_checksum"},
        {Reject::Build, "build"},
        {Reject::Content, "content"},
        {Reject::Title, "title"},
        {Reject::GameVersion, "game_version"},
        {Reject::CoreCount, "core_count"},
        {Reject::BodySize, "body_size"},
        {Reject::BodyChecksum, "body_checksum"},
        {Reject::TooManyEntries, "too_many_entries"},
    };
    std::set<std::string> seen;
    for (const auto& [reject, name] : names) {
        CHECK(std::string{RejectName(reject)} == name);
        seen.insert(name);
    }
    CHECK(seen.size() == names.size());
}

// --- The list to write ---

TEST_CASE("JitPrewarm: the written list drops the stale and adds the new", "[jit_prewarm]") {
    const Entry a{1, 10};
    const Entry b{2, 20};
    const Entry c{3, 30};
    const Entry d{4, 40};
    const Entry e{5, 50};
    const Entry f{6, 60};
    const std::vector<Entry> loaded{a, b, c, d};
    const std::vector<u32> stale{1, 3};
    const std::vector<Entry> recorded{e, a, f, e};

    CHECK(MergeForWrite(loaded, stale, recorded, 100) == std::vector<Entry>{a, c, e, f});
    CHECK(MergeForWrite(loaded, {}, {}, 100) == loaded);
    CHECK(MergeForWrite({}, {}, recorded, 100) == std::vector<Entry>{e, a, f});
    CHECK(MergeForWrite({}, {}, {}, 100).empty());
}

TEST_CASE("JitPrewarm: the written list is cut at the cap, keeping the older entries",
          "[jit_prewarm]") {
    const std::vector<Entry> loaded = MakeList(0, 5);
    const std::vector<u32> stale{0};
    const std::vector<Entry> recorded = MakeList(1, 5);

    const std::vector<Entry> three = MergeForWrite(loaded, stale, recorded, 3);
    CHECK(three == std::vector<Entry>{loaded[1], loaded[2], loaded[3]});
    const std::vector<Entry> six = MergeForWrite(loaded, stale, recorded, 6);
    CHECK(six == std::vector<Entry>{loaded[1], loaded[2], loaded[3], loaded[4], recorded[0],
                                    recorded[1]});
    CHECK(MergeForWrite(loaded, stale, recorded, 0).empty());
}

TEST_CASE("JitPrewarm: one address with two checksums is two entries", "[jit_prewarm]") {
    const std::vector<Entry> loaded{Entry{7, 1}, Entry{7, 2}};
    const std::vector<Entry> recorded{Entry{7, 2}, Entry{7, 3}};
    CHECK(MergeForWrite(loaded, {}, recorded, 10) ==
          std::vector<Entry>{Entry{7, 1}, Entry{7, 2}, Entry{7, 3}});
}

TEST_CASE("JitPrewarm: a large merge keeps the order and has no repeat", "[jit_prewarm]") {
    const std::vector<Entry> loaded = MakeList(2, 1000);
    std::vector<u32> stale;
    for (u32 i = 0; i < loaded.size(); i += 7) {
        stale.push_back(i);
    }
    std::vector<Entry> recorded = MakeList(2, 1500); // the first 1000 are in `loaded`
    const std::vector<Entry> again(recorded.begin(), recorded.begin() + 300);
    recorded.insert(recorded.end(), again.begin(), again.end());

    std::vector<Entry> expected;
    for (std::size_t i = 0; i < loaded.size(); ++i) {
        if (!std::binary_search(stale.begin(), stale.end(), static_cast<u32>(i))) {
            expected.push_back(loaded[i]);
        }
    }
    // The stale entries are in `recorded` as well: recorded again this run, so they come back at
    // the end of the list in the order they were recorded.
    for (std::size_t i = 0; i < 1500; ++i) {
        const Entry& entry = recorded[i];
        if (std::find(expected.begin(), expected.end(), entry) == expected.end()) {
            expected.push_back(entry);
        }
    }
    CHECK(MergeForWrite(loaded, stale, recorded, MaxEntriesPerCore) == expected);
}

// --- Files ---

TEST_CASE("JitPrewarm: a file is replaced whole and read back", "[jit_prewarm]") {
    const ScratchDirectory scratch;
    const std::filesystem::path path = scratch.path / "list.bin";
    const std::vector<u8> first = Serialize(MakeIdentity(2), MakeLists({3, 1}));
    const std::vector<u8> second = Serialize(MakeIdentity(2), MakeLists({9, 8}));

    CHECK(!ReadFileBytes(path, 1 << 20).has_value());
    REQUIRE(WriteFileAtomic(path, first));
    CHECK(ReadFileBytes(path, 1 << 20) == first);
    REQUIRE(WriteFileAtomic(path, second));
    CHECK(ReadFileBytes(path, 1 << 20) == second);
    CHECK(!std::filesystem::exists(scratch.path / "list.bin.tmp"));
    CHECK(ParseVector(*ReadFileBytes(path, 1 << 20), MakeIdentity(2)).reject == Reject::None);
}

TEST_CASE("JitPrewarm: a temporary file left by a crash is overwritten", "[jit_prewarm]") {
    const ScratchDirectory scratch;
    const std::filesystem::path path = scratch.path / "list.bin";
    {
        std::ofstream junk{scratch.path / "list.bin.tmp", std::ios::binary};
        junk << std::string(4096, 'x');
    }
    REQUIRE(std::filesystem::exists(scratch.path / "list.bin.tmp"));
    const std::vector<u8> bytes = Serialize(MakeIdentity(1), MakeLists({2}));
    REQUIRE(WriteFileAtomic(path, bytes));
    CHECK(ReadFileBytes(path, 1 << 20) == bytes);
    CHECK(!std::filesystem::exists(scratch.path / "list.bin.tmp"));
}

TEST_CASE("JitPrewarm: a file larger than the limit is not read", "[jit_prewarm]") {
    const ScratchDirectory scratch;
    const std::filesystem::path path = scratch.path / "list.bin";
    const std::vector<u8> bytes(1000, 0x5a);
    REQUIRE(WriteFileAtomic(path, bytes));
    CHECK(ReadFileBytes(path, 1000) == bytes);
    CHECK(!ReadFileBytes(path, 999).has_value());
}

TEST_CASE("JitPrewarm: a file cannot be written into a folder that does not exist",
          "[jit_prewarm]") {
    const ScratchDirectory scratch;
    const std::vector<u8> bytes(16, 1);
    CHECK(!WriteFileAtomic(scratch.path / "missing" / "list.bin", bytes));
}

// --- The scheduling rule ---

TEST_CASE("JitPrewarm: the cursor goes through the list in order", "[jit_prewarm]") {
    Cursor cursor{4};
    ExpectTranslate(cursor, 0, 0);
    ExpectTranslate(cursor, 0, 0); // until it is told how it went
    cursor.Report(0, Outcome::Compiled);
    ExpectTranslate(cursor, 0, 1);
    cursor.Report(1, Outcome::Present);
    ExpectTranslate(cursor, 5, 2);
    cursor.Report(2, Outcome::Compiled);
    ExpectTranslate(cursor, 5, 3);
    cursor.Report(3, Outcome::Present);
    CHECK(!cursor.Finished());
    CHECK(cursor.Next(5).kind == Cursor::Kind::Finished);
    CHECK(cursor.Finished());
    CHECK(cursor.Compiled() == 2);
    CHECK(cursor.Present() == 2);
    CHECK(cursor.Stale().empty());
    CHECK(cursor.Next(1'000'000).kind == Cursor::Kind::Finished);
}

TEST_CASE("JitPrewarm: an empty list is finished at once", "[jit_prewarm]") {
    Cursor cursor{0};
    CHECK(cursor.Next(0).kind == Cursor::Kind::Finished);
    CHECK(cursor.Finished());
    CHECK(cursor.Stale().empty());
}

TEST_CASE("JitPrewarm: a mismatch is tried again only after the retry gap", "[jit_prewarm]") {
    Cursor cursor{3, Policy{3, 2000}};
    ExpectTranslate(cursor, 100, 0);
    cursor.Report(0, Outcome::Compiled);
    ExpectTranslate(cursor, 100, 1);
    cursor.Report(1, Outcome::Mismatch);
    ExpectTranslate(cursor, 100, 2); // the others do not wait for it
    cursor.Report(2, Outcome::Compiled);

    // The gap starts when the pass ends, not when the mismatch was seen.
    ExpectWait(cursor, 150, 2150);
    ExpectWait(cursor, 2000, 2150);
    ExpectWait(cursor, 2149, 2150);
    ExpectTranslate(cursor, 2150, 1);
    cursor.Report(1, Outcome::Compiled);
    CHECK(cursor.Next(2150).kind == Cursor::Kind::Finished);
    CHECK(cursor.Compiled() == 3);
    CHECK(cursor.Stale().empty());
}

TEST_CASE("JitPrewarm: entries that keep mismatching are stale after the last pass",
          "[jit_prewarm]") {
    Cursor cursor{4, Policy{3, 50}};
    for (u32 i = 0; i < 4; ++i) {
        ExpectTranslate(cursor, 0, i);
        cursor.Report(i, i % 2 == 1 ? Outcome::Mismatch : Outcome::Compiled);
    }
    ExpectWait(cursor, 10, 60);
    // Second pass: only the mismatches, in the same order. Entry 3 is there now.
    ExpectTranslate(cursor, 60, 1);
    cursor.Report(1, Outcome::Mismatch);
    ExpectTranslate(cursor, 60, 3);
    cursor.Report(3, Outcome::Compiled);
    ExpectWait(cursor, 61, 111); // the gap starts at the first look after the pass
    ExpectTranslate(cursor, 111, 1);
    cursor.Report(1, Outcome::Mismatch);
    // Third and last pass: it is finished, with the entry that never matched.
    CHECK(cursor.Next(111).kind == Cursor::Kind::Finished);
    const std::vector<u32> stale(cursor.Stale().begin(), cursor.Stale().end());
    CHECK(stale == std::vector<u32>{1});
    CHECK(cursor.Compiled() == 3);
}

TEST_CASE("JitPrewarm: with one pass the mismatches are stale at once, in ascending order",
          "[jit_prewarm]") {
    Cursor cursor{9, Policy{1, 2000}};
    for (u32 i = 0; i < 9; ++i) {
        ExpectTranslate(cursor, 0, i);
        cursor.Report(i, (i == 2 || i == 5 || i == 7) ? Outcome::Mismatch : Outcome::Compiled);
    }
    CHECK(cursor.Next(0).kind == Cursor::Kind::Finished);
    const std::vector<u32> stale(cursor.Stale().begin(), cursor.Stale().end());
    CHECK(stale == std::vector<u32>{2, 5, 7});
    CHECK(cursor.Compiled() == 6);
}

TEST_CASE("JitPrewarm: no pass at all is taken as one pass", "[jit_prewarm]") {
    Cursor cursor{1, Policy{0, 2000}};
    ExpectTranslate(cursor, 0, 0);
    cursor.Report(0, Outcome::Mismatch);
    CHECK(cursor.Next(0).kind == Cursor::Kind::Finished);
    CHECK(cursor.Stale().size() == 1);
}

TEST_CASE("JitPrewarm: a full code cache ends the list and judges nothing", "[jit_prewarm]") {
    Cursor cursor{5, Policy{3, 10}};
    ExpectTranslate(cursor, 0, 0);
    cursor.Report(0, Outcome::Mismatch);
    ExpectTranslate(cursor, 0, 1);
    cursor.Report(1, Outcome::Compiled);
    ExpectTranslate(cursor, 0, 2);
    cursor.Report(2, Outcome::NoSpace);
    CHECK(cursor.Next(0).kind == Cursor::Kind::Finished);
    CHECK(cursor.Next(100'000).kind == Cursor::Kind::Finished);
    CHECK(cursor.Stale().empty());
    CHECK(cursor.Compiled() == 1);
}

TEST_CASE("JitPrewarm: a JIT that cannot translate ahead ends the list", "[jit_prewarm]") {
    Cursor cursor{5};
    ExpectTranslate(cursor, 0, 0);
    cursor.Report(0, Outcome::Unsupported);
    CHECK(cursor.Next(0).kind == Cursor::Kind::Finished);
    CHECK(cursor.Stale().empty());
    CHECK(cursor.Compiled() == 0);
    CHECK(cursor.Present() == 0);
}

TEST_CASE("JitPrewarm: a busy JIT neither advances nor judges", "[jit_prewarm]") {
    Cursor cursor{2, Policy{2, 10}};
    ExpectTranslate(cursor, 0, 0);
    cursor.Report(0, Outcome::Busy);
    ExpectTranslate(cursor, 0, 0);
    cursor.Report(0, Outcome::Busy);
    ExpectTranslate(cursor, 0, 0);
    cursor.Report(0, Outcome::Compiled);
    ExpectTranslate(cursor, 0, 1);
    cursor.Report(1, Outcome::Busy);
    ExpectTranslate(cursor, 0, 1);
    cursor.Report(1, Outcome::Present);
    CHECK(cursor.Next(0).kind == Cursor::Kind::Finished);
    CHECK(cursor.Compiled() == 1);
    CHECK(cursor.Present() == 1);
    CHECK(cursor.Stale().empty());
}

// --- One core ---

TEST_CASE("JitPrewarm: a core waits for its list to be published", "[jit_prewarm]") {
    CoreState state;
    std::atomic<bool> stop{false};
    std::vector<u64> calls;
    const auto now = [] { return u64{0}; };
    const auto compile = [&](const Entry& entry) {
        calls.push_back(entry.location);
        return Outcome::Compiled;
    };
    CHECK(state.Run(stop, now, compile) == RunResult::Poll);
    CHECK(state.Run(stop, now, compile) == RunResult::Poll);
    CHECK(calls.empty());
    state.Publish(IndexedList(2));
    CHECK(state.Run(stop, now, compile) == RunResult::Finished);
    CHECK(calls == std::vector<u64>{0, 1});
}

TEST_CASE("JitPrewarm: a core without a list never translates", "[jit_prewarm]") {
    CoreState state;
    std::atomic<bool> stop{false};
    std::size_t calls = 0;
    state.Publish(std::nullopt);
    const auto now = [] { return u64{0}; };
    const auto compile = [&](const Entry&) {
        ++calls;
        return Outcome::Compiled;
    };
    CHECK(state.Run(stop, now, compile) == RunResult::Finished);
    CHECK(state.Run(stop, now, compile) == RunResult::Finished);
    CHECK(calls == 0);
    CHECK(state.Loaded().empty());
}

TEST_CASE("JitPrewarm: a core translates its list once and reports it done", "[jit_prewarm]") {
    CoreState state;
    std::atomic<bool> stop{false};
    std::vector<u64> calls;
    const auto now = [] { return u64{0}; };
    const auto compile = [&](const Entry& entry) {
        calls.push_back(entry.location);
        return entry.location % 2 == 0 ? Outcome::Compiled : Outcome::Present;
    };
    std::vector<u32> stale{99};
    state.Publish(IndexedList(5));
    CHECK(!state.GetStale(stale));
    CHECK(state.Run(stop, now, compile) == RunResult::Finished);
    CHECK(calls == std::vector<u64>{0, 1, 2, 3, 4});
    CHECK(state.GetStale(stale));
    CHECK(stale.empty());
    CHECK(state.Run(stop, now, compile) == RunResult::Finished);
    CHECK(calls.size() == 5);
    CHECK(state.Loaded().size() == 5);
}

TEST_CASE("JitPrewarm: a core stops between two blocks when it is told to", "[jit_prewarm]") {
    CoreState state;
    std::atomic<bool> stop{false};
    std::vector<u64> calls;
    const auto now = [] { return u64{0}; };
    const auto compile = [&](const Entry& entry) {
        calls.push_back(entry.location);
        if (entry.location == 1) {
            stop.store(true); // an interrupt arrives while the second block is translated
        }
        return Outcome::Compiled;
    };
    state.Publish(IndexedList(5));
    CHECK(state.Run(stop, now, compile) == RunResult::Poll);
    CHECK(calls == std::vector<u64>{0, 1});
    CHECK(state.Run(stop, now, compile) == RunResult::Poll); // still told to stop: no block
    CHECK(calls.size() == 2);
    stop.store(false);
    CHECK(state.Run(stop, now, compile) == RunResult::Finished);
    CHECK(calls == std::vector<u64>{0, 1, 2, 3, 4});
}

TEST_CASE("JitPrewarm: a busy JIT is asked again for the same block", "[jit_prewarm]") {
    CoreState state;
    std::atomic<bool> stop{false};
    std::vector<u64> calls;
    int busy = 2;
    const auto now = [] { return u64{0}; };
    const auto compile = [&](const Entry& entry) {
        calls.push_back(entry.location);
        if (entry.location == 1 && busy > 0) {
            --busy;
            return Outcome::Busy;
        }
        return Outcome::Compiled;
    };
    state.Publish(IndexedList(3));
    CHECK(state.Run(stop, now, compile) == RunResult::Poll);
    CHECK(calls == std::vector<u64>{0, 1});
    CHECK(state.Run(stop, now, compile) == RunResult::Poll);
    CHECK(calls == std::vector<u64>{0, 1, 1});
    CHECK(state.Run(stop, now, compile) == RunResult::Finished);
    CHECK(calls == std::vector<u64>{0, 1, 1, 1, 2});
}

TEST_CASE("JitPrewarm: a core retries a mismatch later and then calls it stale", "[jit_prewarm]") {
    CoreState state{Policy{3, 2000}};
    std::atomic<bool> stop{false};
    std::vector<u64> calls;
    u64 clock = 10;
    const auto now = [&] { return clock; };
    const auto compile = [&](const Entry& entry) {
        calls.push_back(entry.location);
        return entry.location == 1 ? Outcome::Mismatch : Outcome::Compiled;
    };
    state.Publish(IndexedList(3));

    CHECK(state.Run(stop, now, compile) == RunResult::Poll); // waits for the retry
    CHECK(calls == std::vector<u64>{0, 1, 2});
    clock = 2009;
    CHECK(state.Run(stop, now, compile) == RunResult::Poll);
    CHECK(calls.size() == 3);
    clock = 2010;
    CHECK(state.Run(stop, now, compile) == RunResult::Poll); // tried again, waits for the next
    CHECK(calls == std::vector<u64>{0, 1, 2, 1});
    clock = 4009;
    CHECK(state.Run(stop, now, compile) == RunResult::Poll);
    CHECK(calls.size() == 4);
    clock = 4010;
    CHECK(state.Run(stop, now, compile) == RunResult::Finished);
    CHECK(calls == std::vector<u64>{0, 1, 2, 1, 1});

    std::vector<u32> stale;
    REQUIRE(state.GetStale(stale));
    CHECK(stale == std::vector<u32>{1});
}

TEST_CASE("JitPrewarm: a full code cache leaves the rest of the list for the game",
          "[jit_prewarm]") {
    CoreState state;
    std::atomic<bool> stop{false};
    std::vector<u64> calls;
    const auto now = [] { return u64{0}; };
    const auto compile = [&](const Entry& entry) {
        calls.push_back(entry.location);
        return entry.location == 2 ? Outcome::NoSpace : Outcome::Compiled;
    };
    state.Publish(IndexedList(6));
    CHECK(state.Run(stop, now, compile) == RunResult::Finished);
    CHECK(calls == std::vector<u64>{0, 1, 2});
    std::vector<u32> stale;
    REQUIRE(state.GetStale(stale));
    CHECK(stale.empty());
    CHECK(state.Run(stop, now, compile) == RunResult::Finished);
    CHECK(calls.size() == 3);
}

// --- Recording ---

TEST_CASE("JitPrewarm: a core records the blocks it is told of, up to its capacity",
          "[jit_prewarm]") {
    CoreState state{Policy{}, 3};
    CHECK(state.RecordedCount() == 0);
    state.Record(10, 1);
    state.Record(20, 2);
    CHECK(state.Recorded().size() == 2);
    state.Record(30, 3);
    state.Record(40, 4); // beyond the capacity
    CHECK(state.RecordedCount() == 3);
    const std::span<const Entry> recorded = state.Recorded();
    REQUIRE(recorded.size() == 3);
    CHECK(recorded[0] == Entry{10, 1});
    CHECK(recorded[1] == Entry{20, 2});
    CHECK(recorded[2] == Entry{30, 3});
}

TEST_CASE("JitPrewarm: nothing is recorded after the recording is closed", "[jit_prewarm]") {
    CoreState state{Policy{}, 10};
    state.Record(1, 1);
    state.CloseRecording();
    state.Record(2, 2);
    CHECK(state.RecordedCount() == 1);
    CHECK(state.Recorded()[0] == Entry{1, 1});
}

TEST_CASE("JitPrewarm: a reader sees only complete entries while the core records",
          "[jit_prewarm]") {
    constexpr std::size_t Count = 50'000;
    CoreState state{Policy{}, Count};
    std::thread core{[&state] {
        for (u64 i = 0; i < Count; ++i) {
            state.Record(i, i * 7 + 1);
        }
    }};
    std::size_t torn = 0;
    std::size_t seen = 0;
    while (seen < Count) {
        const std::span<const Entry> recorded = state.Recorded();
        for (; seen < recorded.size(); ++seen) {
            if (!(recorded[seen] == Entry{seen, seen * 7 + 1})) {
                ++torn;
            }
        }
        std::this_thread::yield();
    }
    core.join();
    CHECK(torn == 0);
    CHECK(state.RecordedCount() == Count);
}

// --- Switch ---

TEST_CASE("JitPrewarm: the switch turns the whole thing off", "[jit_prewarm]") {
    const ScopedEnv off{"EDEN_NX2_JIT_PREWARM", "0"};
    const int process_key = 0;
    CHECK(!Attach(0x0123456789abcdefULL, &process_key, 0).has_value());
}

TEST_CASE("JitPrewarm: a core the guest does not have gets no state", "[jit_prewarm]") {
    const ScopedEnv on{"EDEN_NX2_JIT_PREWARM", "1"};
    const int process_key = 0;
    CHECK(!Attach(0x0123456789abcdefULL, &process_key, Core::Hardware::NUM_CPU_CORES)
               .has_value());
}
