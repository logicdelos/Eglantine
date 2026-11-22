// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/nx2_bottleneck.h"
#include "common/nx2_sampler.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/kernel.h"
#include "tests/core/system_test_context.h"

// Runs all guest cores and uses the normal suspension and termination path at shutdown.
struct MulticoreKernelRuntime {
    MulticoreKernelRuntime() {
        Settings::values.use_multi_core.SetValue(true);
        system.Initialize();
        auto& kernel = system.Kernel();
        kernel.Initialize();
        system.GetCpuManager().Initialize();
        system.GetCpuManager().OnGpuReady();
        system.CoreTiming().SyncPause(false);
    }

    ~MulticoreKernelRuntime() {
        auto& kernel = system.Kernel();
        Nx2Sampler::SetGuestMemoryPointer({});
        system.SetShuttingDown(true);
        kernel.RequestSuspendEmulation();
        system.CoreTiming().SyncPause(false);
        kernel.SuspendEmulation(true);
        // Terminate all children together, as in System::ShutdownMainProcess.
        kernel.ShutdownCores();
        Nx2Bottleneck::ClearSleepThreadCounters();
        system.CoreTiming().ClearPendingEvents();
        system.GetCpuManager().Shutdown();
        kernel.Shutdown();
    }

    MulticoreKernelRuntime(const MulticoreKernelRuntime&) = delete;
    MulticoreKernelRuntime& operator=(const MulticoreKernelRuntime&) = delete;

    ScopedSystemTestContext ctx;
    Core::System system;
};
