// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Relocating the translation hint list across launches with address space layout randomisation.
//
// The JIT names a block by an A64 location descriptor (PC, FPCR bits, single-step flag). The PC is
// a guest address, and the whole initial process image moves by a per-launch ASLR delta, so a
// persisted descriptor cannot be replayed at another launch. The hint list stores the PC as a byte
// offset from the image base instead, computed by CodeImage::ToRelative and turned back into a
// descriptor for this launch by CodeImage::ToAbsolute. These checks pin the arithmetic and the
// flags, the rejections that keep a bad offset out of the JIT, the initial-text containment rule
// the record and replay paths gate on, and the file round trip.

#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "core/arm/dynarmic/jit_prewarm.h"
#include "core/hle/kernel/k_process.h"

namespace {

using namespace Core::JitPrewarm;

// The descriptor layout, restated independently of dynarmic: PC in bits 0..55, FPCR in
// 0x07C80000 << 37, single-step in bit 57.
constexpr u64 PcBits = 56;
constexpr u64 PcMask = (u64{1} << PcBits) - 1;
constexpr u64 SingleStepBit = u64{1} << 57;
constexpr u64 FpcrMask = u64{0x07C8'0000};
constexpr u64 AddressLimit = u64{1} << (PcBits - 1);

u64 Location(u64 pc, u32 fpcr = 0, bool single_step = false) {
    return (pc & PcMask) | ((u64{fpcr} & FpcrMask) << 37) | (single_step ? SingleStepBit : 0);
}

// The two measured launches of the frontend: the same image, 0x3f2000 apart.
constexpr u64 ColdBase = 0x80b5'a000ULL;
constexpr u64 WarmBase = 0x80f4'c000ULL;
constexpr u64 Delta = 0x3f'2000ULL;
constexpr u64 ImageSize = 0x0200'0000ULL;

// FPCR bits 26 and 19: they land at descriptor bits 63 and 56, next to the PC field.
constexpr u32 Flags = 0x0408'0000;

constexpr u64 Hash = 0xdead'beef'cafe'f00dULL;

const CodeImage Cold{ColdBase, ImageSize};
const CodeImage Warm{WarmBase, ImageSize};

Identity MakeIdentity() {
    return Identity{0x1122334455667788ULL, 0x99aabbccddeeff00ULL, 0x0123456789abcdefULL,
                    0x00010002, 1};
}

Parsed ParseVector(const std::vector<u8>& bytes, const Identity& expected) {
    return Parse(std::span<const u8>{bytes}, expected);
}

// The spans LoadModule publishes for a two-module image: text only. The gaps model the read-only
// and writable data the loader maps next to each text, which a hint span must never reach into.
using CodeSpans = Kernel::KProcess::InitialCodeSpan;
constexpr CodeSpans TestCodeSpans[] = {
    {0x1000, 0x3000}, // rtld .text
    {0x4000, 0x6000}, // main .text
};
constexpr std::span<const CodeSpans> TestCode{TestCodeSpans};
constexpr CodeSpans ReversedCodeSpans[] = {
    {0x4000, 0x6000},
    {0x1000, 0x3000},
};
constexpr std::span<const CodeSpans> ReversedCode{ReversedCodeSpans};

static_assert(Kernel::KProcess::IsInsideInitialCode(TestCode, 0x1000, 4));
static_assert(Kernel::KProcess::IsInsideInitialCode(TestCode, 0x2FFC, 4));
static_assert(!Kernel::KProcess::IsInsideInitialCode(TestCode, 0x2FFC, 8));
static_assert(!Kernel::KProcess::IsInsideInitialCode(TestCode, 0x3000, 4));
static_assert(!Kernel::KProcess::IsInsideInitialCode(TestCode, 0x2FFC, 0x1008));
static_assert(!Kernel::KProcess::IsInsideInitialCode(TestCode, 0x1000, 0));

} // namespace

TEST_CASE("JitPrewarm rebase: an offset does not depend on the ASLR base", "[jit_prewarm_rebase]") {
    REQUIRE(WarmBase - ColdBase == Delta);

    const u64 cold_location = Location(ColdBase + 0x1234, Flags);
    const std::optional<u64> relative = Cold.ToRelative(cold_location, 3);
    REQUIRE(relative.has_value());

    // The wire value is the offset, not a guest address.
    CHECK(*relative == Location(0x1234, Flags));
    CHECK((*relative & PcMask) == 0x1234);
    CHECK((*relative & PcMask) < ImageSize);

    // The same offset becomes this launch's address, exactly one delta away.
    const std::optional<u64> warm_location = Warm.ToAbsolute(*relative, 3);
    REQUIRE(warm_location.has_value());
    CHECK(*warm_location == Location(WarmBase + 0x1234, Flags));
    CHECK(*warm_location - cold_location == Delta);

    // And back again: relocating twice is not cumulative.
    const std::optional<u64> back = Warm.ToRelative(*warm_location, 3);
    REQUIRE(back.has_value());
    CHECK(*back == *relative);
    CHECK(Warm.ToAbsolute(*back, 3) == warm_location);
}

TEST_CASE("JitPrewarm rebase: the translation flags are preserved", "[jit_prewarm_rebase]") {
    SECTION("FPCR bits are not part of the offset") {
        const u64 with_flags = Location(ColdBase + 0x2000, Flags);
        const u64 without = Location(ColdBase + 0x2000, 0);
        const std::optional<u64> relative = Cold.ToRelative(with_flags, 1);
        REQUIRE(relative.has_value());
        CHECK((*relative & ~PcMask) == (with_flags & ~PcMask));
        CHECK(*relative == Location(0x2000, Flags));
        const std::optional<u64> plain = Cold.ToRelative(without, 1);
        REQUIRE(plain.has_value());
        CHECK(*plain != *relative);

        const std::optional<u64> replay = Warm.ToAbsolute(*relative, 1);
        REQUIRE(replay.has_value());
        CHECK((*replay & ~PcMask) == (with_flags & ~PcMask));
    }
    SECTION("every FPCR bit the descriptor carries survives") {
        const u64 all_flags = Location(ColdBase + 0x40, 0x07C8'0000);
        const std::optional<u64> relative = Cold.ToRelative(all_flags, 1);
        REQUIRE(relative.has_value());
        CHECK(*relative == Location(0x40, 0x07C8'0000));
        CHECK(Warm.ToAbsolute(*relative, 1) == Location(WarmBase + 0x40, 0x07C8'0000));
    }
}

TEST_CASE("JitPrewarm rebase: a single-stepping descriptor is not a hint", "[jit_prewarm_rebase]") {
    const u64 absolute = Location(ColdBase + 0x100, 0, true);
    CHECK_FALSE(Cold.ToRelative(absolute, 1).has_value());
    CHECK_FALSE(Cold.ToAbsolute(Location(0x100, 0, true), 1).has_value());
}

TEST_CASE("JitPrewarm rebase: a value that is not a descriptor is rejected",
          "[jit_prewarm_rebase]") {
    // Bit 58 belongs to no descriptor field: UniqueHash would drop it, so replaying such a value
    // would translate a different descriptor than the guest had. Everything in bit 56 and bits
    // 59..63 is a real FPCR bit and must survive; bit 57 is the single-step flag.
    for (const u64 stray : {u64{1} << 58}) {
        const u64 absolute = Location(ColdBase + 0x80, Flags) | stray;
        CHECK_FALSE(Cold.ToRelative(absolute, 1).has_value());
        CHECK_FALSE(Cold.ToAbsolute(Location(0x80, Flags) | stray, 1).has_value());
    }
}

TEST_CASE("JitPrewarm rebase: a span must lie inside the image", "[jit_prewarm_rebase]") {
    SECTION("the first word") {
        const std::optional<u64> relative = Cold.ToRelative(Location(ColdBase, Flags), 1);
        REQUIRE(relative.has_value());
        CHECK(*relative == Location(0, Flags));
        CHECK(Warm.ToAbsolute(*relative, 1) == Location(WarmBase, Flags));
    }
    SECTION("the last word") {
        const u64 absolute = Location(ColdBase + ImageSize - sizeof(u32), Flags);
        const std::optional<u64> relative = Cold.ToRelative(absolute, 1);
        REQUIRE(relative.has_value());
        CHECK(Warm.ToAbsolute(*relative, 1) ==
              Location(WarmBase + ImageSize - sizeof(u32), Flags));
    }
    SECTION("one word past the end") {
        const u64 absolute = Location(ColdBase + ImageSize, Flags);
        CHECK_FALSE(Cold.ToRelative(absolute, 1).has_value());
        CHECK_FALSE(Warm.ToAbsolute(Location(ImageSize, Flags), 1).has_value());
    }
    SECTION("a span that starts inside and ends outside") {
        const u64 absolute = Location(ColdBase + ImageSize - sizeof(u32), Flags);
        CHECK_FALSE(Cold.ToRelative(absolute, 2).has_value());
        CHECK_FALSE(Warm.ToAbsolute(Location(ImageSize - sizeof(u32), Flags), 2).has_value());
    }
    SECTION("below the image") {
        CHECK_FALSE(Cold.ToRelative(Location(ColdBase - sizeof(u32), Flags), 1).has_value());
    }
    SECTION("no image at all") {
        const CodeImage empty{ColdBase, 0};
        CHECK_FALSE(empty.ToRelative(Location(ColdBase, Flags), 1).has_value());
        CHECK_FALSE(empty.ToRelative(Location(ColdBase + 0x10, Flags), 1).has_value());
        CHECK_FALSE(empty.ToAbsolute(Location(0, Flags), 1).has_value());
    }
    SECTION("an unaligned instruction address") {
        CHECK_FALSE(Cold.ToRelative(Location(ColdBase + 2, Flags), 1).has_value());
        CHECK_FALSE(Warm.ToAbsolute(Location(2, Flags), 1).has_value());
    }
    SECTION("an unaligned image base cannot have offsets") {
        const CodeImage shifted{ColdBase + 2, ImageSize};
        CHECK_FALSE(shifted.ToRelative(Location(ColdBase + 4, Flags), 1).has_value());
        CHECK_FALSE(shifted.ToAbsolute(Location(4, Flags), 1).has_value());
    }
}

TEST_CASE("JitPrewarm rebase: a word count a block cannot have is rejected",
          "[jit_prewarm_rebase]") {
    const u64 absolute = Location(ColdBase + 0x1000, Flags);
    SECTION("zero words") {
        CHECK_FALSE(Cold.ToRelative(absolute, 0).has_value());
        CHECK_FALSE(Cold.ToAbsolute(Location(0x1000, Flags), 0).has_value());
    }
    SECTION("one more word than a block may have") {
        CHECK_FALSE(Cold.ToRelative(absolute, 1025).has_value());
        CHECK_FALSE(Cold.ToAbsolute(Location(0x1000, Flags), 1025).has_value());
    }
    SECTION("the largest span a block may have, when it fits") {
        const u64 at = ColdBase + ImageSize - 1024 * sizeof(u32);
        const u64 largest = Location(at, Flags);
        const std::optional<u64> relative = Cold.ToRelative(largest, 1024);
        REQUIRE(relative.has_value());
        CHECK(*relative == Location(ImageSize - 1024 * sizeof(u32), Flags));
        CHECK(Warm.ToAbsolute(*relative, 1024) == Location(WarmBase + ImageSize - 4096, Flags));
    }
}

TEST_CASE("JitPrewarm rebase: an image outside the positive 56-bit PC range is rejected",
          "[jit_prewarm_rebase]") {
    // The PC is sign-extended from bit 55, so anything at or above 2^55 is not a guest address.
    const CodeImage above{AddressLimit, 0x1000};
    CHECK_FALSE(above.ToRelative(Location(AddressLimit, Flags), 1).has_value());
    CHECK_FALSE(above.ToAbsolute(Location(0, Flags), 1).has_value());

    const CodeImage limit{AddressLimit - 0x1000, 0x1000};
    CHECK(limit.ToAbsolute(Location(0xFFC, Flags), 1) ==
          Location(AddressLimit - sizeof(u32), Flags));
    CHECK(limit.ToRelative(Location(AddressLimit - sizeof(u32), Flags), 1) ==
          Location(0xFFC, Flags));

    const CodeImage crossing{AddressLimit - 0x1000, 0x1001};
    CHECK_FALSE(crossing.ToRelative(Location(AddressLimit - sizeof(u32), Flags), 1).has_value());
    CHECK_FALSE(crossing.ToAbsolute(Location(0xFFC, Flags), 1).has_value());
}

TEST_CASE("JitPrewarm rebase: offsets recorded at one base replay at another through the file",
          "[jit_prewarm_rebase]") {
    CHECK(FormatVersion == 3);

    constexpr u64 Offsets[] = {0, 0x1000, ImageSize / 2, ImageSize - 4096};
    std::vector<Entry> recorded;
    for (const u64 offset : Offsets) {
        const std::optional<u64> relative = Cold.ToRelative(Location(ColdBase + offset, Flags), 4);
        REQUIRE(relative.has_value());
        recorded.push_back(Entry{*relative, Hash ^ offset, 4});
    }

    const Identity identity = MakeIdentity();
    const std::vector<std::vector<Entry>> lists{recorded};
    const std::vector<u8> bytes = Serialize(identity, lists);
    const Parsed parsed = ParseVector(bytes, identity);
    CHECK(parsed.reject == Reject::None);
    CHECK(parsed.per_core == lists);

    // The file holds offsets; only the launch that loads it knows the addresses.
    REQUIRE(parsed.per_core.size() == 1);
    REQUIRE(parsed.per_core[0].size() == std::size(Offsets));
    for (std::size_t i = 0; i < std::size(Offsets); ++i) {
        const Entry& entry = parsed.per_core[0][i];
        const std::optional<u64> absolute = Warm.ToAbsolute(entry.location, entry.word_count);
        REQUIRE(absolute.has_value());
        CHECK(*absolute == Location(WarmBase + Offsets[i], Flags));
        CHECK(*absolute - Location(ColdBase + Offsets[i], Flags) == Delta);
    }
}

TEST_CASE("JitPrewarm rebase: an absolute address of the old format does not replay as an offset",
          "[jit_prewarm_rebase]") {
    // v1/v2 files stored guest addresses. A v3 reader cannot accept one even if it arrives with
    // valid checksums, and the version it would have had is rejected outright.
    for (const u64 offset : {u64{0}, u64{0x4000}, ImageSize - 4096}) {
        const u64 absolute = Location(ColdBase + offset, Flags);
        CHECK_FALSE(Warm.ToAbsolute(absolute, 1).has_value());
        CHECK_FALSE(Cold.ToAbsolute(absolute, 1).has_value());
    }

    const std::vector<std::vector<Entry>> lists{
        {Entry{Location(ColdBase + 0x8000, Flags), Hash, 1}}};
    const Identity identity = MakeIdentity();
    const Parsed parsed = ParseVector(Serialize(identity, lists), identity);
    REQUIRE(parsed.reject == Reject::None);
    REQUIRE(parsed.per_core.size() == 1);
    REQUIRE(parsed.per_core[0].size() == 1);
    CHECK_FALSE(
        Warm.ToAbsolute(parsed.per_core[0][0].location, parsed.per_core[0][0].word_count)
            .has_value());
}

TEST_CASE("JitPrewarm rebase: a hint span must lie inside one initial text segment",
          "[jit_prewarm_rebase]") {
    // ContainsInitialCode is what the record and replay paths gate on after the CodeImage
    // conversion; the process's published spans are text only, so reaching into the read-only or
    // writable data mapped beside a text can never be accepted. Exercised through the same
    // constexpr predicate the process uses, since building a loaded KProcess needs a full kernel.
    using Kernel::KProcess;

    SECTION("inside a module's text is accepted") {
        CHECK(KProcess::IsInsideInitialCode(TestCode, 0x1000, 4));      // first word
        CHECK(KProcess::IsInsideInitialCode(TestCode, 0x2000, 0x1000)); // whole text
        CHECK(KProcess::IsInsideInitialCode(TestCode, 0x2FFC, 4));      // last word
        CHECK(KProcess::IsInsideInitialCode(TestCode, 0x4000, 4));
        CHECK(KProcess::IsInsideInitialCode(TestCode, 0x5FFC, 4));
    }
    SECTION("ending past the text into the mapped data is refused") {
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode, 0x2FFC, 8));
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode, 0x5FFC, 8));
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode, 0x3000, 4)); // data after the text
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode, 0x3F00, 4)); // gap between modules
    }
    SECTION("one block cannot join two text segments") {
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode, 0x2FFC, 0x1008));
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode, 0x1FFC, 0x2008));
    }
    SECTION("a mapping outside the published text is not code for a hint") {
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode, 0x9000, 4));      // alias or other map
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode, 0x0, 4));         // below the image
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode, 0x8000, 0x2000)); // past the modules
    }
    SECTION("degenerate spans and publication order") {
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode, 0x1000, 0));
        CHECK_FALSE(KProcess::IsInsideInitialCode(TestCode,
                                                  std::numeric_limits<u64>::max() - 3, 8));
        CHECK_FALSE(KProcess::IsInsideInitialCode(std::span<const CodeSpans>{}, 0x1000, 4));
        CHECK(KProcess::IsInsideInitialCode(ReversedCode, 0x2FFC, 4));
        CHECK_FALSE(KProcess::IsInsideInitialCode(ReversedCode, 0x2FFC, 8));
    }
}
