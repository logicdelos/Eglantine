// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A large image is unswizzled in runs of whole block rows on several threads. These tests pin the
// runs of a plan, how many runs an image earns, and that the bytes written do not depend on how
// many runs there are. Inputs are generated; nothing waits on a clock.

#include <algorithm>
#include <cstddef>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "video_core/textures/decoders.h"

namespace {

using namespace Tegra::Texture;

constexpr u64 MiB = u64{1} << 20;

struct Shape {
    u32 bytes_per_pixel;
    u32 width;
    u32 height;
    u32 block_height;
    u32 stride_alignment;
};

u32 BandRows(const Shape& shape) {
    return GOB_SIZE_Y << shape.block_height;
}

/// Bytes of one block row of the block linear layout.
std::size_t BandBytes(const Shape& shape, u32 block_depth) {
    const u32 stride = Common::AlignUpLog2(shape.width, shape.stride_alignment) *
                       shape.bytes_per_pixel;
    return std::size_t{Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT)}
           << (GOB_SIZE_SHIFT + shape.block_height + block_depth);
}

std::size_t InputBytes(const Shape& shape) {
    return std::size_t{Common::DivCeil(shape.height, BandRows(shape))} * BandBytes(shape, 0);
}

std::vector<u8> RandomBytes(std::size_t count, u32 seed) {
    std::mt19937 engine{seed};
    std::vector<u8> bytes(count);
    for (u8& byte : bytes) {
        byte = static_cast<u8>(engine() & 0xFFU);
    }
    return bytes;
}

/// The rows left at 0xCD are rows no run wrote.
std::vector<u8> Unswizzle(const Shape& shape, std::span<const u8> input, bool native_tiling,
                          u32 parts) {
    std::vector<u8> output(std::size_t{shape.width} * shape.bytes_per_pixel * shape.height, 0xCD);
    UnswizzleTextureParallel(output, input, shape.bytes_per_pixel, shape.width, shape.height, 1,
                             shape.block_height, 0, shape.stride_alignment, native_tiling, parts);
    return output;
}

void ExpectBand(const UnswizzleBand& band, std::size_t src_offset, std::size_t dst_offset,
                u32 rows) {
    REQUIRE(band.src_offset == src_offset);
    REQUIRE(band.dst_offset == dst_offset);
    REQUIRE(band.rows == rows);
}

} // Anonymous namespace

TEST_CASE("Unswizzle plan cuts block rows into runs with exact offsets", "[video_core][textures]") {
    SECTION("Four block rows in two runs") {
        // 1024 byte rows, 16 rows a block row, 16 blocks of 1 KiB: 16384 bytes a block row.
        const auto bands = PlanUnswizzleBands(4, 256, 64, 1, 0, 1, 2);
        REQUIRE(bands.size() == 2);
        ExpectBand(bands[0], 0, 0, 32);
        ExpectBand(bands[1], 32768, 32768, 32);
    }
    SECTION("The first run takes the extra block row and the last run ends on the last row") {
        const auto bands = PlanUnswizzleBands(4, 256, 40, 1, 0, 1, 2);
        REQUIRE(bands.size() == 2);
        ExpectBand(bands[0], 0, 0, 32);
        ExpectBand(bands[1], 32768, 32768, 8);
    }
    SECTION("More runs asked for than block rows gives one run a block row") {
        const auto bands = PlanUnswizzleBands(4, 256, 40, 1, 0, 1, 8);
        REQUIRE(bands.size() == 3);
        ExpectBand(bands[0], 0, 0, 16);
        ExpectBand(bands[1], 16384, 16384, 16);
        ExpectBand(bands[2], 32768, 32768, 8);
    }
    SECTION("A padded row stride sets the guest offsets, the pixel width the linear ones") {
        // 100 pixels pad to 128: 512 byte stride, 8 blocks of 512 bytes. Linear rows are 400 bytes.
        const auto bands = PlanUnswizzleBands(4, 100, 24, 0, 0, 6, 3);
        REQUIRE(bands.size() == 3);
        ExpectBand(bands[0], 0, 0, 8);
        ExpectBand(bands[1], 4096, 3200, 8);
        ExpectBand(bands[2], 8192, 6400, 8);
    }
    SECTION("A block depth doubles the bytes of a block row") {
        const auto bands = PlanUnswizzleBands(1, 64, 16, 0, 1, 6, 2);
        REQUIRE(bands.size() == 2);
        ExpectBand(bands[0], 0, 0, 8);
        ExpectBand(bands[1], 1024, 512, 8);
    }
    SECTION("Zero runs is one run, and an image without rows has none") {
        const auto one = PlanUnswizzleBands(4, 64, 16, 0, 0, 4, 0);
        REQUIRE(one.size() == 1);
        ExpectBand(one[0], 0, 0, 16);
        REQUIRE(PlanUnswizzleBands(4, 64, 0, 0, 0, 4, 4).empty());
    }
}

TEST_CASE("Unswizzle plan covers every row once for any shape", "[video_core][textures]") {
    for (const u32 bytes_per_pixel : {1U, 4U, 16U}) {
        for (const u32 width : {64U, 100U}) {
            for (const u32 height : {1U, 8U, 9U, 64U, 100U, 257U, 1000U}) {
                for (u32 block_height = 0; block_height <= 4; ++block_height) {
                    for (u32 parts = 1; parts <= 9; ++parts) {
                        const Shape shape{bytes_per_pixel, width, height, block_height, 4};
                        const auto bands = PlanUnswizzleBands(bytes_per_pixel, width, height,
                                                              block_height, 0, 4, parts);
                        const u32 band_count = Common::DivCeil(height, BandRows(shape));
                        REQUIRE(bands.size() == (std::min)(parts, band_count));
                        const std::size_t row_bytes = std::size_t{width} * bytes_per_pixel;
                        u32 rows_total = 0;
                        for (std::size_t index = 0; index < bands.size(); ++index) {
                            const UnswizzleBand& band = bands[index];
                            REQUIRE(band.dst_offset == std::size_t{rows_total} * row_bytes);
                            const std::size_t first_band =
                                band.dst_offset / row_bytes / BandRows(shape);
                            REQUIRE(band.src_offset == first_band * BandBytes(shape, 0));
                            if (index + 1 < bands.size()) {
                                REQUIRE(band.rows % BandRows(shape) == 0);
                            }
                            rows_total += band.rows;
                        }
                        REQUIRE(rows_total == height);
                    }
                }
            }
        }
    }
}

TEST_CASE("Unswizzle parts need a megabyte of output each", "[video_core][textures]") {
    REQUIRE(UnswizzleParts(4 * MiB, 100, 4) == 4);
    REQUIRE(UnswizzleParts(4 * MiB, 100, 8) == 4);
    REQUIRE(UnswizzleParts(2 * MiB, 100, 4) == 2);
    REQUIRE(UnswizzleParts(2 * MiB - 1, 100, 4) == 1);
    REQUIRE(UnswizzleParts(3 * MiB, 100, 16) == 3);
    REQUIRE(UnswizzleParts(64 * MiB, 3, 8) == 3);
    REQUIRE(UnswizzleParts(64 * MiB, 100, 1) == 1);
    // A 3840x2160 RGBA8 image: 31 MiB of output, many block rows.
    REQUIRE(UnswizzleParts(33'177'600, 17, 4) == 4);
    REQUIRE(UnswizzleParts(33'177'600, 17, 16) == 16);
    REQUIRE(UnswizzleParts(33'177'600, 17, 64) == 17);
    // The two shapes of the largest CPU unswizzles in the 0.5 first-minutes run: a 2048x2048
    // D32_FLOAT depth target (16 MiB, block height 4, 16 block rows) and a 2560x1440
    // R16G16B16A16_FLOAT image (29.5 MiB, block height 0, 180 block rows). Four threads split
    // both in four, so the run's log lines are of images that took the parallel path.
    REQUIRE(UnswizzleParts(16'777'216, Common::DivCeil(2048, GOB_SIZE_Y << 4), 4) == 4);
    REQUIRE(UnswizzleParts(29'491'200, Common::DivCeil(1440, GOB_SIZE_Y << 0), 4) == 4);
    // Never less than one.
    REQUIRE(UnswizzleParts(0, 0, 0) == 1);
    REQUIRE(UnswizzleParts(8 * MiB, 0, 4) == 1);
    REQUIRE(UnswizzleParts(8 * MiB, 4, 0) == 1);
}

TEST_CASE("Unswizzle puts GOB bytes where the layout says", "[video_core][textures]") {
    // One byte a pixel, 64 pixels across: a block row is 512 bytes. Byte (x, y) of a GOB sits at
    // x bits 0-3, 4, 5 -> offset bits 0-3, 5, 8 and y bits 0, 1, 2 -> offset bits 4, 6, 7.
    std::vector<u8> input(1024);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<u8>(index % 251 + 1);
    }
    for (const u32 parts : {1U, 2U, 4U}) {
        std::vector<u8> output(64 * 16, 0);
        UnswizzleTextureParallel(output, input, 1, 64, 16, 1, 0, 0, 6, false, parts);
        REQUIRE(output[0] == 1);              // (0, 0): offset 0
        REQUIRE(output[16] == 33);            // (16, 0): offset 32
        REQUIRE(output[32] == 6);             // (32, 0): offset 256 -> 256 % 251 + 1
        REQUIRE(output[63] == 53);            // (63, 0): offset 303 -> 303 % 251 + 1
        REQUIRE(output[64] == 17);            // (0, 1): offset 16
        REQUIRE(output[128] == 65);           // (0, 2): offset 64
        REQUIRE(output[256] == 129);          // (0, 4): offset 128
        REQUIRE(output[7 * 64 + 63] == 10);   // (63, 7): offset 511 -> 511 % 251 + 1
        // The second block row starts at byte 512 of the input.
        REQUIRE(output[8 * 64] == 11);        // (0, 8): offset 512 -> 512 % 251 + 1
        REQUIRE(output[9 * 64] == 27);        // (0, 9): offset 528 -> 528 % 251 + 1
    }
    std::vector<u8> whole(64 * 16, 0);
    UnswizzleTexture(whole, input, 1, 64, 16, 1, 0, 0, 6, false);
    REQUIRE(whole[9 * 64] == 27);
}

TEST_CASE("Unswizzle bytes do not depend on the number of runs", "[video_core][textures]") {
    const Shape shapes[] = {
        {1, 64, 8, 0, 6},    {1, 100, 33, 1, 6},  {2, 130, 100, 2, 5}, {4, 256, 64, 1, 4},
        {4, 100, 24, 0, 6},  {4, 301, 257, 3, 4}, {8, 77, 200, 4, 3},  {16, 64, 129, 2, 2},
        {3, 21, 70, 1, 1},   {12, 50, 90, 2, 1},  {4, 1024, 1024, 4, 4},
    };
    u32 seed = 1;
    for (const Shape& shape : shapes) {
        const std::vector<u8> input = RandomBytes(InputBytes(shape), seed++);
        for (const bool native_tiling : {false, true}) {
            CAPTURE(shape.bytes_per_pixel, shape.width, shape.height, shape.block_height,
                    shape.stride_alignment, native_tiling);
            const std::vector<u8> serial = Unswizzle(shape, input, native_tiling, 1);
            REQUIRE(std::ranges::count(serial, u8{0xCD}) <
                    static_cast<std::ptrdiff_t>(serial.size()));
            for (const u32 parts : {2U, 3U, 4U, 7U}) {
                CAPTURE(parts);
                REQUIRE(Unswizzle(shape, input, native_tiling, parts) == serial);
            }
        }
    }
}

TEST_CASE("Unswizzle of a large image matches one run", "[video_core][textures]") {
    // 12 MiB of output over 12 block rows: UnswizzleTexture splits it by itself when the host
    // allows more than one thread, and writes the bytes of a single run either way.
    const Shape shape{4, 2048, 1536, 4, 4};
    const std::vector<u8> input = RandomBytes(InputBytes(shape), 7);
    const std::vector<u8> serial = Unswizzle(shape, input, false, 1);
    std::vector<u8> automatic(serial.size(), 0xCD);
    UnswizzleTexture(automatic, input, shape.bytes_per_pixel, shape.width, shape.height, 1,
                     shape.block_height, 0, shape.stride_alignment, false);
    REQUIRE(automatic == serial);
    REQUIRE(Unswizzle(shape, input, false, 4) == serial);
}

TEST_CASE("Unswizzle of a 3D image stays one run", "[video_core][textures]") {
    // 64x32 at 4 bytes, 2 slices, block height 1: a slice is 2 block rows of 4096 bytes.
    const std::vector<u8> input = RandomBytes(2 * 2 * 4096, 11);
    std::vector<u8> expected(64 * 4 * 32 * 2, 0xCD);
    UnswizzleTexture(expected, input, 4, 64, 32, 2, 1, 0, 4, false);
    std::vector<u8> output(expected.size(), 0xCD);
    UnswizzleTextureParallel(output, input, 4, 64, 32, 2, 1, 0, 4, false, 4);
    REQUIRE(output == expected);
    REQUIRE(std::ranges::count(output, u8{0xCD}) < static_cast<std::ptrdiff_t>(output.size()));
}
