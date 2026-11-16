// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>

#include "common/common_types.h"

namespace Tegra::Engines {

struct NativeUpscaleRegions {
    // Half-open coordinates; allocation dimensions stay unchanged for tiled addressing.
    std::array<u32, 4> source;
    std::array<u32, 4> destination;
};

// The compact DLSS parameter layout of one-dimensional thread blocks has a dispatch header,
// followed by inclusive source/destination rectangles at 0x10/0x20. Words 1-3 repeat the grid;
// match them before using this layout. Word 0 is not the block width: Donkey Kong Bananza stores
// 128 with 128-thread blocks, Xenoblade Chronicles 2 stores 160 with 128-thread blocks.
inline std::optional<NativeUpscaleRegions> DecodeNativeUpscaleRegions(
    std::span<const u32> words, const std::array<u32, 3>& block, const std::array<u32, 3>& grid,
    const std::array<u32, 2>& source_size, const std::array<u32, 2>& destination_size) {
    if (words.size() < 12 || block[1] != 1 || block[2] != 1 ||
        !std::equal(grid.begin(), grid.end(), words.begin() + 1)) {
        return std::nullopt;
    }
    const auto rectangle = [](const u32* data, const std::array<u32, 2>& size)
        -> std::optional<std::array<u32, 4>> {
        if (data[0] > data[2] || data[1] > data[3] || data[2] >= size[0] || data[3] >= size[1]) {
            return std::nullopt;
        }
        return std::array<u32, 4>{data[0], data[1], data[2] + 1, data[3] + 1};
    };
    const auto source = rectangle(words.data() + 4, source_size);
    const auto destination = rectangle(words.data() + 8, destination_size);
    if (!source || !destination || (*destination)[2] - (*destination)[0] < (*source)[2] - (*source)[0] ||
        (*destination)[3] - (*destination)[1] < (*source)[3] - (*source)[1]) {
        return std::nullopt;
    }
    return NativeUpscaleRegions{*source, *destination};
}

template <typename Image, typename ReadImage, typename IsUpscalePair>
std::optional<u32> FindNativeUpscaleOutput(const std::optional<Image>& input,
                                         std::span<const u32> stores, ReadImage&& image,
                                         IsUpscalePair&& upscale_pair) {
    if (!input) return std::nullopt;
    for (const u32 store : stores) {
        if (upscale_pair(input, image(store), false)) return store;
    }
    return std::nullopt;
}

template <typename ReadImage, typename IsUpscalePair>
std::optional<std::pair<u32, u32>> FindNativeUpscaleSlots(
    std::span<const u32> reads, std::span<const u32> stores, ReadImage&& image,
    IsUpscalePair&& upscale_pair) {
    for (const u32 store : stores) {
        const auto to = image(store);
        for (const bool same_format : {true, false}) {
            std::optional<std::pair<u32, u32>> selected;
            u64 area{};
            for (const u32 read : reads) {
                const auto from = image(read);
                if (upscale_pair(from, to, same_format) &&
                    (!selected || u64{from->Width()} * from->Height() > area)) {
                    selected = std::pair{read, store};
                    area = u64{from->Width()} * from->Height();
                }
            }
            if (selected) {
                return selected;
            }
        }
    }
    return std::nullopt;
}

} // namespace Tegra::Engines
