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
}
inline void SetReplayEffectObserver(EffectObserver observer) noexcept {
    detail::replay_effect_observer.store(observer, std::memory_order_release);
}
inline void ObserveReplayEffect(const LiveEffect& effect) noexcept {
    if (const auto observer = detail::replay_effect_observer.load(std::memory_order_acquire)) {
        observer(effect);
    }
}
nlohmann::json EffectRow(u64 frame, u32 ordinal, const LiveEffect& effect);
} // namespace VideoCore::Replay
