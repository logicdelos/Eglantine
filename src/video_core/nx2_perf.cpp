// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "common/settings.h"
#include "video_core/nx2_perf.h"

namespace VideoCommon::Nx2Perf {
namespace {

constexpr u64 all_opts = ~u64{0} >> (64 - static_cast<unsigned>(Opt::Count));

/// The off-mask the environment alone decides (every switch off when EDEN_NX2_PERF_OPT=0), read
/// once per process.
u64 EnvironmentOffMask() {
    static const u64 mask = [] {
        const char* on_list = std::getenv("EDEN_NX2_PERF_ON");
        const char* off_list = std::getenv("EDEN_NX2_PERF_OFF");
        const u64 env_mask = ParseOffMask(std::getenv("EDEN_NX2_PERF_SAFE") != nullptr,
                                          on_list ? on_list : "", off_list ? off_list : "");
        return Enabled() ? env_mask : all_opts;
    }();
    return mask;
}

/// The environment's mask with the setting skip_unneeded_target_copies as it is now.
u64 EffectiveOffMask() {
    return WithTargetCopySetting(EnvironmentOffMask(),
                                 Settings::values.skip_unneeded_target_copies.GetValue());
}

} // Anonymous namespace

namespace detail {

u64 InitOffMask() {
    [[maybe_unused]] static const bool initialized = [] {
        const u64 mask = EffectiveOffMask();
        std::string state;
        for (size_t i = 0; i < opt_names.size(); ++i) {
            state += fmt::format(" {}={}", opt_names[i], IsOff(mask, i) ? "off" : "on");
        }
        LOG_INFO(HW_GPU, "Nx2Perf:{}", state);
        live_off_mask.store(mask, std::memory_order_relaxed);
        return true;
    }();
    // With every switch off (all 64 bits) the mask equals the sentinel and On() comes back here
    // each time, which still answers right.
    return live_off_mask.load(std::memory_order_relaxed);
}

} // namespace detail

void ApplySettings() {
    // The first call (or the first On() before it) logs the full switch list.
    static_cast<void>(detail::InitOffMask());
    const u64 mask = EffectiveOffMask();
    detail::live_off_mask.store(mask, std::memory_order_relaxed);
    LOG_INFO(HW_GPU, "Nx2Perf: skip_unneeded_target_copies={} feedback_shadow_share={} "
                     "feedback_skip_unwritten={} masked_target_unmodified={}",
             Settings::values.skip_unneeded_target_copies.GetValue() ? "on" : "off",
             IsOff(mask, static_cast<size_t>(Opt::FeedbackShadowShare)) ? "off" : "on",
             IsOff(mask, static_cast<size_t>(Opt::FeedbackSkipUnwritten)) ? "off" : "on",
             IsOff(mask, static_cast<size_t>(Opt::MaskedTargetUnmodified)) ? "off" : "on");
}

} // namespace VideoCommon::Nx2Perf
