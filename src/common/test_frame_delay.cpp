// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/test_frame_delay.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace Common::TestHooks {

FrameDelay::FrameDelay(const char* delay_ns, const char* target) noexcept {
    state.configured = delay_ns || target;
    if (!state.configured) {
        return;
    }
    if (!delay_ns) {
        state.error = "EGLANTINE_TEST_FRAME_DELAY_TARGET is set without a delay";
        return;
    }
    const std::string_view text{delay_ns};
    u64 ns{};
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, ns);
    if (parsed.ec != std::errc{} || parsed.ptr != end || ns > 1'000'000'000) {
        state.error = "EGLANTINE_TEST_FRAME_DELAY_NS must be a decimal from 0 to 1000000000";
        return;
    }
    if ((target && std::string_view{target} != "cpu-present") || (ns && !target)) {
        state.error = "EGLANTINE_TEST_FRAME_DELAY_TARGET must be cpu-present";
        return;
    }
    state.requested_ns = ns;
}

FrameDelay& FrameDelay::ForProcess() {
    static FrameDelay delay{std::getenv("EGLANTINE_TEST_FRAME_DELAY_NS"),
                            std::getenv("EGLANTINE_TEST_FRAME_DELAY_TARGET")};
    return delay;
}

u64 FrameDelay::Apply() {
    if (!state.requested_ns) {
        return 0;
    }
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    constexpr auto spin_margin = 200us;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::nanoseconds{static_cast<s64>(state.requested_ns)};
    auto now = start;
    while (now < deadline) {
        const auto remaining = deadline - now;
        if (remaining > spin_margin) {
            std::this_thread::sleep_for(remaining - spin_margin);
        }
        now = Clock::now();
    }
    const auto added = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
    std::scoped_lock lock{counters_mutex};
    ++state.frames_delayed;
    state.added_ns_total += added;
    state.max_added_ns = std::max(state.max_added_ns, added);
    return added;
}

FrameDelaySnapshot FrameDelay::Snapshot() const {
    std::scoped_lock lock{counters_mutex};
    return state;
}

} // namespace Common::TestHooks
