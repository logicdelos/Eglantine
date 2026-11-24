// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

#include "common/common_types.h"

// The decisions of the texture cache's feedback shadow cuts (Nx2Perf feedback_shadow_share,
// feedback_skip_unwritten, masked_target_unmodified), as plain functions of register values so a
// test can run them without a GPU. The cache fills a ColorSlots from the Maxwell3D registers.
namespace VideoCommon::FeedbackCut {

inline constexpr size_t NumColorSlots = 8;

/// The cuts whose "the draw cannot write this target" claim Nx2Perf verify checks against the
/// host's resolved colour write mask (see TextureCache::TakeUnwrittenClaims).
enum class Cut : size_t {
    SkipUnwritten,    ///< feedback_skip_unwritten: no shadow copy
    MaskedUnmodified, ///< masked_target_unmodified: the target is not flagged as modified
    Count,
};

/// What asked the texture cache's runtime for an image copy (the Nx2Profile image copy counter).
enum class CopyCaller : u32 {
    FeedbackShadow, ///< ReplaceColorFeedbackViews: the shadow of a sampled colour target
    AliasSync,      ///< SynchronizeAliases: an alias brought up to date before its use
    JoinAlias,      ///< JoinImages: an alias copied into the image being created
    JoinOverlap,    ///< JoinImages: an overlapping image's data copied into the new one
    Count,
};

/// What a draw's colour registers say, by render target register position (regs.rt[position]).
struct ColorSlots {
    u32 count{};   ///< rt_control.count
    bool common{}; ///< color_mask_common: color_mask[0] applies to every output
    std::array<bool, NumColorSlots> bound{};  ///< rt[position].format != NONE
    std::array<u64, NumColorSlots> address{}; ///< rt[position].Address()
    std::array<u8, NumColorSlots> map{};      ///< rt_control.Map(output)
    std::array<u32, NumColorSlots> mask{};    ///< color_mask[index].raw
};

/// A Maxwell colour mask (R, G, B, A at bits 0, 4, 8 and 12) that lets any component through.
constexpr bool MaskWrites(u32 raw) {
    return (raw & 0x1111) != 0;
}

constexpr u32 OutputCount(const ColorSlots& slots) {
    return (std::min)(slots.count, static_cast<u32>(NumColorSlots));
}

/// Whether the mask the guest set for output `index` lets the draw write.
constexpr bool OutputWrites(const ColorSlots& slots, size_t index) {
    return MaskWrites(slots.mask[slots.common ? 0 : index]);
}

/// Whether register position `position` is an attachment of the draw: the Vulkan backend binds
/// rt[i] as attachment i for i < count, the cache's own lookups reach rt[Map(output)].
constexpr bool SlotAttached(const ColorSlots& slots, size_t position) {
    const u32 count = OutputCount(slots);
    if (position < count) {
        return true;
    }
    for (u32 output = 0; output < count; ++output) {
        if (static_cast<size_t>(slots.map[output]) == position) {
            return true;
        }
    }
    return false;
}

/// Whether the draw may write what register position `position` holds. The host indexes the
/// colour masks by attachment (= position), the control map would index them by output: when they
/// disagree the position counts as written unless both masks are zero.
constexpr bool SlotWritten(const ColorSlots& slots, size_t position) {
    const u32 count = OutputCount(slots);
    if (position < count && OutputWrites(slots, position)) {
        return true;
    }
    for (u32 output = 0; output < count; ++output) {
        if (static_cast<size_t>(slots.map[output]) == position && OutputWrites(slots, output)) {
            return true;
        }
    }
    return false;
}

/// Bit p set: register position p holds a bound target at `address` that the draw attaches.
constexpr u32 AttachmentsAt(const ColorSlots& slots, u64 address) {
    u32 positions = 0;
    for (size_t position = 0; position < NumColorSlots; ++position) {
        if (slots.bound[position] && slots.address[position] == address &&
            SlotAttached(slots, position)) {
            positions |= 1u << position;
        }
    }
    return positions;
}

/// Whether the draw may write the target at `address`. True when no attachment is found there:
/// what the registers cannot prove stays written.
constexpr bool TargetWritten(const ColorSlots& slots, u64 address) {
    const u32 positions = AttachmentsAt(slots, address);
    if (positions == 0) {
        return true;
    }
    for (size_t position = 0; position < NumColorSlots; ++position) {
        if ((positions >> position) % 2 != 0 && SlotWritten(slots, position)) {
            return true;
        }
    }
    return false;
}

/// Bit p set: attachment p (a bound target of the draw) cannot be written by it.
constexpr u32 UnwrittenAttachments(const ColorSlots& slots) {
    u32 unwritten = 0;
    const u32 count = OutputCount(slots);
    for (u32 position = 0; position < count; ++position) {
        if (slots.bound[position] && !SlotWritten(slots, position)) {
            unwritten |= 1u << position;
        }
    }
    return unwritten;
}

/// Bit p set: attachment p holds a bound target at `address` (the host attachments among
/// AttachmentsAt).
constexpr u32 HostAttachmentsAt(const ColorSlots& slots, u64 address) {
    return AttachmentsAt(slots, address) &
           ((u32{1} << OutputCount(slots)) - 1);
}

/// One draw's source image -> shadow image table: the second and later descriptors that sample
/// the same source reuse the shadow the draw already copied (a copy of one snapshot, taken before
/// the draw). Fixed capacity: a source past it is not remembered, its later descriptors copy
/// again, as they did before the table.
template <class Source, class Shadow, size_t Capacity = 16>
class ShadowShare {
public:
    [[nodiscard]] const Shadow* Find(const Source& source) const noexcept {
        for (size_t i = 0; i < size; ++i) {
            if (entries[i].source == source) {
                return &entries[i].shadow;
            }
        }
        return nullptr;
    }

    void Add(const Source& source, const Shadow& shadow) {
        if (size < Capacity) {
            entries[size++] = Entry{source, shadow};
        }
    }

private:
    struct Entry {
        Source source{};
        Shadow shadow{};
    };
    std::array<Entry, Capacity> entries{};
    size_t size{};
};

/// Texels a list of image copies writes (extent times the layers of each region).
template <class Copies>
[[nodiscard]] u64 CopyPixels(const Copies& copies) {
    u64 pixels = 0;
    for (const auto& copy : copies) {
        pixels += u64{copy.extent.width} * copy.extent.height * copy.extent.depth *
                  static_cast<u64>((std::max)(copy.dst_subresource.num_layers, 1));
    }
    return pixels;
}

} // namespace VideoCommon::FeedbackCut
