// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/div_ceil.h"
#include "common/thread_worker.h"
#include "video_core/gpu.h"
#include "video_core/textures/decoders.h"

namespace Tegra::Texture {
namespace {
constexpr u32 NativeGobOffset(u32 offset, bool native_tiling) {
    if (!native_tiling) return offset;
    const u32 difference = ((offset >> 5) ^ (offset >> 6)) & 1;
    return offset ^ (difference << 5) ^ (difference << 6);
}
template <u32 mask>
constexpr u32 pdep(u32 value) {
    u32 result = 0;
    u32 m = mask;
    for (u32 bit = 1; m; bit += bit) {
        if (value & bit)
            result |= m & (~m + 1);
        m &= m - 1;
    }
    return result;
}

template <u32 mask, u32 incr_amount>
void incrpdep(u32& value) {
    static constexpr u32 swizzled_incr = pdep<mask>(incr_amount);
    value = ((value | ~mask) + swizzled_incr) & mask;
}

template <bool TO_LINEAR, u32 BYTES_PER_PIXEL, bool NATIVE_MASKS = false>
void SwizzleImpl(std::span<u8> output, std::span<const u8> input, u32 width, u32 height, u32 depth,
                 u32 block_height, u32 block_depth, u32 stride, bool native_tiling) {
    // Native GOBs exchange bits 5 and 6; deposit them directly instead of swapping each element.
    static constexpr u32 x_bits = NativeGobOffset(SWIZZLE_X_BITS, NATIVE_MASKS);
    static constexpr u32 y_bits = NativeGobOffset(SWIZZLE_Y_BITS, NATIVE_MASKS);
    // The origin of the transformation can be configured here, leave it as zero as the current API
    // doesn't expose it.
    static constexpr u32 origin_x = 0;
    static constexpr u32 origin_y = 0;
    static constexpr u32 origin_z = 0;

    // We can configure here a custom pitch
    // As it's not exposed 'width * BYTES_PER_PIXEL' will be the expected pitch.
    const u32 pitch = width * BYTES_PER_PIXEL;

    const u32 gobs_in_x = Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT);
    const u32 block_size = gobs_in_x << (GOB_SIZE_SHIFT + block_height + block_depth);
    const u32 slice_size =
        Common::DivCeilLog2(height, block_height + GOB_SIZE_Y_SHIFT) * block_size;

    const u32 block_height_mask = (1U << block_height) - 1;
    const u32 block_depth_mask = (1U << block_depth) - 1;
    const u32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;

    // Both swizzle masks and the native GOB twist only touch bits 4 and up of the byte offset, so
    // a 16-byte run that starts on a 16-byte boundary in one layout stays one 16-byte run in the
    // other. Copy whole chunks instead of one pixel at a time: a 4-bytes-per-pixel tile drops from
    // four offset computations and four 4-byte copies to one of each per 16 bytes.
    constexpr u32 CHUNK_BYTES = 16;
    constexpr bool CAN_CHUNK = BYTES_PER_PIXEL <= CHUNK_BYTES && CHUNK_BYTES % BYTES_PER_PIXEL == 0;
    constexpr u32 CHUNK_COLUMNS = CAN_CHUNK ? CHUNK_BYTES / BYTES_PER_PIXEL : 1;

    for (u32 slice = 0; slice < depth; ++slice) {
        const u32 z = slice + origin_z;
        const u32 offset_z = (z >> block_depth) * slice_size +
                             ((z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));
        for (u32 line = 0; line < height; ++line) {
            const u32 y = line + origin_y;
            const u32 swizzled_y = pdep<y_bits>(y);

            const u32 block_y = y >> GOB_SIZE_Y_SHIFT;
            const u32 offset_y = (block_y >> block_height) * block_size +
                                 ((block_y & block_height_mask) << GOB_SIZE_SHIFT);
            const u32 line_offset = slice * pitch * height + line * pitch;

            u32 swizzled_x = pdep<x_bits>(origin_x * BYTES_PER_PIXEL);
            u32 column = 0;
            if constexpr (CAN_CHUNK) {
                const u32 chunk_end = width - width % CHUNK_COLUMNS;
                for (; column < chunk_end; column += CHUNK_COLUMNS) {
                    const u32 x = (column + origin_x) * BYTES_PER_PIXEL;
                    const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;

                    const u32 base_swizzled_offset = offset_z + offset_y + offset_x;
                    const u32 element_offset = base_swizzled_offset + (swizzled_x | swizzled_y);
                    const u32 swizzled_offset = NATIVE_MASKS
                                                   ? element_offset
                                                   : NativeGobOffset(element_offset, native_tiling);

                    const u32 unswizzled_offset = line_offset + column * BYTES_PER_PIXEL;

                    u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                    const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];
                    std::memcpy(dst, src, CHUNK_BYTES);
                    // The same advance CHUNK_COLUMNS pixel steps make: the masked add carries
                    // through the permuted bits like the per-pixel increments do.
                    incrpdep<x_bits, CHUNK_BYTES>(swizzled_x);
                }
            }
            for (; column < width; ++column, incrpdep<x_bits, BYTES_PER_PIXEL>(swizzled_x)) {
                const u32 x = (column + origin_x) * BYTES_PER_PIXEL;
                const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;

                const u32 base_swizzled_offset = offset_z + offset_y + offset_x;
                const u32 element_offset = base_swizzled_offset + (swizzled_x | swizzled_y);
                const u32 swizzled_offset = NATIVE_MASKS
                                               ? element_offset
                                               : NativeGobOffset(element_offset, native_tiling);

                const u32 unswizzled_offset = line_offset + column * BYTES_PER_PIXEL;

                u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];

                std::memcpy(dst, src, BYTES_PER_PIXEL);
            }
        }
    }
}

template <bool TO_LINEAR, u32 BYTES_PER_PIXEL>
void SwizzleSubrectImpl(std::span<u8> output, std::span<const u8> input, u32 width, u32 height,
                        u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 num_lines,
                        u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling) {
    // The origin of the transformation can be configured here, leave it as zero as the current API
    // doesn't expose it.
    static constexpr u32 origin_z = 0;

    // We can configure here a custom pitch
    // As it's not exposed 'width * BYTES_PER_PIXEL' will be the expected pitch.
    const u32 pitch = pitch_linear;
    const u32 stride = Common::AlignUpLog2(width * BYTES_PER_PIXEL, GOB_SIZE_X_SHIFT);

    const u32 gobs_in_x = Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT);
    const u32 block_size = gobs_in_x << (GOB_SIZE_SHIFT + block_height + block_depth);
    const u32 slice_size =
        Common::DivCeilLog2(height, block_height + GOB_SIZE_Y_SHIFT) * block_size;

    const u32 block_height_mask = (1U << block_height) - 1;
    const u32 block_depth_mask = (1U << block_depth) - 1;
    const u32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;

    u32 unprocessed_lines = num_lines;
    u32 extent_y = (std::min)(num_lines, height - origin_y);

    for (u32 slice = 0; slice < depth; ++slice) {
        const u32 z = slice + origin_z;
        const u32 offset_z = (z >> block_depth) * slice_size +
                             ((z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));
        const u32 lines_in_y = (std::min)(unprocessed_lines, extent_y);
        for (u32 line = 0; line < lines_in_y; ++line) {
            const u32 y = line + origin_y;
            const u32 swizzled_y = pdep<SWIZZLE_Y_BITS>(y);

            const u32 block_y = y >> GOB_SIZE_Y_SHIFT;
            const u32 offset_y = (block_y >> block_height) * block_size +
                                 ((block_y & block_height_mask) << GOB_SIZE_SHIFT);

            u32 swizzled_x = pdep<SWIZZLE_X_BITS>(origin_x * BYTES_PER_PIXEL);
            for (u32 column = 0; column < extent_x;
                 ++column, incrpdep<SWIZZLE_X_BITS, BYTES_PER_PIXEL>(swizzled_x)) {
                const u32 x = (column + origin_x) * BYTES_PER_PIXEL;
                const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;

                const u32 base_swizzled_offset = offset_z + offset_y + offset_x;
                const u32 swizzled_offset = NativeGobOffset(base_swizzled_offset + (swizzled_x | swizzled_y), native_tiling);

                const u32 unswizzled_offset =
                    slice * pitch * height + line * pitch + column * BYTES_PER_PIXEL;

                u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];

                std::memcpy(dst, src, BYTES_PER_PIXEL);
            }
        }
        unprocessed_lines -= lines_in_y;
        if (unprocessed_lines == 0) {
            return;
        }
    }
}

template <bool TO_LINEAR>
void Swizzle(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
             u32 height, u32 depth, u32 block_height, u32 block_depth, u32 stride_alignment, bool native_tiling) {
    static const bool native_masks = [] {
        const char* value = std::getenv("EDEN_NX2_NATIVE_SWIZZLE_MASKS");
        return value != nullptr && value[0] == '1';
    }();
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        if (native_tiling && native_masks) {                                                        \
            return SwizzleImpl<TO_LINEAR, x, true>(output, input, width, height, depth, block_height, \
                                                    block_depth, stride_alignment, true);           \
        }                                                                                          \
        return SwizzleImpl<TO_LINEAR, x>(output, input, width, height, depth, block_height,        \
                                         block_depth, stride_alignment, native_tiling);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

/// Unswizzles `height` rows. `stride` is the padded row size of the whole image, so a run of
/// block rows taken from the middle of it unswizzles like the same rows of the whole.
void UnswizzleRun(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                  u32 height, u32 depth, u32 block_height, u32 block_depth, u32 stride,
                  bool native_tiling) {
    const u32 new_bpp = (std::min)(4U, static_cast<u32>(std::countr_zero(width * bytes_per_pixel)));
    width = (width * bytes_per_pixel) >> new_bpp;
    bytes_per_pixel = 1U << new_bpp;
    Swizzle<false>(output, input, bytes_per_pixel, width, height, depth, block_height, block_depth,
                   stride, native_tiling);
}

// EDEN_NX2_UNSWIZZLE_THREADS=n: threads that unswizzle one large image, the caller's included (1
// keeps every image on the calling thread). Without it a quarter of the host's threads, 1 to 4.
u32 UnswizzleThreads() {
    static const u32 count = [] {
        const char* const value = std::getenv("EDEN_NX2_UNSWIZZLE_THREADS");
        if (value != nullptr && value[0] != '\0') {
            return static_cast<u32>((std::clamp)(std::strtol(value, nullptr, 10), 1L, 16L));
        }
        return (std::clamp)(std::thread::hardware_concurrency() / 4, 1U, 4U);
    }();
    return count;
}

/// Threads that only take runs of block rows. Made on the first image worth splitting.
Common::ThreadWorker& UnswizzlePool() {
    static Common::ThreadWorker pool{(std::max)(UnswizzleThreads(), 4U) - 1, "TexUnswizzle"};
    return pool;
}

/// The runs of one image and who has taken them. Shared with the queued helpers: one that starts
/// after every run is taken only reads the counters, so the caller never waits for it.
struct BandJob {
    std::vector<UnswizzleBand> bands;
    std::span<u8> output;
    std::span<const u8> input;
    u32 bytes_per_pixel = 0;
    u32 width = 0;
    u32 block_height = 0;
    u32 block_depth = 0;
    u32 stride = 0;
    bool native_tiling = false;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::mutex mutex;
    std::condition_variable finished;
};

void RunBands(BandJob& job) {
    const std::size_t count = job.bands.size();
    for (;;) {
        const std::size_t index = job.next.fetch_add(1, std::memory_order_relaxed);
        if (index >= count) {
            return;
        }
        const UnswizzleBand& band = job.bands[index];
        UnswizzleRun(job.output.subspan(band.dst_offset), job.input.subspan(band.src_offset),
                     job.bytes_per_pixel, job.width, band.rows, 1, job.block_height,
                     job.block_depth, job.stride, job.native_tiling);
        if (job.done.fetch_add(1, std::memory_order_acq_rel) + 1 == count) {
            std::scoped_lock lock{job.mutex};
            job.finished.notify_all();
        }
    }
}

} // Anonymous namespace

std::vector<UnswizzleBand> PlanUnswizzleBands(u32 bytes_per_pixel, u32 width, u32 height,
                                              u32 block_height, u32 block_depth,
                                              u32 stride_alignment, u32 parts) {
    std::vector<UnswizzleBand> bands;
    const u32 band_rows = GOB_SIZE_Y << block_height;
    const u32 band_count = Common::DivCeil(height, band_rows);
    if (band_count == 0) {
        return bands;
    }
    parts = (std::min)((std::max)(parts, 1U), band_count);
    // The layout UnswizzleTexture reads: a block row is one block per 64 bytes of the padded row,
    // each block 512 bytes a GOB, 2^block_height GOBs (and 2^block_depth slices) tall.
    const u32 stride = Common::AlignUpLog2(width, stride_alignment) * bytes_per_pixel;
    const std::size_t band_bytes = std::size_t{Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT)}
                                   << (GOB_SIZE_SHIFT + block_height + block_depth);
    const std::size_t row_bytes = std::size_t{width} * bytes_per_pixel;
    const u32 per_part = band_count / parts;
    const u32 longer = band_count % parts;
    bands.reserve(parts);
    u32 first_band = 0;
    for (u32 part = 0; part < parts; ++part) {
        const u32 count = per_part + (part < longer ? 1U : 0U);
        const u32 first_row = first_band * band_rows;
        bands.push_back(UnswizzleBand{
            .src_offset = std::size_t{first_band} * band_bytes,
            .dst_offset = std::size_t{first_row} * row_bytes,
            .rows = (std::min)(count * band_rows, height - first_row),
        });
        first_band += count;
    }
    return bands;
}

u32 UnswizzleParts(u64 output_bytes, u32 bands, u32 threads) {
    constexpr u64 MinPartBytes = u64{1} << 20;
    const u64 parts = (std::min)((std::min)(u64{threads}, u64{bands}), output_bytes / MinPartBytes);
    return static_cast<u32>((std::max)(parts, u64{1}));
}

void UnswizzleTextureParallel(std::span<u8> output, std::span<const u8> input,
                              u32 bytes_per_pixel, u32 width, u32 height, u32 depth,
                              u32 block_height, u32 block_depth, u32 stride_alignment,
                              bool native_tiling, u32 parts) {
    const u32 stride = Common::AlignUpLog2(width, stride_alignment) * bytes_per_pixel;
    std::vector<UnswizzleBand> bands;
    if (depth == 1 && parts > 1) {
        bands = PlanUnswizzleBands(bytes_per_pixel, width, height, block_height, block_depth,
                                   stride_alignment, parts);
    }
    if (bands.size() < 2) {
        UnswizzleRun(output, input, bytes_per_pixel, width, height, depth, block_height,
                     block_depth, stride, native_tiling);
        return;
    }
    const auto job = std::make_shared<BandJob>();
    job->bands = std::move(bands);
    job->output = output;
    job->input = input;
    job->bytes_per_pixel = bytes_per_pixel;
    job->width = width;
    job->block_height = block_height;
    job->block_depth = block_depth;
    job->stride = stride;
    job->native_tiling = native_tiling;
    for (std::size_t helper = 1; helper < job->bands.size(); ++helper) {
        UnswizzlePool().QueueWork([job] { RunBands(*job); });
    }
    RunBands(*job);
    std::unique_lock lock{job->mutex};
    job->finished.wait(lock, [&job] {
        return job->done.load(std::memory_order_acquire) == job->bands.size();
    });
}

void UnswizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                      u32 stride_alignment, bool native_tiling) {
    if (depth == 1) {
        const u32 parts = UnswizzleParts(u64{width} * bytes_per_pixel * height,
                                         Common::DivCeil(height, GOB_SIZE_Y << block_height),
                                         UnswizzleThreads());
        if (parts > 1) {
            return UnswizzleTextureParallel(output, input, bytes_per_pixel, width, height, depth,
                                            block_height, block_depth, stride_alignment,
                                            native_tiling, parts);
        }
    }
    const u32 stride = Common::AlignUpLog2(width, stride_alignment) * bytes_per_pixel;
    UnswizzleRun(output, input, bytes_per_pixel, width, height, depth, block_height, block_depth,
                 stride, native_tiling);
}

void SwizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 block_height, u32 block_depth,
                    u32 stride_alignment, bool native_tiling) {
    const u32 stride = Common::AlignUpLog2(width, stride_alignment) * bytes_per_pixel;
    const u32 new_bpp = (std::min)(4U, static_cast<u32>(std::countr_zero(width * bytes_per_pixel)));
    width = (width * bytes_per_pixel) >> new_bpp;
    bytes_per_pixel = 1U << new_bpp;
    Swizzle<true>(output, input, bytes_per_pixel, width, height, depth, block_height, block_depth,
                  stride, native_tiling);
}

void SwizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 extent_y,
                    u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        return SwizzleSubrectImpl<true, x>(output, input, width, height, depth, origin_x,          \
                                           origin_y, extent_x, extent_y, block_height,             \
                                           block_depth, pitch_linear, native_tiling);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

void UnswizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x,
                      u32 extent_y, u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        return SwizzleSubrectImpl<false, x>(output, input, width, height, depth, origin_x,         \
                                            origin_y, extent_x, extent_y, block_height,            \
                                            block_depth, pitch_linear, native_tiling);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

std::size_t CalculateSize(bool tiled, u32 bytes_per_pixel, u32 width, u32 height, u32 depth,
                          u32 block_height, u32 block_depth) {
    if (tiled) {
        const u32 aligned_width = Common::AlignUpLog2(width * bytes_per_pixel, GOB_SIZE_X_SHIFT);
        const u32 aligned_height = Common::AlignUpLog2(height, GOB_SIZE_Y_SHIFT + block_height);
        const u32 aligned_depth = Common::AlignUpLog2(depth, GOB_SIZE_Z_SHIFT + block_depth);
        return aligned_width * aligned_height * aligned_depth;
    } else {
        return width * height * depth * bytes_per_pixel;
    }
}

u64 GetGOBOffset(u32 width, u32 height, u32 dst_x, u32 dst_y, u32 block_height,
                 u32 bytes_per_pixel) {
    auto div_ceil = [](const u32 x, const u32 y) { return ((x + y - 1) / y); };
    const u32 gobs_in_block = 1 << block_height;
    const u32 y_blocks = GOB_SIZE_Y << block_height;
    const u32 x_per_gob = GOB_SIZE_X / bytes_per_pixel;
    const u32 x_blocks = div_ceil(width, x_per_gob);
    const u32 block_size = GOB_SIZE * gobs_in_block;
    const u32 stride = block_size * x_blocks;
    const u32 base = (dst_y / y_blocks) * stride + (dst_x / x_per_gob) * block_size;
    const u32 relative_y = dst_y % y_blocks;
    return base + (relative_y / GOB_SIZE_Y) * GOB_SIZE;
}

} // namespace Tegra::Texture
