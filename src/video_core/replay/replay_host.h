// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Seams the replay session calls and other slices fill in. The pointers live in
// replay_session.cpp. Until a slice registers the one a call needs, Run fails with a clear
// reason instead of touching a guest process. Nothing here runs unless a replay is in progress.
//
// Host syncpoints need no seam: Run reaches them through Host1x's SyncpointManager.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
namespace Frontend {
class EmuWindow;
}
} // namespace Core

namespace Tegra {
class MemoryManager;
namespace Control {
struct ChannelState;
}
} // namespace Tegra

namespace VideoCore::Replay {

struct ReplayHostFns {
    // Creates device memory, the host1x core, the GPU and the renderer without a guest process.
    // Leaves the system unpowered and the GPU not started (Run calls GPU::Start). Defined by the
    // integrator (core.cpp owns the System internals); called once from Run, after the pins were
    // applied and before the checkpoint is restored.
    bool (*setup)(Core::System& system, Core::Frontend::EmuWindow& window,
                  std::string& error) = nullptr;
    // Stops the GPU thread (GPU::NotifyShutdown) and releases what setup created. Called once
    // when setup succeeded, whatever the outcome of the run.
    void (*teardown)(Core::System& system) = nullptr;

    // Captured channel id and address-space id to the objects RestoreGpuCheckpoint created.
    // Defined by the capture slice, which owns the restore.
    Tegra::Control::ChannelState* (*channel)(Core::System& system, u32 channel_id) = nullptr;
    std::shared_ptr<Tegra::MemoryManager> (*address_space)(Core::System& system, u32 address_space_id) = nullptr;

    // A fresh, zero-filled device allocation of `size` bytes for one capture allocation id and
    // generation. `host_bytes` is writable for the whole size and stays valid until teardown.
    bool (*map_backing)(Core::System& system, u64 alloc_id, u64 size, DAddr& device_addr,
                        u8*& host_bytes, std::string& error) = nullptr;

    // The renderer patch registers these (Vulkan::RendererVulkan::SetReplayReadback and
    // ReadReplayPicture). set_readback is always called before the first frame: from then on the
    // renderer composes off-screen at each present's own extent (Tegra::PictureExtent, the one
    // the capture used), and `enabled` selects whether the picture
    // is kept for read_picture (false is the benchmark: no picture, no download). read_picture
    // takes the last composed B8G8R8A8 buffer, tightly packed, unscaled. A null set_readback
    // fails every run; a null read_picture fails a non-benchmark run.
    void (*set_readback)(bool enabled) = nullptr;
    bool (*read_picture)(u32& width, u32& height, std::vector<u8>& bgra, std::string& error) = nullptr;
};

void SetReplayHost(const ReplayHostFns& fns);
[[nodiscard]] const ReplayHostFns& GetReplayHost();

} // namespace VideoCore::Replay
