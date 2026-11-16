// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "common/stb.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/renderer_base.h"
#include "yuzu_cmd/frame_capture.h"

namespace FrameCapture {

std::optional<Frame> Pending::Wait(std::chrono::milliseconds timeout, std::string& error) {
    if (future.wait_for(timeout) != std::future_status::ready) {
        error = "timed out waiting for a frame (paused, loading, or a hidden or null window)";
        return std::nullopt;
    }
    return std::move(capture->frame);
}

std::optional<Pending> Begin(Core::System& system, u32 width, std::string& error) {
    if (!system.IsPoweredOn()) {
        error = "no game is running";
        return std::nullopt;
    }
    auto& renderer = system.Renderer();
    if (renderer.IsScreenshotPending()) {
        error = "another screenshot is in progress; retry";
        return std::nullopt;
    }
    width = std::clamp<u32>(width, 160, 3840);
    const u32 height = width * 9 / 16;
    auto capture = std::make_shared<Pending::Capture>();
    capture->frame = {.width = width, .height = height};
    capture->frame.bgra.resize(size_t{width} * height * 4);
    auto future = capture->done.get_future();
    renderer.RequestScreenshot(
        capture->frame.bgra.data(),
        [capture](bool invert_y) {
            if (invert_y) {
                auto& f = capture->frame;
                std::vector<u8> row(size_t{f.width} * 4);
                for (u32 y = 0; y < f.height / 2; ++y) {
                    u8* a = f.bgra.data() + size_t{y} * f.width * 4;
                    u8* b = f.bgra.data() + size_t{f.height - 1 - y} * f.width * 4;
                    std::memcpy(row.data(), a, row.size());
                    std::memcpy(a, b, row.size());
                    std::memcpy(b, row.data(), row.size());
                }
            }
            capture->done.set_value(true);
        },
        Layout::DefaultFrameLayout(width, height));
    return Pending{std::move(capture), std::move(future)};
}

std::vector<u8> EncodePng(u32 width, u32 height, std::vector<u8> bgra) {
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        std::swap(bgra[i], bgra[i + 2]); // BGRA -> RGBA
        bgra[i + 3] = 255;
    }
    std::vector<u8> png;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<u8>*>(context);
            out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
        },
        &png, static_cast<int>(width), static_cast<int>(height), 4, bgra.data(),
        static_cast<int>(width * 4));
    return png;
}

} // namespace FrameCapture
