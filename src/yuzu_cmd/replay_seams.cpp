// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/host_memory.h"
#include "common/page_table.h"
#include "common/typed_address.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/device_memory_manager.h"
#include "core/hle/kernel/board/nintendo/nx/k_system_control.h"
#include "core/hle/kernel/k_memory_layout.h"
#include "core/hle/kernel/k_memory_region_type.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/replay/checkpoint.h"
#include "video_core/replay/replay_host.h"
#include "yuzu_cmd/replay_seams.h"

namespace ReplayRunner {
namespace {

constexpr u64 PageSize = Core::Memory::YUZU_PAGESIZE;
constexpr u64 AddressSpaceBits = 39;
// Virtual addresses of the replay's own address space start above the first page.
constexpr u64 FirstVirtual = 0x10000000ULL;

// What a replay maps device allocations into: an address space with no guest process. Each
// allocation is a fresh run of the device memory buffer (zero until written), mapped at its own
// virtual range and registered with the host1x device memory manager under one ASID. Single
// replay per process, driven from one thread.
struct BackingSpace {
    std::unique_ptr<Core::Memory::Memory> memory;
    Common::PageTable page_table;
    Core::Asid asid{};
    u64 next_physical = Core::DramMemoryMap::Base;
    u64 next_virtual = FirstVirtual;
};

BackingSpace* g_space = nullptr;

// The kernel is initialised in a replay (there is just no process), so its code, slab heaps and
// page table heap already sit in the bottom of the device memory buffer. The first byte above
// every non-user-pool DRAM region is free; the user pools hold nothing without a process.
u64 FirstFreePhysical(Core::System& system) {
    u64 first = Core::DramMemoryMap::Base;
    for (const auto& region : system.Kernel().MemoryLayout().GetPhysicalMemoryRegionTree()) {
        if (region.IsDerivedFrom(Kernel::KMemoryRegionType_Dram) &&
            !region.IsDerivedFrom(Kernel::KMemoryRegionType_DramUserPool)) {
            first = std::max<u64>(first, region.GetEndAddress());
        }
    }
    return Common::AlignUp(first, PageSize);
}

bool Setup(Core::System& system, Core::Frontend::EmuWindow& window, std::string& error) {
    if (!system.SetupForReplay(window, error)) {
        return false;
    }
    auto space = std::make_unique<BackingSpace>();
    space->page_table.Resize(AddressSpaceBits, Core::Memory::YUZU_PAGEBITS);
    space->next_physical = FirstFreePhysical(system);
    space->memory = std::make_unique<Core::Memory::Memory>(system);
    space->memory->SetCurrentPageTable(space->page_table);
    space->asid = system.Host1x().MemoryManager().RegisterProcess(space->memory.get());
    g_space = space.release();
    return true;
}

void Teardown(Core::System& system) {
    system.TeardownReplay();
    // The GPU is gone, so nothing calls into the objects the checkpoint restore made any more.
    // A restore the GPU refused leaves channels behind; TeardownReplay discarded that GPU.
    VideoCore::Replay::ReleaseRestoredGpuState();
    delete g_space;
    g_space = nullptr;
}

bool MapBacking(Core::System& system, [[maybe_unused]] u64 alloc_id, u64 size, DAddr& device_addr,
                u8*& host_bytes, std::string& error) {
    if (g_space == nullptr) {
        error = "the replay backing space does not exist";
        return false;
    }
    if (size == 0) {
        error = "an allocation of zero bytes";
        return false;
    }
    BackingSpace& space = *g_space;
    const u64 bytes = (size + PageSize - 1) / PageSize * PageSize;
    const u64 limit = Core::DramMemoryMap::Base +
                      Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize();
    if (bytes > limit - space.next_physical) {
        error = "the replay's allocations exceed the device memory buffer";
        return false;
    }
    const u64 physical = space.next_physical;
    const u64 virtual_base = space.next_virtual;
    if (bytes > (u64{1} << AddressSpaceBits) - virtual_base) {
        error = "the replay's allocations exceed its address space";
        return false;
    }
    space.next_physical += bytes;
    space.next_virtual += bytes;

    space.memory->MapMemoryRegion(space.page_table, Common::ProcessAddress{virtual_base}, bytes,
                                  Common::PhysicalAddress{physical},
                                  Common::MemoryPermission::ReadWrite, false);
    auto& device_memory = system.Host1x().MemoryManager();
    device_addr = device_memory.Allocate(bytes);
    device_memory.Map(device_addr, virtual_base, bytes, space.asid, true);
    host_bytes = system.DeviceMemory().GetPointerFromRaw<u8>(physical - Core::DramMemoryMap::Base);
    return true;
}

} // namespace

void RegisterHost() {
    static std::once_flag once;
    std::call_once(once, [] {
        VideoCore::Replay::ReplayHostFns fns = VideoCore::Replay::GetReplayHost();
        fns.setup = Setup;
        fns.teardown = Teardown;
        fns.channel = VideoCore::Replay::RestoredChannel;
        fns.address_space = VideoCore::Replay::RestoredAddressSpace;
        fns.map_backing = MapBacking;
        fns.set_readback = Vulkan::RendererVulkan::SetReplayReadback;
        fns.read_picture = Vulkan::RendererVulkan::ReadReplayPicture;
        VideoCore::Replay::SetReplayHost(fns);
    });
}

} // namespace ReplayRunner
