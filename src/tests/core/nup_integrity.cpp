// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <memory>
#include <chrono>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <openssl/evp.h>

#include "common/zstd_compression.h"
#include "core/file_sys/game_file_identity.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_integrity.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {

using FileSys::NupContainer;
namespace Integrity = FileSys::NupIntegrity;
using namespace std::chrono_literals;

void Put(std::vector<u8>& bytes, std::size_t offset, u64 value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (8 * i));
    }
}

std::vector<u8> Content(std::size_t size, u64 seed) {
    std::vector<u8> out(size);
    std::mt19937_64 rng{seed};
    for (u8& byte : out) {
        const u64 x = rng();
        byte = static_cast<u8>(x & (x >> 8));
    }
    return out;
}

std::array<u8, 0x20> Sha256(const std::vector<u8>& data) {
    std::array<u8, 0x20> hash{};
    unsigned int length{};
    EVP_Digest(data.data(), data.size(), hash.data(), &length, EVP_sha256(), nullptr);
    return hash;
}

struct Member {
    std::string name;
    std::vector<u8> extent;
    u64 size;
    u8 codec;
    std::array<u8, 0x20> sha;
};

Member Raw(std::string name, const std::vector<u8>& content) {
    return {std::move(name), content, content.size(), 0, Sha256(content)};
}

Member Zstd(std::string name, const std::vector<u8>& content) {
    return {std::move(name),
            Common::Compression::CompressDataZSTD(content.data(), content.size(), 3),
            content.size(), 1, Sha256(content)};
}

// A single base package whose root directory holds the members as files 0..n-1; unlike the
// builder of nup_zstd_read.cpp it stores the member hashes.
std::vector<u8> BuildNup(const std::vector<Member>& members) {
    constexpr std::size_t TocOffset = 0x80;
    const std::size_t files_at = 0x30 + 0x40 + 0x18;
    const std::size_t children_at = files_at + members.size() * 0x50;
    const std::size_t pool_at = children_at + members.size() * 4;
    std::string pool;
    for (const auto& member : members) {
        pool += member.name;
    }
    const std::size_t toc_size = pool_at + pool.size();
    const std::size_t blob_offset = (TocOffset + toc_size + 15) & ~std::size_t{15};
    std::vector<u8> bytes(blob_offset);
    std::vector<u64> extent_offsets;
    for (const auto& member : members) {
        bytes.resize((bytes.size() + 15) & ~std::size_t{15});
        extent_offsets.push_back(bytes.size());
        bytes.insert(bytes.end(), member.extent.begin(), member.extent.end());
    }

    std::copy(NupContainer::HeaderMagic.begin(), NupContainer::HeaderMagic.end(), bytes.begin());
    Put(bytes, 0x04, 0x80, 2);
    Put(bytes, 0x08, TocOffset, 8);
    Put(bytes, 0x10, toc_size, 8);
    Put(bytes, 0x38, blob_offset, 8);
    Put(bytes, 0x40, bytes.size(), 8);
    Put(bytes, 0x48, 1, 4);
    const std::size_t toc = TocOffset;
    Put(bytes, toc + 0x00, 0x01434F54, 4);
    Put(bytes, toc + 0x04, 1, 4);
    Put(bytes, toc + 0x08, 1, 8);
    Put(bytes, toc + 0x10, members.size(), 8);
    Put(bytes, toc + 0x18, pool_at, 8);
    Put(bytes, toc + 0x20, pool.size(), 8);
    std::copy(pool.begin(), pool.end(), bytes.begin() + toc + pool_at);
    Put(bytes, toc + 0x30 + 0x0C, 0xFFFFFFFF, 4);
    Put(bytes, toc + 0x30 + 0x40 + 0x0C, members.size(), 4);
    std::size_t name_offset = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const std::size_t record = toc + files_at + i * 0x50;
        Put(bytes, record + 0x00, name_offset, 4);
        Put(bytes, record + 0x04, members[i].name.size(), 4);
        name_offset += members[i].name.size();
        Put(bytes, record + 0x08, members[i].size, 8);
        Put(bytes, record + 0x10, extent_offsets[i], 8);
        Put(bytes, record + 0x18, members[i].extent.size(), 8);
        std::copy(members[i].sha.begin(), members[i].sha.end(), bytes.begin() + record + 0x20);
        bytes[record + 0x40] = members[i].codec;
        Put(bytes, toc + children_at + i * 4, 1 + i, 4);
    }
    unsigned int length{};
    EVP_Digest(bytes.data() + toc, toc_size, bytes.data() + 0x18, &length, EVP_sha256(), nullptr);
    return bytes;
}

std::unique_ptr<NupContainer> Open(const std::vector<Member>& members) {
    auto nup = std::make_unique<NupContainer>(
        std::make_shared<FileSys::VectorVfsFile>(BuildNup(members)), std::size_t{64} << 20,
        std::filesystem::temp_directory_path());
    REQUIRE(nup->GetStatus() == Loader::ResultStatus::Success);
    return nup;
}

bool Named(const NupContainer::MemberVerifyResult& result, std::string_view name) {
    return std::ranges::any_of(result.bad, [&](const NupContainer::BadMember& bad) {
        return bad.path.ends_with(name);
    });
}

Integrity::CacheEntry Entry(std::string path, u64 size, s64 mtime) {
    Integrity::CacheEntry entry;
    entry.stamp = {std::move(path), size, mtime};
    entry.checked_at = 1'700'000'000;
    entry.members = 3;
    return entry;
}

} // namespace

TEST_CASE("NupContainer::VerifyMembers passes a sound container", "[nup][nup-integrity]") {
    const auto nup = Open({Raw("a.bin", Content(3 * 1024 * 1024 + 17, 1)),
                           Zstd("b.bin", Content(2 * 1024 * 1024 + 5, 2))});
    u64 asked = 0;
    const auto result = nup->VerifyMembers([&](u64 bytes) {
        asked += bytes;
        return true;
    });
    REQUIRE(result.bad.empty());
    REQUIRE_FALSE(result.cancelled);
    REQUIRE(result.members == 2);
    REQUIRE(result.checked == 2);
    REQUIRE(result.bytes_hashed == (3 * 1024 * 1024 + 17) + (2 * 1024 * 1024 + 5));
    REQUIRE(result.bytes_read == asked);
}

TEST_CASE("NupContainer::VerifyMembers names every corrupt member and goes on",
          "[nup][nup-integrity]") {
    auto raw = Raw("raw_bad.bin", Content(1500 * 1024, 3));
    raw.extent[1200 * 1024] ^= 0xFF;
    auto zstd = Zstd("zstd_bad.bin", Content(1500 * 1024, 4));
    zstd.extent[zstd.extent.size() / 2] ^= 0xFF;
    auto wrong_hash = Raw("hash_bad.bin", Content(4096, 5));
    wrong_hash.sha[7] ^= 0x01;
    const auto nup = Open({raw, Raw("good_1.bin", Content(70000, 6)), zstd, wrong_hash,
                           Zstd("good_2.bin", Content(70000, 7))});

    const auto result = nup->VerifyMembers({});
    REQUIRE_FALSE(result.cancelled);
    REQUIRE(result.checked == 5);
    REQUIRE(result.bad.size() == 3);
    REQUIRE(Named(result, "raw_bad.bin"));
    REQUIRE(Named(result, "zstd_bad.bin"));
    REQUIRE(Named(result, "hash_bad.bin"));
    REQUIRE_FALSE(Named(result, "good_1.bin"));
    REQUIRE_FALSE(Named(result, "good_2.bin"));
    for (const auto& bad : result.bad) {
        REQUIRE_FALSE(bad.reason.empty());
    }
}

TEST_CASE("NupContainer::VerifyMembers stops when before_read says so", "[nup][nup-integrity]") {
    const auto nup = Open({Raw("a.bin", Content(3 * 1024 * 1024, 8)),
                           Raw("b.bin", Content(1024, 9))});
    int calls = 0;
    const auto result = nup->VerifyMembers([&](u64) { return ++calls < 3; });
    REQUIRE(result.cancelled);
    REQUIRE(result.checked == 0);
    REQUIRE(calls == 3);
}

TEST_CASE("NUP integrity cache round trip", "[nup][nup-integrity]") {
    std::vector<Integrity::CacheEntry> entries;
    Integrity::Remember(entries, Entry("C:\\Games\\a b.nup", 1000, -5));
    auto bad = Entry("D:\\x.nup", 2000, 77);
    bad.bad = {{"base/romfs/a.bin", "SHA-256 mismatch"}, {"update-v1/exefs/main", "zstd data corrupt"}};
    Integrity::Remember(entries, bad);

    const auto parsed = Integrity::ParseCache(Integrity::SerializeCache(entries));
    REQUIRE(parsed.size() == 2);
    REQUIRE(parsed[0].stamp == bad.stamp);
    REQUIRE(parsed[0].bad.size() == 2);
    REQUIRE(parsed[0].bad[1].path == "update-v1/exefs/main");
    REQUIRE(parsed[0].bad[1].reason == "zstd data corrupt");
    REQUIRE(parsed[1].stamp.path == "C:\\Games\\a b.nup");
    REQUIRE(parsed[1].stamp.mtime == -5);
    REQUIRE(parsed[1].members == 3);

    REQUIRE(Integrity::FindCached(parsed, bad.stamp) != nullptr);
    auto touched = bad.stamp;
    touched.mtime += 1;
    REQUIRE(Integrity::FindCached(parsed, touched) == nullptr);
    touched = bad.stamp;
    touched.size += 1;
    REQUIRE(Integrity::FindCached(parsed, touched) == nullptr);
}

TEST_CASE("NUP integrity cache ignores damage and keeps a bounded size", "[nup][nup-integrity]") {
    REQUIRE(Integrity::ParseCache("").empty());
    REQUIRE(Integrity::ParseCache("something else 1\nfile\t1\t2\t3\t4\tp\n").empty());
    const auto parsed = Integrity::ParseCache(
        "eglantine-nup-integrity 1\r\nbad\tx\ty\nfile\tnot-a-number\t2\t3\t4\tp\n"
        "file\t10\t20\t30\t4\tgood.nup\r\nbad\treason\tmember\nbad\tbroken\nfile\t1\t2\n");
    REQUIRE(parsed.size() == 1);
    REQUIRE(parsed[0].stamp.path == "good.nup");
    REQUIRE(parsed[0].bad.size() == 1);

    // A name with a control character cannot split a line.
    auto odd = Entry("a\nb\tc.nup", 1, 1);
    odd.bad = {{"m\nx", "r\ty"}};
    const auto sane = Integrity::ParseCache(Integrity::SerializeCache(std::vector{odd}));
    REQUIRE(sane.size() == 1);
    REQUIRE(sane[0].stamp.path == "a?b?c.nup");

    std::vector<Integrity::CacheEntry> many;
    for (s64 i = 0; i < 40; ++i) {
        Integrity::Remember(many, Entry("f" + std::to_string(i % 20), 1, i));
    }
    REQUIRE(many.size() == 20 - 4);
    REQUIRE(many.front().stamp.mtime == 39);
    REQUIRE(Integrity::ParseCache(Integrity::SerializeCache(many)).size() == many.size());
}

TEST_CASE("NUP integrity warning lists every member", "[nup][nup-integrity]") {
    std::vector<Integrity::BadMember> bad;
    for (int i = 0; i < 19; ++i) {
        bad.push_back({"base/romfs/m" + std::to_string(i), "SHA-256 mismatch"});
    }
    const auto lines = Integrity::FormatBadMembers("xc2.nup", bad);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].starts_with("NUP integrity: xc2.nup has 19 corrupt member(s), 1-12:"));
    REQUIRE(lines[1].starts_with("NUP integrity: xc2.nup has 19 corrupt member(s), 13-19:"));
    for (const auto& member : bad) {
        const bool listed = std::ranges::any_of(lines, [&](const std::string& line) {
            return line.find(member.path + " (") != std::string::npos;
        });
        REQUIRE(listed);
    }
    REQUIRE(Integrity::FormatBadMembers("xc2.nup", {}).empty());
}

TEST_CASE("NUP integrity reads are paced and wait for the game", "[nup][nup-integrity]") {
    using Clock = Integrity::ReadPacer::Clock;
    constexpr u64 MiB = 1024 * 1024;
    Integrity::ReadPacer pacer;
    const auto t0 = Clock::now();

    // The first read goes at once; the next one waits for the rate limit.
    REQUIRE(pacer.Admit(0, 0, t0, MiB) == 0ns);
    const auto wait = pacer.Admit(0, 0, t0 + 10ms, MiB);
    REQUIRE(wait > 0ns);
    REQUIRE(wait <= Integrity::PollInterval);
    REQUIRE(pacer.Admit(0, 0, t0 + 40ms, MiB) == 0ns);

    // 300 reads in 300 ms is a game that is loading: wait, and keep waiting while it goes on.
    REQUIRE(pacer.Admit(300, 300 * 4096, t0 + 340ms, MiB) == Integrity::PollInterval);
    REQUIRE(pacer.GameIsBusy());
    REQUIRE(pacer.Admit(600, 600 * 4096, t0 + 640ms, MiB) == Integrity::PollInterval);

    // Quiet again: go on.
    REQUIRE(pacer.Admit(600, 600 * 4096, t0 + 900ms, MiB) == 0ns);
    REQUIRE_FALSE(pacer.GameIsBusy());

    // Few but large reads are heavy too.
    REQUIRE(pacer.Admit(610, 600 * 4096 + 40 * MiB, t0 + 1200ms, MiB) == Integrity::PollInterval);
    REQUIRE(pacer.GameIsBusy());
}

TEST_CASE("Game file identity line", "[nup][nup-integrity]") {
    namespace Identity = FileSys::GameFileIdentity;
    const std::vector<std::string> labels{
        "0|0100e95004038800|65536|Update|1.1.0",
        "1|0100e95004039001|0|Expansion Pass|0",
        "file|x.nca|123",
        "2|0000000000000000|0|a|b|c|3",
    };
    const auto addons = Identity::SummarizeAddons(labels);
    REQUIRE(addons.updates == 1);
    REQUIRE(addons.dlcs == 1);
    REQUIRE(addons.mods == 1);
    REQUIRE(addons.entries.size() == 3);
    REQUIRE(addons.entries[0] == "update \"Update\" v1.1.0");
    REQUIRE(addons.entries[2] == "mod \"a|b|c\" v3");

    Identity::FileFacts facts;
    facts.path = "D:\\Games\\xc2.nup";
    facts.exists = true;
    facts.size = 15245867008ull;
    REQUIRE(Identity::KindOf(facts) == "nup");
    REQUIRE(Identity::FormatIdentity(facts, 0x0100E95004038000ull, 65536, addons, 0xABCDEF) ==
            "Game file: title=0100E95004038000 version=65536 kind=nup path=\"D:\\Games\\xc2.nup\" "
            "size=14.20 GiB (15245867008 bytes) addons=3 (1 update, 1 dlc, 1 mod: update "
            "\"Update\" v1.1.0, dlc \"Expansion Pass\" v0, mod \"a|b|c\" v3) content=0000000000abcdef");

    facts.exists = false;
    REQUIRE(Identity::FormatIdentity(facts, 1, 0, {}, 0).find("exists=no") != std::string::npos);
    facts.path = "C:\\x\\game";
    REQUIRE(Identity::KindOf(facts) == "unknown");
    facts.path = "C:\\x.y\\game";
    REQUIRE(Identity::KindOf(facts) == "unknown");
    REQUIRE(Identity::FormatSize(900) == "900 B");
    REQUIRE(Identity::FormatSize(512ull << 20) == "512.0 MiB");
}
