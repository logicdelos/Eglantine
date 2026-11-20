// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <nlohmann/json_fwd.hpp>
#include "common/common_types.h"

namespace VideoCore::Replay {

enum class EffectKind { Submit, Consume, Draw, ComputeDispatch, SyncpointIncrement, FenceSignal,
                        ReadbackVisibility, PresentCompose };
struct LiveEffect {
    EffectKind kind;
    u64 id, value;
    bool observable;
};
using EffectObserver = void (*)(const LiveEffect&) noexcept;
namespace detail {
inline std::atomic<EffectObserver> replay_effect_observer{};
enum class ReadbackOrigin { EmulatedWork, ReplayVerification };
inline thread_local ReadbackOrigin readback_origin = ReadbackOrigin::EmulatedWork;
}

// Cache downloads still run for coherent verification reads. Only their origin changes, on
// the calling thread; the GPU worker's emulated reads remain independently observable.
class ScopedReplayVerificationRead {
public:
    ScopedReplayVerificationRead() noexcept : previous{detail::readback_origin} {
        detail::readback_origin = detail::ReadbackOrigin::ReplayVerification;
    }
    ~ScopedReplayVerificationRead() { detail::readback_origin = previous; }
    ScopedReplayVerificationRead(const ScopedReplayVerificationRead&) = delete;
    ScopedReplayVerificationRead& operator=(const ScopedReplayVerificationRead&) = delete;

private:
    detail::ReadbackOrigin previous;
};
inline void SetReplayEffectObserver(EffectObserver observer) noexcept {
    detail::replay_effect_observer.store(observer, std::memory_order_release);
}
inline void ObserveReplayEffect(const LiveEffect& effect) noexcept {
    if (const auto observer = detail::replay_effect_observer.load(std::memory_order_acquire)) {
        observer(effect);
    }
}
inline void ObserveReplayReadback() noexcept {
    if (detail::readback_origin == detail::ReadbackOrigin::EmulatedWork) {
        ObserveReplayEffect({EffectKind::ReadbackVisibility, 0, 0, false});
    }
}
nlohmann::json EffectRow(u64 frame, u32 ordinal, const LiveEffect& effect);
} // namespace VideoCore::Replay
