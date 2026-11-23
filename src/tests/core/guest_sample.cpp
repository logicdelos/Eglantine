// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/arm/guest_sample.h"

TEST_CASE("GuestSample: nothing is read before the core publishes", "[core]") {
    Core::GuestSample sample;
    u64 pc = 1;
    u64 thread_id = 2;
    REQUIRE_FALSE(sample.Read(pc, thread_id));
    REQUIRE(pc == 1);
    REQUIRE(thread_id == 2);
}

TEST_CASE("GuestSample: a read returns the last published pair", "[core]") {
    Core::GuestSample sample;
    sample.Publish(0x7100001000, 5);
    sample.Publish(0x7100002340, 9);
    u64 pc = 0;
    u64 thread_id = 0;
    REQUIRE(sample.Read(pc, thread_id));
    REQUIRE(pc == 0x7100002340);
    REQUIRE(thread_id == 9);
}

TEST_CASE("GuestSample: a reader running with the writer never sees a torn pair", "[core]") {
    // Every published pair satisfies pc == thread_id * 4096 + 7: a read that mixed two stops
    // breaks it, and ids only grow, so a read cannot go back to an older stop either.
    constexpr u64 Stops = 300000;
    Core::GuestSample sample;
    std::atomic<bool> done{false};
    std::atomic<u64> torn{0};
    std::thread reader{[&] {
        u64 last = 0;
        while (!done.load(std::memory_order_relaxed)) {
            u64 pc = 0;
            u64 thread_id = 0;
            if (!sample.Read(pc, thread_id)) {
                continue;
            }
            if (pc != thread_id * 4096 + 7 || thread_id < last) {
                torn.fetch_add(1, std::memory_order_relaxed);
            }
            last = thread_id;
        }
    }};
    for (u64 stop = 1; stop <= Stops; ++stop) {
        sample.Publish(stop * 4096 + 7, stop);
    }
    done.store(true, std::memory_order_relaxed);
    reader.join();
    REQUIRE(torn.load() == 0);

    u64 pc = 0;
    u64 thread_id = 0;
    REQUIRE(sample.Read(pc, thread_id));
    REQUIRE(thread_id == Stops);
    REQUIRE(pc == Stops * 4096 + 7);
}
