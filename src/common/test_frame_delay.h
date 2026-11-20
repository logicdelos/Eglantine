// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <string_view>
#include "common/common_types.h"

namespace Common::TestHooks {

struct FrameDelaySnapshot {
    bool configured{}; // At least one environment variable was set.
    u64 requested_ns{}; // Zero when disabled.
    std::string_view error; // Static text; empty for a valid configuration.
    u64 frames_delayed{};
    u64 added_ns_total{};
    u64 max_added_ns{};
};

// Validation only. One accepted present is one RequestComposite actually submitted by nvdisp or
// GSV. An unchanged-buffer refresh or a failed GSV lookup submits nothing; two submissions using
// the same address are distinct. Apply once on the producing thread immediately after submission.
// This waits on the host steady clock, never on a guest timer. No GPU/replay or title-specific mode.
class FrameDelay {
public:
    FrameDelay(const char* delay_ns, const char* target) noexcept;
    // Reads EGLANTINE_TEST_FRAME_DELAY_NS and EGLANTINE_TEST_FRAME_DELAY_TARGET once. Both a
    // positive decimal dose (at most 1 s) and "cpu-present" are required. Zero is a valid off arm.
    static FrameDelay& ForProcess();
    u64 Apply(); // Actual host nanoseconds waited, or zero when disabled.
    [[nodiscard]] FrameDelaySnapshot Snapshot() const;

private:
    mutable std::mutex counters_mutex;
    FrameDelaySnapshot state;
};

} // namespace Common::TestHooks
