// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Tegra {

/// Counts of the fast graphics preview (Settings nx2_fast_graphics_preview): headers the decoder
/// took (accepted) and header attempts it handed to the legacy path, by the first reason found.
/// Packets, not NVN calls. Counted only while the setting is on.
struct FastGraphicsPreviewStats {
    u64 accepted{};
    u64 other_opcode{};     ///< an idle header that does not start with const_buffer.size
    u64 incomplete_span{};  ///< a word of the bind is not in the current command span
    u64 shape_subchannel{}; ///< not three words, no bind write straight after, another subchannel
    u64 invalid_binding{};  ///< stage, slot, valid bit, reserved bits, size or address
    u64 backend_engine{};   ///< not a Maxwell3D subchannel, or not the Vulkan renderer
    u64 ordering{};         ///< split GPU thread, pending acquire, macro, deferred draw, replay
    u64 observer{};         ///< something watches the stream: capture, replay, profiling, watch

    [[nodiscard]] u64 Rejected() const {
        return other_opcode + incomplete_span + shape_subchannel + invalid_binding +
               backend_engine + ordering + observer;
    }

    bool operator==(const FastGraphicsPreviewStats&) const = default;
};

/// Cumulative counts of every channel, published when a command list ends (DmaPusher::
/// DispatchCalls) and when a pusher is destroyed. For status sampling from any thread.
[[nodiscard]] FastGraphicsPreviewStats FastGraphicsPreviewTotals();

/// Whether the setting was on the last time a pusher published.
[[nodiscard]] bool FastGraphicsPreviewActive();

/// Applies EDEN_NX2_FAST_GRAPHICS_PREVIEW=0|1 to the setting (once per process; any other value
/// means off, with a warning) and logs the effective value and where it came from.
void ApplyFastGraphicsPreviewEnvironment();

/// Logs the totals once, when the setting was on at any time. Called at shutdown.
void LogFastGraphicsPreviewTotals();

} // namespace Tegra
