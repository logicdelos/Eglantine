// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

// The renderer's screenshot request, shared by the MCP host and the control channel of eden-cli.
namespace FrameCapture {

/// One presented frame, B G R A bytes per pixel, rows packed, top row first.
struct Frame {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> bgra;
};

/// A request the renderer has not answered yet. The renderer's callback may run after Wait gave
/// up; it keeps the buffer alive through `capture`.
class Pending {
public:
    struct Capture {
        Frame frame;
        std::promise<bool> done;
    };

    Pending(std::shared_ptr<Capture> capture_, std::future<bool> future_)
        : capture{std::move(capture_)}, future{std::move(future_)} {}

    /// Waits up to `timeout` for the frame; `error` says why there is none.
    [[nodiscard]] std::optional<Frame> Wait(std::chrono::milliseconds timeout, std::string& error);

private:
    std::shared_ptr<Capture> capture;
    std::future<bool> future;
};

/// Asks the renderer for a 16:9 frame `width` wide (clamped to 160..3840) and returns at once.
/// Fails with `error` set when no game runs or another screenshot is in progress. Does not touch
/// the emulation state: any thread may call it.
[[nodiscard]] std::optional<Pending> Begin(Core::System& system, u32 width, std::string& error);

/// Encodes a frame as an opaque RGBA PNG.
[[nodiscard]] std::vector<u8> EncodePng(u32 width, u32 height, std::vector<u8> bgra);

} // namespace FrameCapture
