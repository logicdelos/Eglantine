// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/guest_call/nvn_epoch_slot.h"

namespace {
using namespace Core::NvnCensus;

constexpr u64 Epochs = 4;
} // namespace

TEST_CASE("NVN epoch slot: a writer that read an old epoch cannot write after the slot is recycled", "[core][nvn_census]") {
    // The schedule that a second read of the epoch counter does not catch: writer W has read epoch 0
    // and is paused before it reserves. The recycler closes slot 0, sees no reservation, empties
    // it and opens it for epoch 4, but has not yet published the epoch counter. W resumes now.
    std::atomic<u64> open{0};
    std::atomic<u64> inflight{0};

    CloseSlot(open);
    REQUIRE(SlotReservations(inflight) == 0);
    OpenSlot(open, 4);

    REQUIRE_FALSE(ReserveSlotWrite(inflight, open, 0));
    CommitSlotWrite(inflight);
    REQUIRE(SlotReservations(inflight) == 0);
}

TEST_CASE("NVN epoch slot: a writer that reserves while the slot is closed is late", "[core][nvn_census]") {
    std::atomic<u64> open{0};
    std::atomic<u64> inflight{0};

    CloseSlot(open);
    REQUIRE_FALSE(ReserveSlotWrite(inflight, open, 0));
    // The reservation is visible, so the recycler waits for it even though the writer will not write.
    REQUIRE(SlotReservations(inflight) == 1);
    CommitSlotWrite(inflight);
    REQUIRE(SlotReservations(inflight) == 0);
}

TEST_CASE("NVN epoch slot: a reservation taken before the close is waited for", "[core][nvn_census]") {
    std::atomic<u64> open{7};
    std::atomic<u64> inflight{0};

    REQUIRE(ReserveSlotWrite(inflight, open, 7));
    CloseSlot(open);
    REQUIRE(SlotReservations(inflight) == 1);
    CommitSlotWrite(inflight);
    REQUIRE(SlotReservations(inflight) == 0);
}

TEST_CASE("NVN epoch slot: a writer for the epoch the slot is open for may write", "[core][nvn_census]") {
    std::atomic<u64> open{9};
    std::atomic<u64> inflight{0};
    REQUIRE(ReserveSlotWrite(inflight, open, 9));
    CommitSlotWrite(inflight);
    REQUIRE_FALSE(ReserveSlotWrite(inflight, open, 5));
    CommitSlotWrite(inflight);
    REQUIRE(SlotReservations(inflight) == 0);
}

TEST_CASE("NVN epoch slot: concurrent writers and a recycler never misattribute a count", "[core][nvn_census]") {
    constexpr u64 Recycles = 3000;
    constexpr std::size_t Writers = 3;

    std::atomic<u64> epoch{0};
    std::array<std::atomic<u64>, Epochs> open{};
    std::array<std::atomic<u64>, Epochs> inflight{};
    std::array<std::atomic<u64>, Epochs> cell{};
    for (u64 slot = 0; slot < Epochs; ++slot) {
        open[slot].store(slot == 0 ? 0 : SlotClosed);
    }
    // What each writer meant to count per epoch, against what the recycler found in the slot.
    std::vector<std::atomic<u64>> meant(Recycles + Epochs + 1);
    std::vector<u64> found(Recycles + Epochs + 1, 0);
    std::atomic<u64> late{0};
    std::atomic<u64> total{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> writers;
    for (std::size_t w = 0; w < Writers; ++w) {
        writers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const u64 e = epoch.load(std::memory_order_acquire);
                const std::size_t slot = static_cast<std::size_t>(e % Epochs);
                if (ReserveSlotWrite(inflight[slot], open[slot], e)) {
                    cell[slot].fetch_add(1, std::memory_order_relaxed);
                    meant[e].fetch_add(1, std::memory_order_relaxed);
                } else {
                    late.fetch_add(1, std::memory_order_relaxed);
                }
                CommitSlotWrite(inflight[slot]);
                total.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (u64 next = 1; next <= Recycles; ++next) {
        const std::size_t slot = static_cast<std::size_t>(next % Epochs);
        CloseSlot(open[slot]);
        while (SlotReservations(inflight[slot]) != 0) {
            std::this_thread::yield();
        }
        if (next >= Epochs) {
            found[next - Epochs] += cell[slot].exchange(0, std::memory_order_relaxed);
        } else {
            REQUIRE(cell[slot].load(std::memory_order_relaxed) == 0);
        }
        OpenSlot(open[slot], next);
        epoch.store(next, std::memory_order_seq_cst);
        if (next % 64 == 0) {
            std::this_thread::yield();
        }
    }
    stop.store(true);
    for (std::thread& writer : writers) {
        writer.join();
    }
    // Quiescent: what is left in each slot belongs to the epoch it is open for.
    for (std::size_t slot = 0; slot < Epochs; ++slot) {
        const u64 held = open[slot].load();
        if (held != SlotClosed) {
            found[held] += cell[slot].exchange(0, std::memory_order_relaxed);
        }
    }
    u64 kept = 0;
    for (u64 e = 0; e <= Recycles; ++e) {
        REQUIRE(found[e] == meant[e].load());
        kept += found[e];
    }
    REQUIRE(kept + late.load() == total.load());
}
