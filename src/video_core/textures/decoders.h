// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/common_types.h"
#include "video_core/textures/texture.h"

namespace Tegra::Texture {

constexpr u32 GOB_SIZE_X = 64;
constexpr u32 GOB_SIZE_Y = 8;
constexpr u32 GOB_SIZE_Z = 1;
constexpr u32 GOB_SIZE = GOB_SIZE_X * GOB_SIZE_Y * GOB_SIZE_Z;

constexpr u32 GOB_SIZE_X_SHIFT = 6;
constexpr u32 GOB_SIZE_Y_SHIFT = 3;
constexpr u32 GOB_SIZE_Z_SHIFT = 0;
constexpr u32 GOB_SIZE_SHIFT = GOB_SIZE_X_SHIFT + GOB_SIZE_Y_SHIFT + GOB_SIZE_Z_SHIFT;

constexpr u32 SWIZZLE_X_BITS = 0b100101111;
constexpr u32 SWIZZLE_Y_BITS = 0b011010000;

/// Unswizzles a block linear texture into linear memory.
void UnswizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                      u32 stride_alignment = 1, bool native_tiling = false);

/// One run of whole block rows of a single-slice block linear image.
struct UnswizzleBand {
    std::size_t src_offset; ///< Into the block linear input
    std::size_t dst_offset; ///< Into the linear output
    u32 rows;               ///< Texel rows in the run
};

/// Splits the rows of a single-slice block linear image into at most `parts` runs of whole block
/// rows (a block row is 8 << block_height rows). The runs hold the block rows in order, the first
/// ones one block row longer when the count does not divide evenly, and between them they cover
/// every row once. An image without rows gives no runs.
[[nodiscard]] std::vector<UnswizzleBand> PlanUnswizzleBands(u32 bytes_per_pixel, u32 width,
                                                            u32 height, u32 block_height,
                                                            u32 block_depth, u32 stride_alignment,
                                                            u32 parts);

/// How many runs an image of `output_bytes` over `bands` block rows is worth splitting into on
/// `threads` threads: no more than there are threads or block rows, and 1 MiB of output each.
/// Never less than one.
[[nodiscard]] u32 UnswizzleParts(u64 output_bytes, u32 bands, u32 threads);

/// UnswizzleTexture with the block rows of a single-slice image split into `parts` runs. The
/// texture unswizzle threads take runs and the caller takes its share, so it returns once every
/// run is done. The bytes written are the bytes UnswizzleTexture writes; a 3D image, or one with
/// fewer than two block rows, is unswizzled as one run.
void UnswizzleTextureParallel(std::span<u8> output, std::span<const u8> input,
                              u32 bytes_per_pixel, u32 width, u32 height, u32 depth,
                              u32 block_height, u32 block_depth, u32 stride_alignment,
                              bool native_tiling, u32 parts);

/// Swizzles linear memory into a block linear texture.
void SwizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 block_height, u32 block_depth,
                    u32 stride_alignment = 1, bool native_tiling = false);

/// This function calculates the correct size of a texture depending if it's tiled or not.
std::size_t CalculateSize(bool tiled, u32 bytes_per_pixel, u32 width, u32 height, u32 depth,
                          u32 block_height, u32 block_depth);

/// Copies an untiled subrectangle into a tiled surface.
void SwizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 extent_y,
                    u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling = false);

/// Copies a tiled subrectangle into a linear surface.
void UnswizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x,
                      u32 extent_y, u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling = false);

/// Obtains the offset of the gob for positions 'dst_x' & 'dst_y'
u64 GetGOBOffset(u32 width, u32 height, u32 dst_x, u32 dst_y, u32 block_height,
                 u32 bytes_per_pixel);

} // namespace Tegra::Texture
