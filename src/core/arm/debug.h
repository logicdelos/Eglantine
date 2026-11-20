// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <chrono>

#include "core/hle/kernel/k_thread.h"
#include "core/loader/loader.h"

namespace Core {

std::optional<std::string> GetThreadName(const Kernel::KThread* thread);
std::string_view GetThreadWaitReason(const Kernel::KThread* thread);
std::string GetThreadState(const Kernel::KThread* thread);

Loader::AppLoader::Modules FindModules(Kernel::KProcess* process);
Kernel::KProcessAddress GetModuleEnd(const Kernel::KProcess* process, Kernel::KProcessAddress base);
Kernel::KProcessAddress FindMainModuleEntrypoint(Kernel::KProcess* process);

void InvalidateInstructionCacheRange(const Kernel::KProcess* process, u64 address, u64 size);

/// Invalidates on every core, then waits until no other core can still be executing code it
/// translated before: each is idle (its next slice services the invalidation first), has ended
/// the slice it was in, or is itself waiting in a host call. False if that did not happen within
/// `timeout`. The calling core must itself leave its block through the dispatcher afterwards (a
/// probe with end_block_after does).
bool InvalidateInstructionCacheRangeAndWait(const Kernel::KProcess* process, u64 address, u64 size,
                                            std::chrono::milliseconds timeout);

struct BacktraceEntry {
    std::string module;
    u64 address;
    u64 original_address;
    u64 offset;
    std::string name;
};

std::vector<BacktraceEntry> GetBacktraceFromContext(Kernel::KProcess* process,
                                                    const Kernel::Svc::ThreadContext& ctx);
std::vector<BacktraceEntry> GetBacktrace(const Kernel::KThread* thread);

} // namespace Core
