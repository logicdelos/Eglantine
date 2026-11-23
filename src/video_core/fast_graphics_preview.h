// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

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
    u64 backend_engine{};   ///< not Vulkan, not Maxwell3D, or not an NX2 (Ampere) channel
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

/// What the process decided at GPU startup: setting and environment, and the renderer. Taken
/// once; the saved setting is never changed by the environment.
struct FastGraphicsPreviewSnapshot {
    bool enabled{};                          ///< the effective value
    bool vulkan{};                           ///< the renderer is Vulkan
    std::string_view source{"default"};      ///< "default", "setting", "environment" or "invalid"
};

/// Takes the snapshot (once per process): Settings nx2_fast_graphics_preview, overridden by
/// EDEN_NX2_FAST_GRAPHICS_PREVIEW=0|1 (any other value is "invalid" and means off, with a
/// warning). Logs the effective value and its source. Called from the GPU constructor.
void ResolveFastGraphicsPreview();

/// The snapshot; off, source "default", until ResolveFastGraphicsPreview ran. DmaPusher copies it
/// when it is constructed.
[[nodiscard]] FastGraphicsPreviewSnapshot FastGraphicsPreviewMode();

/// Logs the totals once, when the snapshot is on. Called at shutdown.
void LogFastGraphicsPreviewTotals();

} // namespace Tegra
