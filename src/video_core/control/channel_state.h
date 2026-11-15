// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/nv01_timer.h"
#include "video_core/dma_pusher.h"

namespace Core {
class System;
}

namespace VideoCore {
class RasterizerInterface;
}

namespace Tegra {

class GPU;
class MemoryManager;

namespace Control {

struct ChannelState {
    explicit ChannelState(s32 bind_id);

    void Init(Core::System& system, u64 program_id);

    void BindRasterizer(VideoCore::RasterizerInterface* rasterizer);

    /// Switch 2: an engine's semaphore release at address 0, one increment of the channel's
    /// syncpoint (video_core/nx2.h, queue syncpoints). Runs where the rasterizer does.
    void Nx2IncrementSyncpoint();

    /// Switch 2: the end of a submission, whose increments reach `value` (nvhost_gpu).
    void Nx2RaiseSyncpoint(u32 value);

    struct Payload {
        explicit Payload(Core::System& system, MemoryManager& memory_manager, ChannelState& channel_state);

        /// 3D engine
        Engines::Maxwell3D maxwell_3d;
        /// 2D engine
        Engines::Fermi2D fermi_2d;
        /// Compute engine
        Engines::KeplerCompute kepler_compute;
        /// DMA engine
        Engines::MaxwellDMA maxwell_dma;
        /// Inline memory engine
        Engines::KeplerMemory kepler_memory;
        /// NV01 Timer
        Engines::Nv01Timer nv01_timer;
        DmaPusher dma_pusher;
    };
    std::optional<Payload> payload;
    MemoryManager* memory_manager = nullptr;

    s32 bind_id = -1;
    u64 program_id = 0;
    bool initialized = false;

    /// Switch 2: the channel's syncpoint, the highest value its submissions reach, and the GPU
    /// address of the word its queue's waits read (0 until the queue released its slot).
    u32 nx2_syncpoint = 0;
    std::atomic<u32> nx2_syncpoint_max{};
    std::atomic<u64> nx2_syncpoint_word{};

private:
    void Nx2WriteSyncpointWord();
};

} // namespace Control

} // namespace Tegra
