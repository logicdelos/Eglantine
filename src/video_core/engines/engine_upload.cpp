// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "common/algorithm.h"
#include "common/assert.h"
#include "video_core/engines/engine_upload.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/textures/decoders.h"

namespace Tegra::Engines::Upload {

State::State(MemoryManager& memory_manager_, Registers& regs_)
    : regs{regs_}, memory_manager{memory_manager_} {}

State::~State() = default;

void State::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

void State::ProcessExec(const bool is_linear_) {
    write_offset = 0;
    flushed_offset = 0;
    copy_size = regs.line_length_in * regs.line_count;
    inner_buffer.resize_destructive(copy_size);
    is_linear = is_linear_;
}

void State::ProcessData(const u32 data, const bool is_last_call) {
    Receive({reinterpret_cast<const u8*>(&data), sizeof(data)}, is_last_call);
}

void State::ProcessData(const u32* data, size_t num_data) {
    Receive({reinterpret_cast<const u8*>(data), num_data * sizeof(u32)}, true);
}

void State::Receive(std::span<const u8> data, bool is_last_call) {
    if (!HasPendingData()) {
        return;
    }
    data = data.first(std::min<size_t>(data.size(), copy_size - write_offset));
    const bool partial = native_partial && is_linear && regs.line_count == 1;
    const u32 end = write_offset + static_cast<u32>(data.size());
    const bool flush = is_last_call && (partial || end == copy_size);
    // A complete bulk packet (or a native linear prefix) needs no staging copy.
    if (!flush || write_offset != flushed_offset) {
        if (!data.empty()) {
            std::memcpy(inner_buffer.data() + write_offset, data.data(), data.size());
        }
        data = std::span<const u8>{inner_buffer}.subspan(flushed_offset, end - flushed_offset);
    }
    write_offset = end;
    if (!flush || data.empty()) {
        return;
    }
    if (partial) {
        rasterizer->AccelerateInlineToMemory(regs.dest.Address() + flushed_offset,
                                             static_cast<u32>(data.size()), data);
    } else {
        ProcessData(data);
    }
    flushed_offset = end;
}

void State::ProcessData(std::span<const u8> read_buffer) {
    const GPUVAddr address{regs.dest.Address()};
    if (is_linear) {
        for (size_t line = 0; line < regs.line_count; ++line) {
            const GPUVAddr dest_line = address + line * regs.dest.pitch;
            std::span<const u8> buffer(read_buffer.data() + line * regs.line_length_in,
                                       regs.line_length_in);
            rasterizer->AccelerateInlineToMemory(dest_line, regs.line_length_in, buffer);
        }
    } else {
        u32 width = regs.dest.width;
        u32 x_elements = regs.line_length_in;
        u32 x_offset = regs.dest.x;
        const u32 bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return (std::min)(x, static_cast<u32>(std::countr_zero(y))); },
            width, x_elements, x_offset, static_cast<u32>(address));
        width >>= bpp_shift;
        x_elements >>= bpp_shift;
        x_offset >>= bpp_shift;
        const u32 bytes_per_pixel = 1U << bpp_shift;
        const std::size_t dst_size = Tegra::Texture::CalculateSize(
            true, bytes_per_pixel, width, regs.dest.height, regs.dest.depth,
            regs.dest.BlockHeight(), regs.dest.BlockDepth());

        Tegra::Memory::GpuGuestMemoryScoped<u8,
                                            Tegra::Memory::GuestMemoryFlags::SafeReadCachedWrite>
            tmp(memory_manager, address, dst_size, &tmp_buffer);

        Tegra::Texture::SwizzleSubrect(tmp, read_buffer, bytes_per_pixel, width, regs.dest.height,
                                       regs.dest.depth, x_offset, regs.dest.y, x_elements,
                                       regs.line_count, regs.dest.BlockHeight(),
                                       regs.dest.BlockDepth(), regs.line_length_in);
    }
}

} // namespace Tegra::Engines::Upload
