// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "core/hle/service/nvnflinger/pixel_format.h"
#include "video_core/replay/format.h"
#include "video_core/textures/decoders.h"

namespace VideoCore::Replay {

// Bytes read by the Vulkan raw presentation fallback, starting at address + framebuffer_offset.
// The accelerated path finding a cached image is not proof these padded rows exist on replay.
[[nodiscard]] inline std::optional<u64> PresentBackingBytes(u32 width, u32 height, u32 stride,
                                                            u32 pixel_format) {
    if (width == 0 || height == 0 || stride < width || stride > MaxPictureDimension ||
        height > MaxPictureDimension) {
        return std::nullopt;
    }
    using Format = Service::android::PixelFormat;
    u32 bytes_per_pixel;
    switch (static_cast<Format>(pixel_format)) {
    case Format::Rgba8888:
    case Format::Rgbx8888:
    case Format::Bgra8888:
    case Format::Rgba1010102:
        bytes_per_pixel = 4;
        break;
    case Format::Rgb565:
        bytes_per_pixel = 2;
        break;
    default:
        return std::nullopt;
    }
    return Tegra::Texture::CalculateSize(true, bytes_per_pixel, stride, height, 1, 4, 0);
}

} // namespace VideoCore::Replay
