// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "common/common_types.h"

namespace Common::X64 {

#ifdef _MSC_VER
__forceinline static u64 FencedRDTSC() {
    _mm_lfence();
    _ReadWriteBarrier();
    const u64 result = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return result;
}
#else
static inline u64 FencedRDTSC() {
    u64 eax;
    u64 edx;
    asm volatile("lfence\n\t"
                 "rdtsc\n\t"
                 "lfence\n\t"
                 : "=a"(eax), "=d"(edx));
    return (edx << 32) | eax;
}
#endif

/// The TSC without the serialising fences: a few cycles instead of tens, but not ordered against
/// neighbouring instructions (readings may be off by about the pipeline's depth). For timing many
/// short scopes, not for ordering.
#ifdef _MSC_VER
__forceinline static u64 UnfencedRDTSC() {
    return __rdtsc();
}
#else
static inline u64 UnfencedRDTSC() {
    u64 eax;
    u64 edx;
    asm volatile("rdtsc" : "=a"(eax), "=d"(edx));
    return (edx << 32) | eax;
}
#endif

u64 EstimateRDTSCFrequency();

} // namespace Common::X64
