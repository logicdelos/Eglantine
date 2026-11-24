// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
#include "video_core/texture_cache/feedback_cut.h"

namespace {

using namespace VideoCommon::FeedbackCut;

constexpr u64 AddressA = 0x1000;
constexpr u64 AddressB = 0x2000;

/// `count` bound targets, target i at 0x1000 * (i + 1), identity control map, colour mask `mask`
/// for every output unless `common` is off and masks[i] is set by the caller afterwards.
ColorSlots Targets(u32 count, bool common, u32 mask) {
    ColorSlots slots;
    slots.count = count;
    slots.common = common;
    for (size_t i = 0; i < NumColorSlots; ++i) {
        slots.bound[i] = i < count;
        slots.address[i] = 0x1000 * (i + 1);
        slots.map[i] = static_cast<u8>(i);
        slots.mask[i] = mask;
    }
    return slots;
}

struct TestExtent {
    u32 width;
    u32 height;
    u32 depth;
};
struct TestSubresource {
    int num_layers;
};
struct TestCopy {
    TestExtent extent;
    TestSubresource dst_subresource;
};

} // namespace

TEST_CASE("FeedbackCut a mask with any colour component writes", "[feedback_cut]") {
    REQUIRE(!MaskWrites(0));
    REQUIRE(MaskWrites(0x1));
    REQUIRE(MaskWrites(0x10));
    REQUIRE(MaskWrites(0x100));
    REQUIRE(MaskWrites(0x1000));
    REQUIRE(MaskWrites(0x1111));
    // Bits that are no component do not count
    REQUIRE(!MaskWrites(0x2222));
}

TEST_CASE("FeedbackCut common mask decides every slot", "[feedback_cut]") {
    ColorSlots slots = Targets(2, true, 0);
    REQUIRE(!TargetWritten(slots, AddressA));
    REQUIRE(!TargetWritten(slots, AddressB));

    // With the common mask only color_mask[0] counts, whatever the other entries hold
    slots.mask[1] = 0xF;
    REQUIRE(!TargetWritten(slots, AddressB));
    slots.mask[0] = 0x1000;
    REQUIRE(TargetWritten(slots, AddressA));
    REQUIRE(TargetWritten(slots, AddressB));
}

TEST_CASE("FeedbackCut per-slot masks decide their own slot", "[feedback_cut]") {
    ColorSlots slots = Targets(2, false, 0);
    slots.mask[1] = 0x1111;
    REQUIRE(!TargetWritten(slots, AddressA));
    REQUIRE(TargetWritten(slots, AddressB));
    REQUIRE(UnwrittenAttachments(slots) == 0b01u);

    slots.mask[0] = 0x10;
    REQUIRE(TargetWritten(slots, AddressA));
    REQUIRE(UnwrittenAttachments(slots) == 0u);

    slots.mask[0] = 0;
    slots.mask[1] = 0;
    REQUIRE(UnwrittenAttachments(slots) == 0b11u);
}

TEST_CASE("FeedbackCut two slots on one address: one writing makes the address written",
          "[feedback_cut]") {
    ColorSlots slots = Targets(2, false, 0);
    slots.address[1] = slots.address[0];
    REQUIRE(!TargetWritten(slots, AddressA));
    REQUIRE(AttachmentsAt(slots, AddressA) == 0b11u);

    slots.mask[1] = 0x1;
    REQUIRE(TargetWritten(slots, AddressA));
    // The slot that does not write itself is still unwritten for the modified flag
    REQUIRE(UnwrittenAttachments(slots) == 0b01u);
}

TEST_CASE("FeedbackCut what the registers cannot show stays written", "[feedback_cut]") {
    ColorSlots slots = Targets(2, true, 0);
    // No attachment at the address
    REQUIRE(TargetWritten(slots, 0x9000));
    REQUIRE(AttachmentsAt(slots, 0x9000) == 0u);

    // A slot past rt_control.count that no output maps to is not an attachment
    slots = Targets(1, true, 0);
    slots.bound[1] = true;
    REQUIRE(AttachmentsAt(slots, AddressB) == 0u);
    REQUIRE(TargetWritten(slots, AddressB));

    // An unbound slot (format NONE) at the address is not a target
    slots = Targets(2, true, 0);
    slots.bound[1] = false;
    REQUIRE(TargetWritten(slots, AddressB));
    REQUIRE(!TargetWritten(slots, AddressA));
    REQUIRE(UnwrittenAttachments(slots) == 0b01u);

    // rt_control.count past the eight slots is clamped
    slots = Targets(20, true, 0);
    REQUIRE(OutputCount(slots) == NumColorSlots);
    REQUIRE(UnwrittenAttachments(slots) == 0xFFu);
}

TEST_CASE("FeedbackCut a control map that moves outputs counts both readings",
          "[feedback_cut]") {
    // Output 0 -> position 1 and output 1 -> position 0. The host reads the masks by position,
    // the control map by output: a position unwritten in both readings is unwritten, a position
    // written in either is written.
    ColorSlots slots = Targets(2, false, 0);
    slots.map[0] = 1;
    slots.map[1] = 0;
    REQUIRE(!TargetWritten(slots, AddressA));
    REQUIRE(!TargetWritten(slots, AddressB));

    slots.mask[0] = 0xF; // position 0 by identity, position 1 through output 0
    REQUIRE(TargetWritten(slots, AddressA));
    REQUIRE(TargetWritten(slots, AddressB));
    REQUIRE(UnwrittenAttachments(slots) == 0u);

    slots.mask[0] = 0;
    slots.mask[1] = 0xF; // position 1 by identity, position 0 through output 1
    REQUIRE(TargetWritten(slots, AddressA));
    REQUIRE(TargetWritten(slots, AddressB));

    // A position the control map reaches beyond rt_control.count is an attachment by map only
    slots = Targets(1, false, 0);
    slots.bound[3] = true;
    slots.address[3] = 0x7000;
    slots.map[0] = 3;
    REQUIRE(AttachmentsAt(slots, 0x7000) == 0b1000u);
    REQUIRE(HostAttachmentsAt(slots, 0x7000) == 0u);
    REQUIRE(!TargetWritten(slots, 0x7000));
    slots.mask[0] = 0x1;
    REQUIRE(TargetWritten(slots, 0x7000));
}

TEST_CASE("FeedbackCut host attachments are the first rt_control.count slots", "[feedback_cut]") {
    ColorSlots slots = Targets(3, true, 0);
    slots.address[2] = slots.address[0];
    REQUIRE(HostAttachmentsAt(slots, AddressA) == 0b101u);
    REQUIRE(HostAttachmentsAt(slots, AddressB) == 0b010u);
    REQUIRE(HostAttachmentsAt(slots, 0x9000) == 0u);
}

TEST_CASE("FeedbackCut shadow share: same source with different views reuses one shadow",
          "[feedback_cut]") {
    ShadowShare<u32, u32> share;
    const u32 source = 7;
    REQUIRE(share.Find(source) == nullptr);
    // The first descriptor copies the source and records its shadow
    share.Add(source, 100);
    // Later descriptors of other views of the same source find it: the table is keyed by the
    // source image, not by the view
    const u32* const found = share.Find(source);
    REQUIRE(found != nullptr);
    REQUIRE(*found == 100);
    REQUIRE(*share.Find(source) == 100);
}

TEST_CASE("FeedbackCut shadow share: different sources keep their own shadows", "[feedback_cut]") {
    ShadowShare<u32, u32> share;
    share.Add(1, 10);
    share.Add(2, 20);
    REQUIRE(*share.Find(1) == 10);
    REQUIRE(*share.Find(2) == 20);
    REQUIRE(share.Find(3) == nullptr);
}

TEST_CASE("FeedbackCut shadow share past its capacity forgets the newest source",
          "[feedback_cut]") {
    ShadowShare<u32, u32, 2> share;
    share.Add(1, 10);
    share.Add(2, 20);
    share.Add(3, 30);
    REQUIRE(*share.Find(1) == 10);
    REQUIRE(*share.Find(2) == 20);
    REQUIRE(share.Find(3) == nullptr);
}

TEST_CASE("FeedbackCut copy pixels sum extent times layers", "[feedback_cut]") {
    const std::array<TestCopy, 3> copies{{
        {{880, 880, 1}, {1}},
        {{4, 4, 1}, {6}},
        {{2, 2, 2}, {0}}, // no layers counts as one
    }};
    REQUIRE(CopyPixels(copies) == 880u * 880u + 4u * 4u * 6u + 2u * 2u * 2u);
    REQUIRE(CopyPixels(std::array<TestCopy, 0>{}) == u64{0});
}

TEST_CASE("Nx2Profile image copies by caller: per frame copies and MP", "[feedback_cut]") {
    using VideoCommon::Nx2Profile::ImageCopyProbe;
    ImageCopyProbe probe;
    REQUIRE(probe.Format(2).empty());

    probe.Add(ImageCopyProbe::Caller::FeedbackShadow, 1'000'000);
    probe.Add(ImageCopyProbe::Caller::FeedbackShadow, 1'000'000);
    probe.Add(ImageCopyProbe::Caller::FeedbackShadow, 1'000'000);
    probe.Add(ImageCopyProbe::Caller::AliasSync, 2'000'000);
    const std::string line = probe.Format(2);
    REQUIRE(line.starts_with("Image copies by caller, per frame: feedback shadow 1.5 copies / "
                             "1.5 MP, alias sync 0.5 copies / 1.0 MP, join alias 0.0 copies / "
                             "0.0 MP, join overlap 0.0 copies / 0.0 MP"));
    REQUIRE(line.find("total 2.0 copies / 2.5 MP") != std::string::npos);
}

TEST_CASE("Nx2Profile feedback probe reports the shadows the cuts avoided", "[feedback_cut]") {
    using Avoided = VideoCommon::Nx2Profile::FeedbackProbe::Avoided;
    VideoCommon::Nx2Profile::FeedbackProbe probe;
    REQUIRE(probe.Format().empty());

    probe.NoteAvoided(Avoided::Shared);
    probe.NoteAvoided(Avoided::Shared);
    probe.NoteAvoided(Avoided::Unwritten);
    probe.NoteAvoided(Avoided::Unmodified);
    const std::string line = probe.Format();
    REQUIRE(line.starts_with("Feedback shadow copies per frame: 0.0 copies in 0.0 draws"));
    REQUIRE(line.find("shadow copies avoided per frame: 2.0 shared within the draw, 1.0 target "
                      "sampled in place") != std::string::npos);
    REQUIRE(line.find("flagged modified by a draw with mask 0: 1.0") != std::string::npos);
}

TEST_CASE("Nx2Perf feedback cuts are named, on by default and off under SAFE", "[feedback_cut]") {
    using namespace VideoCommon::Nx2Perf;
    REQUIRE(opt_names[static_cast<size_t>(Opt::FeedbackShadowShare)] == "feedback_shadow_share");
    REQUIRE(opt_names[static_cast<size_t>(Opt::FeedbackSkipUnwritten)] ==
            "feedback_skip_unwritten");
    REQUIRE(opt_names[static_cast<size_t>(Opt::MaskedTargetUnmodified)] ==
            "masked_target_unmodified");
    for (const Opt opt :
         {Opt::FeedbackShadowShare, Opt::FeedbackSkipUnwritten, Opt::MaskedTargetUnmodified}) {
        const size_t index = static_cast<size_t>(opt);
        REQUIRE((OptInMask() & Bit(opt)) == 0);
        REQUIRE(IsOff(DefaultOffMask(), index));
        REQUIRE(!IsOff(ParseOffMask(false, "", ""), index));
        REQUIRE(IsOff(ParseOffMask(true, "", ""), index));
        REQUIRE(IsOff(ParseOffMask(false, "", opt_names[index]), index));
        REQUIRE(!IsOff(ParseOffMask(false, opt_names[index], ""), index));
        REQUIRE(!IsOff(ParseOffMask(true, opt_names[index], ""), index));
    }
}
