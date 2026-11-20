// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace Core::NvnCensus {

/// The protocol that lets many writers count into one of a few recycled frame slots while one
/// recycler empties and reuses the slot, without a writer's count landing in the wrong frame.
///
/// A slot is open for exactly one epoch at a time (`open` holds it), or closed (`SlotClosed`).
/// A writer reserves, then checks that the slot is still open for the epoch it read; the recycler
/// closes the slot, then waits for the writers' reservations to drain. Each side writes its own
/// word and then reads the other's with sequentially consistent ordering, so one of them sees the
/// other: either the writer sees the slot closed (and counts itself late, writing nothing), or the
/// recycler sees the reservation and waits for it. Reading the epoch counter again is not a
/// substitute: a writer can reserve between the recycler's zero reading and the epoch publication.
inline constexpr u64 SlotClosed = ~u64{0};

/// Writer side. True when the slot is open for `epoch` and the caller may write into it; the caller
/// must then call `CommitSlotWrite` once, whatever the answer. A false answer means the call is late.
[[nodiscard]] inline bool ReserveSlotWrite(std::atomic<u64>& inflight, const std::atomic<u64>& open, u64 epoch) {
    inflight.fetch_add(1, std::memory_order_seq_cst);
    return open.load(std::memory_order_seq_cst) == epoch;
}

/// Writer side: ends a reservation. The release pairs with the recycler's acquire reading of zero.
inline void CommitSlotWrite(std::atomic<u64>& inflight) {
    inflight.fetch_sub(1, std::memory_order_release);
}

/// Recycler side, step 1: no writer can reserve this slot for the generation it held from now on.
inline void CloseSlot(std::atomic<u64>& open) {
    open.store(SlotClosed, std::memory_order_seq_cst);
}

/// Recycler side, step 2 (after the reservations drained and the slot was emptied): the slot
/// belongs to `epoch`. Publish the epoch counter only after this.
inline void OpenSlot(std::atomic<u64>& open, u64 epoch) {
    open.store(epoch, std::memory_order_seq_cst);
}

/// Recycler side: whether a writer holding a reservation can still be writing into the slot. The
/// seq_cst load is the recycler's half of the handshake with `ReserveSlotWrite`.
[[nodiscard]] inline u64 SlotReservations(const std::atomic<u64>& inflight) {
    return inflight.load(std::memory_order_seq_cst);
}

} // namespace Core::NvnCensus
