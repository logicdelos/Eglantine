// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The kernel of a replay: there is no guest process, but the replay's physical allocator reads the
// kernel memory layout, so that layout has to exist once the replay's system is set up.

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/nx2_bottleneck.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_memory_layout.h"
#include "core/hle/kernel/k_memory_region_type.h"
#include "core/hle/kernel/kernel.h"
#include "tests/core/system_test_context.h"

namespace {

// The first byte above every DRAM region that is not the user pool, as the replay computes it.
u64 EndOfKernelDram(Core::System& system) {
    u64 end = 0;
    for (const auto& region : system.Kernel().MemoryLayout().GetPhysicalMemoryRegionTree()) {
        if (region.IsDerivedFrom(Kernel::KMemoryRegionType_Dram) &&
            !region.IsDerivedFrom(Kernel::KMemoryRegionType_DramUserPool)) {
            end = std::max<u64>(end, region.GetEndAddress());
        }
    }
    return end;
}

} // namespace

TEST_CASE("the replay kernel has its memory layout and no process", "[core][replay]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    system.InitializeReplayKernel();

    // The slab heaps sit at SlabHeapBase, so the allocator's first free byte is above it.
    REQUIRE(EndOfKernelDram(system) > Core::DramMemoryMap::SlabHeapBase);
    REQUIRE(system.Kernel().ApplicationProcess() == nullptr);
    REQUIRE_FALSE(system.Kernel().HasCreatedUserProcess());
    system.ShutdownReplayKernel();
}

TEST_CASE("the replay kernel can be brought up again after a shutdown", "[core][replay]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    system.InitializeReplayKernel();
    REQUIRE(EndOfKernelDram(system) > Core::DramMemoryMap::SlabHeapBase);
    system.ShutdownReplayKernel();
    system.InitializeReplayKernel();
    REQUIRE(EndOfKernelDram(system) > Core::DramMemoryMap::SlabHeapBase);
    system.ShutdownReplayKernel();
}

// The sleep counters of the kernel's threads are linked into one registry that points into the
// slab storage inside the device memory. A shutdown that leaves them linked sends the host
// thread's own counter, destroyed with the process after the device memory is gone, down a list
// of dangling nodes.
TEST_CASE("the replay kernel shutdown unlinks every sleep counter", "[core][replay]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    system.InitializeReplayKernel();
    Nx2Bottleneck::SleepThreadCounter counter;
    counter.Register(0x7001);
    REQUIRE(counter.id == 0x7001);
    system.ShutdownReplayKernel();
    REQUIRE(counter.id == 0);
    REQUIRE(counter.next == nullptr);
}
