// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/pipeline_prepare.h"

namespace {

using namespace VideoCommon::PipelinePrepare;
using VideoCommon::CacheEnvelope::CacheStats;
using VideoCommon::CacheEnvelope::DeviceIdentity;
using VideoCommon::CacheEnvelope::FirstUse;
using VideoCommon::CacheEnvelope::PrepareOptions;
using VideoCommon::CacheEnvelope::RecordKind;
using VideoCommon::CacheEnvelope::RejectReason;

constexpr u64 Gib = u64{1} << 30;
constexpr u64 PlentyOfRam = u64{64} * Gib;

u64 NextWorkId() {
    static std::atomic<u64> counter{0};
    return ++counter;
}

// One worker has a window of four records, two workers eight.
PrepareOptions Options(u32 workers = 1, u32 ready_cap = 128, u64 budget = u64{256} << 20,
                       u64 min_ram = 2 * Gib) {
    PrepareOptions options;
    options.workers = workers;
    options.ready_cap = ready_cap;
    options.memory_budget_bytes = budget;
    options.min_available_ram_bytes = min_ram;
    return options;
}

Candidate Cand(FirstUse phase, u64 ns, u32 ordinal = 0, u32 length = 100, u32 cost_us = 0) {
    Candidate candidate;
    candidate.meta.kind = RecordKind::Graphics;
    candidate.meta.first_use_phase = phase;
    candidate.meta.first_use_ns = ns;
    candidate.meta.first_use_ordinal = ordinal;
    candidate.meta.build_cost_us = cost_us;
    candidate.length = length;
    return candidate;
}

std::vector<Candidate> Run(size_t count, u32 length = 100) {
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < count; ++i) {
        candidates.push_back(Cand(FirstUse::Run, i, 0, length));
    }
    return candidates;
}

std::vector<u8> Key(u8 value) {
    return {value, 0xAB, 0xCD, value};
}

u64 KeyHash(u8 value) {
    return 0x1000 + value;
}

// Takes one Load task per value (the workers decode several records at once), then queues each
// under the key of its value. The tasks come back in the order taken.
std::vector<Scheduler::Task> LoadAll(Scheduler& scheduler, u64 generation,
                                     std::initializer_list<u8> values,
                                     bool variants_allowed = false) {
    std::vector<Scheduler::Task> tasks;
    for (size_t i = 0; i < values.size(); ++i) {
        tasks.push_back(scheduler.Next(generation, PlentyOfRam));
        REQUIRE(tasks.back().kind == Scheduler::TaskKind::Load);
    }
    size_t index = 0;
    for (const u8 value : values) {
        REQUIRE(scheduler.Loaded(tasks[index].id, generation, KeyHash(value), Key(value),
                                 std::make_shared<int>(value),
                                 variants_allowed) == Scheduler::LoadOutcome::Queued);
        ++index;
    }
    return tasks;
}

Scheduler::Task BuildNext(Scheduler& scheduler, u64 generation) {
    const auto task = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(task.kind == Scheduler::TaskKind::Build);
    return task;
}

// A clock the test moves by hand, so times are exact.
u64 fake_now_ns = 0;

u64 FakeClock() {
    return fake_now_ns;
}

// One worker; the game counts as off the recorded order after `after_ns` of draws that match no
// record.
PrepareOptions Diverging(u32 ready_cap, u64 after_ns) {
    PrepareOptions options = Options(1, ready_cap);
    options.diverged_after_ns = after_ns;
    return options;
}

u64 IndexBytes(size_t count, u32 length) {
    Scheduler probe{Options(), &NextWorkId};
    (void)probe.Begin(Run(count, length));
    return probe.UsedBytes();
}

} // Anonymous namespace

TEST_CASE("Preparation order is boot phase, then first use time, then ordinal, then cost",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(2), &NextWorkId};
    const std::vector<Candidate> candidates{
        Cand(FirstUse::Run, 10),            // 0
        Cand(FirstUse::Boot, 900),          // 1: boot comes before any run record
        Cand(FirstUse::Run, 5, 7),          // 2
        Cand(FirstUse::Run, 5, 3),          // 3: same time, lower ordinal
        Cand(FirstUse::Run, 5, 3, 100, 40), // 4: same time and ordinal, more expensive
        Cand(FirstUse::Boot, 100),          // 5
    };
    const u64 generation = scheduler.Begin(candidates);
    const auto loads = LoadAll(scheduler, generation, {1, 2, 3, 4, 5, 6});
    std::vector<EntryId> loaded;
    std::vector<EntryId> built;
    for (const auto& task : loads) {
        loaded.push_back(task.id);
    }
    for (size_t i = 0; i < loads.size(); ++i) {
        built.push_back(BuildNext(scheduler, generation).id);
    }
    CHECK(loaded == std::vector<EntryId>{5, 1, 4, 3, 2, 0});
    CHECK(built == loaded);
}

TEST_CASE("The queue is built best first, however the decodes finished",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(3));
    std::vector<Scheduler::Task> loads;
    for (int i = 0; i < 3; ++i) {
        loads.push_back(scheduler.Next(generation, PlentyOfRam));
        REQUIRE(loads.back().kind == Scheduler::TaskKind::Load);
    }
    // The decodes end in the opposite order of the priority.
    for (size_t i = loads.size(); i-- > 0;) {
        const u8 value = static_cast<u8>(loads[i].id + 1);
        REQUIRE(scheduler.Loaded(loads[i].id, generation, KeyHash(value), Key(value),
                                 std::make_shared<int>(value),
                                 false) == Scheduler::LoadOutcome::Queued);
    }
    CHECK(BuildNext(scheduler, generation).id == 0);
    CHECK(BuildNext(scheduler, generation).id == 1);
    CHECK(BuildNext(scheduler, generation).id == 2);
}

TEST_CASE("A prepared record is adopted once, with its object, and counted",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(1));
    LoadAll(scheduler, generation, {1});
    const auto build = BuildNext(scheduler, generation);
    CHECK(build.work != 0);
    auto object = std::make_shared<int>(42);
    REQUIRE(scheduler.BuildFinished(build.id, generation, true, object));
    CHECK(scheduler.StateOf(build.id) == State::Ready);
    CHECK(scheduler.ReadyCount() == 1);

    auto demand = scheduler.Request(KeyHash(1), Key(1));
    REQUIRE(demand.kind == Scheduler::DemandKind::Ready);
    REQUIRE(demand.results.size() == 1);
    CHECK(demand.results[0].get() == object.get());
    CHECK(scheduler.StateOf(build.id) == State::Adopted);
    CHECK(scheduler.ReadyCount() == 0);

    // A second ask finds it taken: nothing more to adopt, nothing to build twice.
    CHECK(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Unavailable);
    CacheStats stats;
    scheduler.FillStats(stats);
    CHECK(stats.prepared == 1);
    CHECK(stats.adopted == 1);
    CHECK(stats.demand_promoted == 0);
}

TEST_CASE("A draw before the record is decoded builds normally and the record is then skipped",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(2));
    // Nothing is known about key 1 yet.
    CHECK(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Miss);
    // Its record then turns up: no second build of what the draw built.
    const auto task = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(task.kind == Scheduler::TaskKind::Load);
    CHECK(scheduler.Loaded(task.id, generation, KeyHash(1), Key(1), std::make_shared<int>(1),
                           false) == Scheduler::LoadOutcome::Duplicate);
    CHECK(scheduler.StateOf(task.id) == State::Skipped);
    // The next record is unaffected.
    LoadAll(scheduler, generation, {2});
}

TEST_CASE("Two records of one key are a duplicate unless variants are allowed",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(2));
    const auto first = scheduler.Next(generation, PlentyOfRam);
    const auto second = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(scheduler.Loaded(first.id, generation, KeyHash(1), Key(1), nullptr, false) ==
            Scheduler::LoadOutcome::Queued);
    CHECK(scheduler.Loaded(second.id, generation, KeyHash(1), Key(1), nullptr, false) ==
          Scheduler::LoadOutcome::Duplicate);
}

TEST_CASE("Compute variants of one key are all kept", "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(3));
    // The draw already built its own variant of the key: the recorded ones are still wanted.
    CHECK(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Miss);
    LoadAll(scheduler, generation, {1, 1, 1}, true);
    const auto first = BuildNext(scheduler, generation);
    const auto second = BuildNext(scheduler, generation);
    REQUIRE(scheduler.BuildFinished(first.id, generation, true, std::make_shared<int>(1)));
    REQUIRE(scheduler.BuildFinished(second.id, generation, true, std::make_shared<int>(2)));
    // The two ready variants come at once; the third stays queued for later.
    const auto demand = scheduler.Request(KeyHash(1), Key(1));
    REQUIRE(demand.kind == Scheduler::DemandKind::Ready);
    CHECK(demand.results.size() == 2);
    CHECK(scheduler.StateOf(2) == State::Queued);
    const auto third = BuildNext(scheduler, generation);
    REQUIRE(scheduler.BuildFinished(third.id, generation, true, std::make_shared<int>(3)));
    const auto later = scheduler.Request(KeyHash(1), Key(1));
    REQUIRE(later.kind == Scheduler::DemandKind::Ready);
    CHECK(later.results.size() == 1);
}

TEST_CASE("A hash collision is not a claim: the whole key must match",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(1));
    LoadAll(scheduler, generation, {1});
    // Same hash, different key bytes.
    CHECK(scheduler.Request(KeyHash(1), Key(2)).kind == Scheduler::DemandKind::Miss);
    CHECK(scheduler.StateOf(0) == State::Queued);
}

TEST_CASE("A draw that finds its record queued claims it and preparation never builds it",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(2));
    LoadAll(scheduler, generation, {1, 2});
    const u64 before = scheduler.UsedBytes();

    const auto claim = scheduler.Request(KeyHash(1), Key(1));
    CHECK(claim.kind == Scheduler::DemandKind::Claimed);
    CHECK(scheduler.StateOf(0) == State::Claimed);
    CHECK(scheduler.UsedBytes() < before); // its decoded record is released
    // The worker skips the claimed one.
    CHECK(BuildNext(scheduler, generation).id == 1);

    CacheStats stats;
    scheduler.FillStats(stats);
    CHECK(stats.demand_promoted == 1);
}

TEST_CASE("Two requesters of one queued record: exactly one claim",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(1));
    LoadAll(scheduler, generation, {1});

    constexpr int Requesters = 8;
    std::atomic<bool> go{false};
    std::atomic<int> claimed{0};
    std::atomic<int> unavailable{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < Requesters; ++i) {
        threads.emplace_back([&] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            const auto demand = scheduler.Request(KeyHash(1), Key(1));
            if (demand.kind == Scheduler::DemandKind::Claimed) {
                ++claimed;
            } else if (demand.kind == Scheduler::DemandKind::Unavailable) {
                ++unavailable;
            }
        });
    }
    go = true;
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(claimed.load() == 1);
    CHECK(unavailable.load() == Requesters - 1);
    CacheStats stats;
    scheduler.FillStats(stats);
    CHECK(stats.demand_promoted == 1);
}

TEST_CASE("Two requesters of one ready record: the object is adopted once",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(1));
    LoadAll(scheduler, generation, {1});
    const auto build = BuildNext(scheduler, generation);
    REQUIRE(scheduler.BuildFinished(build.id, generation, true, std::make_shared<int>(5)));

    std::atomic<bool> go{false};
    std::atomic<int> got{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            const auto demand = scheduler.Request(KeyHash(1), Key(1));
            if (demand.kind == Scheduler::DemandKind::Ready) {
                got += static_cast<int>(demand.results.size());
            }
        });
    }
    go = true;
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(got.load() == 1);
}

TEST_CASE("A draw that finds its record building waits for that build, not a second one",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(1));
    LoadAll(scheduler, generation, {1});
    const auto build = BuildNext(scheduler, generation);

    const auto demand = scheduler.Request(KeyHash(1), Key(1));
    REQUIRE(demand.kind == Scheduler::DemandKind::Building);
    CHECK(demand.id == build.id);
    CHECK(demand.work == build.work); // the wait can name its producer
    // A second asker also waits; neither claims it.
    CHECK(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Building);

    std::promise<void> entered;
    auto waiter = std::async(std::launch::async, [&] {
        entered.set_value();
        scheduler.Await(demand.id, demand.generation);
        return scheduler.StateOf(demand.id);
    });
    entered.get_future().wait();
    auto object = std::make_shared<int>(9);
    REQUIRE(scheduler.BuildFinished(build.id, generation, true, object));
    // However the threads interleave, the waiter returns after the build is Ready. The bound
    // only keeps a missed wakeup from hanging the run.
    REQUIRE(waiter.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
    CHECK(waiter.get() == State::Ready);

    const auto adopted = scheduler.Request(KeyHash(1), Key(1));
    REQUIRE(adopted.kind == Scheduler::DemandKind::Ready);
    CHECK(adopted.results[0].get() == object.get());
}

TEST_CASE("Await on a record that is not building returns at once",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(1));
    LoadAll(scheduler, generation, {1});
    scheduler.Await(0, generation);     // queued
    scheduler.Await(7, generation);     // no such entry
    scheduler.Await(0, generation + 1); // another generation
    SUCCEED();
}

TEST_CASE("Cancelling ends the generation: waiters wake, completions are refused, objects come back",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(3));
    LoadAll(scheduler, generation, {1, 2, 3});
    const auto ready_build = BuildNext(scheduler, generation); // entry 0
    const auto in_flight = BuildNext(scheduler, generation);   // entry 1; entry 2 stays queued
    auto object = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = object;
    REQUIRE(scheduler.BuildFinished(ready_build.id, generation, true, std::move(object)));
    REQUIRE(scheduler.Request(KeyHash(2), Key(2)).kind == Scheduler::DemandKind::Building);

    auto waiter = std::async(std::launch::async, [&] { scheduler.Await(in_flight.id, generation); });

    auto dropped = scheduler.Cancel();
    REQUIRE(waiter.wait_for(std::chrono::seconds(30)) == std::future_status::ready); // woke
    waiter.get();
    CHECK(dropped.size() >= 2); // the ready object and the queued record
    dropped.clear();
    CHECK(watch.expired()); // nothing in the scheduler still holds the ready object
    CHECK_FALSE(scheduler.Running());
    CHECK(scheduler.UsedBytes() == 0);
    CHECK(scheduler.ReadyCount() == 0);

    // The build that was running when the title stopped finishes late: refused, the caller keeps
    // the object and destroys it.
    CHECK_FALSE(scheduler.BuildFinished(in_flight.id, generation, true, std::make_shared<int>(2)));
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Done);
    CHECK(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Miss);
    CHECK_FALSE(scheduler.LoadFailed(2, generation));
    CHECK(scheduler.Loaded(2, generation, KeyHash(3), Key(3), nullptr, false) ==
          Scheduler::LoadOutcome::Stale);
}

TEST_CASE("A completion from an older generation never lands in the new one",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 old_generation = scheduler.Begin(Run(1));
    LoadAll(scheduler, old_generation, {1});
    const auto old_build = BuildNext(scheduler, old_generation);
    (void)scheduler.Cancel();

    // The same entry id is Building again in the next generation.
    const u64 generation = scheduler.Begin(Run(1));
    CHECK(generation != old_generation);
    LoadAll(scheduler, generation, {1});
    const auto build = BuildNext(scheduler, generation);
    REQUIRE(build.id == old_build.id);

    CHECK_FALSE(scheduler.BuildFinished(old_build.id, old_generation, true,
                                        std::make_shared<int>(1)));
    CHECK(scheduler.StateOf(build.id) == State::Building);
    CHECK(scheduler.Next(old_generation, PlentyOfRam).kind == Scheduler::TaskKind::Done);
    CHECK(scheduler.BuildFinished(build.id, generation, true, std::make_shared<int>(2)));
}

TEST_CASE("The memory budget admits up to its exact size and then pauses once",
          "[video_core][pipeline_prepare]") {
    constexpr u32 Length = 1000;
    const u64 cost = RecordCost(Length);
    const u64 index_bytes = IndexBytes(3, Length);

    // Room for exactly two records: the budget equals what the index and two records need.
    Scheduler scheduler{Options(1, 128, index_bytes + 2 * cost), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(3, Length));
    LoadAll(scheduler, generation, {1, 2});
    CHECK(scheduler.UsedBytes() == index_bytes + 2 * cost);

    // The third does not fit while both are held; nothing else is queued once they are built,
    // and asking again does not count another pause.
    const auto build1 = BuildNext(scheduler, generation);
    const auto build2 = BuildNext(scheduler, generation);
    CHECK(build1.id == 0);
    CHECK(build2.id == 1);
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);
    CacheStats stats;
    scheduler.FillStats(stats);
    CHECK(stats.budget_pauses == 1);
    CHECK(stats.memory_high_water_bytes == index_bytes + 2 * cost);

    // Builds that end give their record's share back down to the ready bookkeeping: room again.
    REQUIRE(scheduler.BuildFinished(build1.id, generation, true, std::make_shared<int>(1)));
    REQUIRE(scheduler.BuildFinished(build2.id, generation, true, std::make_shared<int>(2)));
    CHECK(scheduler.UsedBytes() == index_bytes + 2 * ReadyBookkeepingBytes);
    const auto task = scheduler.Next(generation, PlentyOfRam);
    CHECK(task.kind == Scheduler::TaskKind::Load);
    CHECK(task.id == 2);
}

TEST_CASE("One byte under the budget refuses the second record", "[video_core][pipeline_prepare]") {
    constexpr u32 Length = 1000;
    const u64 cost = RecordCost(Length);
    const u64 index_bytes = IndexBytes(2, Length);
    Scheduler scheduler{Options(1, 128, index_bytes + 2 * cost - 1), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(2, Length));
    LoadAll(scheduler, generation, {1});
    BuildNext(scheduler, generation); // record 0 stays in flight: nothing else frees memory
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);
}

TEST_CASE("A record larger than the whole budget is skipped and the queue moves on",
          "[video_core][pipeline_prepare]") {
    const u64 index_bytes = IndexBytes(2, 100);
    // Room for a record of 1000 bytes exactly, not 1001.
    Scheduler scheduler{Options(1, 128, index_bytes + RecordCost(1000)), &NextWorkId};
    const std::vector<Candidate> candidates{Cand(FirstUse::Run, 1, 0, 1001),
                                            Cand(FirstUse::Run, 2, 0, 1000)};
    const u64 generation = scheduler.Begin(candidates);
    REQUIRE(scheduler.UsedBytes() == index_bytes);
    const auto task = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(task.kind == Scheduler::TaskKind::Load);
    CHECK(task.id == 1);
    CHECK(scheduler.StateOf(0) == State::Skipped);
}

TEST_CASE("The ready cap holds builds back at exactly the cap, releases nothing and counts the wait",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(2, /*ready_cap=*/2), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(3));
    LoadAll(scheduler, generation, {1, 2, 3});

    const auto first = BuildNext(scheduler, generation);  // one building: under the cap
    const auto second = BuildNext(scheduler, generation); // two building: the cap
    CHECK(first.evicted == nullptr);
    CHECK(second.evicted == nullptr);
    // Nothing is ready to release, so the third record waits, however often it is asked.
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);
    CacheStats stats;
    scheduler.FillStats(stats);
    CHECK(stats.budget_pauses == 1); // one pause, however often it was asked
    CHECK(stats.held_back == 0);     // workers are the limit here, not unused ready objects

    // One ready and one building make the cap again. The ready object is unused, but it is the
    // one the game reaches first: the third record waits instead of releasing it.
    REQUIRE(scheduler.BuildFinished(first.id, generation, true, std::make_shared<int>(1)));
    for (int asked = 0; asked < 3; ++asked) {
        const auto held = scheduler.Next(generation, PlentyOfRam);
        CHECK(held.kind == Scheduler::TaskKind::Wait);
        CHECK(held.evicted == nullptr);
    }
    CHECK(scheduler.ReadyCount() == 1);
    scheduler.FillStats(stats);
    CHECK(stats.held_back == 1); // once for the run of waits
    CHECK(stats.evicted == 0);

    // The draw adopts it: the slot is free and the third record is built.
    const auto adopted = scheduler.Request(KeyHash(1), Key(1));
    REQUIRE(adopted.kind == Scheduler::DemandKind::Ready);
    CHECK(*std::static_pointer_cast<int>(adopted.results[0]) == 1);
    const auto third = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(third.kind == Scheduler::TaskKind::Build);
    CHECK(third.id == 2);
    CHECK(third.evicted == nullptr);

    // Two building again: the cap, and nothing is left to take.
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);
    REQUIRE(scheduler.BuildFinished(second.id, generation, true, std::make_shared<int>(2)));
    CHECK(scheduler.Request(KeyHash(2), Key(2)).kind == Scheduler::DemandKind::Ready);
}

TEST_CASE("Records are built in recorded first-use order and the early ones are never released",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(1, /*ready_cap=*/3), &NextWorkId};
    // Records 0 to 3 were first used at 0, 1, 2 and 3 ns: the order they are built in.
    const u64 generation = scheduler.Begin(Run(4));
    LoadAll(scheduler, generation, {1, 2, 3, 4});
    for (EntryId expected = 0; expected < 3; ++expected) {
        const auto build = BuildNext(scheduler, generation);
        CHECK(build.id == expected);
        REQUIRE(scheduler.BuildFinished(build.id, generation, true,
                                        std::make_shared<int>(static_cast<int>(expected))));
    }

    // The cap is full of the three earliest records. The fourth waits, however often it is asked.
    for (int asked = 0; asked < 5; ++asked) {
        const auto held = scheduler.Next(generation, PlentyOfRam);
        CHECK(held.kind == Scheduler::TaskKind::Wait);
        CHECK(held.evicted == nullptr);
    }
    CHECK(scheduler.StateOf(0) == State::Ready);
    CHECK(scheduler.StateOf(1) == State::Ready);
    CHECK(scheduler.StateOf(2) == State::Ready);
    CHECK(scheduler.StateOf(3) == State::Queued);
    CacheStats stats;
    scheduler.FillStats(stats);
    CHECK(stats.prepared == 3);
    CHECK(stats.evicted == 0);
    CHECK(stats.held_back == 1);

    // The game reaches the records in that order: each one adopted moves the frontier on.
    REQUIRE(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Ready);
    const auto fourth = BuildNext(scheduler, generation);
    CHECK(fourth.id == 3);
    CHECK(fourth.evicted == nullptr);
    REQUIRE(scheduler.BuildFinished(fourth.id, generation, true, std::make_shared<int>(4)));
    for (const u8 value : std::initializer_list<u8>{2, 3, 4}) {
        REQUIRE(scheduler.Request(KeyHash(value), Key(value)).kind ==
                Scheduler::DemandKind::Ready);
    }
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Done);
    scheduler.FillStats(stats);
    CHECK(stats.records == 4);
    CHECK(stats.prepared == 4);
    CHECK(stats.adopted == 4); // every prepared object was used
    CHECK(stats.evicted == 0);
    CHECK(stats.demand_building == 0);
    CHECK(stats.late_records == 0);
}

TEST_CASE("The counters tell objects ready in time from the ones the game found late",
          "[video_core][pipeline_prepare]") {
    fake_now_ns = 0;
    Scheduler scheduler{Options(2), &NextWorkId, &FakeClock};
    const u64 generation = scheduler.Begin(Run(3));
    LoadAll(scheduler, generation, {1, 2, 3});
    const auto first = BuildNext(scheduler, generation);
    const auto second = BuildNext(scheduler, generation);

    fake_now_ns = 100;
    REQUIRE(scheduler.BuildFinished(first.id, generation, true, std::make_shared<int>(1)));
    // Ready at 100 and used at 350: 250 ns early.
    fake_now_ns = 350;
    REQUIRE(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Ready);

    // Asked for at 500 while it builds, asked again at 600, ready at 900: it waited 400 ns,
    // counted once.
    fake_now_ns = 500;
    REQUIRE(scheduler.Request(KeyHash(2), Key(2)).kind == Scheduler::DemandKind::Building);
    fake_now_ns = 600;
    REQUIRE(scheduler.Request(KeyHash(2), Key(2)).kind == Scheduler::DemandKind::Building);
    fake_now_ns = 900;
    REQUIRE(scheduler.BuildFinished(second.id, generation, true, std::make_shared<int>(2)));
    // Not counted as early: its draw was waiting.
    REQUIRE(scheduler.Request(KeyHash(2), Key(2)).kind == Scheduler::DemandKind::Ready);

    // Asked for at 1000 while still queued: the draw builds it.
    fake_now_ns = 1000;
    REQUIRE(scheduler.Request(KeyHash(3), Key(3)).kind == Scheduler::DemandKind::Claimed);
    // A key no record has.
    CHECK(scheduler.Request(KeyHash(9), Key(9)).kind == Scheduler::DemandKind::Miss);

    CacheStats stats;
    scheduler.FillStats(stats);
    CHECK(stats.records == 3);
    CHECK(stats.prepared == 2);
    CHECK(stats.adopted == 2);
    CHECK(stats.adopted_lead_ns == 250);
    CHECK(stats.demand_building == 1);
    CHECK(stats.demand_promoted == 1);
    CHECK(stats.demand_evicted == 0);
    CHECK(stats.demand_unknown == 1);
    CHECK(stats.late_records == 2);
    CHECK(stats.late_wait_ns == 400);
    CHECK(stats.late_wait_max_ns == 400);

    const auto late = scheduler.LateRecords();
    REQUIRE(late.size() == 2);
    CHECK(late[0].rank == 1);
    CHECK(late[0].recorded_first_use_ns == 1);
    CHECK(late[0].outcome == Scheduler::LateOutcome::Building);
    CHECK(late[0].demanded_ns == 500);
    CHECK(late[0].ready_ns == 900);
    CHECK(late[1].rank == 2);
    CHECK(late[1].outcome == Scheduler::LateOutcome::Claimed);
    CHECK(late[1].demanded_ns == 1000);
    CHECK(late[1].ready_ns == 0);
}

TEST_CASE("A demand is never held back by the cap, the budget or the RAM pause",
          "[video_core][pipeline_prepare]") {
    // A one byte budget admits nothing, yet a draw is answered at once.
    Scheduler scheduler{Options(1, /*ready_cap=*/1, /*budget=*/1, /*min_ram=*/2 * Gib),
                        &NextWorkId};
    const u64 generation = scheduler.Begin(Run(1));
    CHECK(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Miss);
    CHECK(scheduler.Next(generation, 0).kind != Scheduler::TaskKind::Load);
}

TEST_CASE("RAM pause starts below the minimum and ends a quarter above it",
          "[video_core][pipeline_prepare]") {
    constexpr u64 MinRam = 1000;
    Scheduler scheduler{Options(1, 128, u64{256} << 20, MinRam), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(2));

    CHECK(scheduler.Next(generation, MinRam - 1).kind == Scheduler::TaskKind::Wait);
    CHECK(scheduler.Next(generation, MinRam).kind == Scheduler::TaskKind::Wait); // still paused
    CHECK(scheduler.Next(generation, MinRam + MinRam / 4 - 1).kind == Scheduler::TaskKind::Wait);
    CacheStats stats;
    scheduler.FillStats(stats);
    CHECK(stats.budget_pauses == 1);
    CHECK(scheduler.Next(generation, MinRam + MinRam / 4).kind == Scheduler::TaskKind::Load);

    // Not paused before: exactly the minimum is enough.
    Scheduler fresh{Options(1, 128, u64{256} << 20, MinRam), &NextWorkId};
    const u64 fresh_generation = fresh.Begin(Run(1));
    CHECK(fresh.Next(fresh_generation, MinRam).kind == Scheduler::TaskKind::Load);
}

TEST_CASE("A failed record does not poison the queue", "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(3));
    // Three decodes in flight: two succeed, the third fails to decode.
    const auto first = scheduler.Next(generation, PlentyOfRam);
    const auto second = scheduler.Next(generation, PlentyOfRam);
    const auto third = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(third.kind == Scheduler::TaskKind::Load);
    REQUIRE(scheduler.Loaded(first.id, generation, KeyHash(1), Key(1), nullptr, false) ==
            Scheduler::LoadOutcome::Queued);
    REQUIRE(scheduler.Loaded(second.id, generation, KeyHash(2), Key(2), nullptr, false) ==
            Scheduler::LoadOutcome::Queued);
    const u64 held = scheduler.UsedBytes();
    CHECK(scheduler.LoadFailed(third.id, generation));
    CHECK(scheduler.StateOf(third.id) == State::Failed);
    CHECK(scheduler.UsedBytes() < held);

    // The first build fails; the queue goes on with the next record.
    const auto failing = BuildNext(scheduler, generation);
    REQUIRE(failing.id == first.id);
    CHECK(scheduler.BuildFinished(failing.id, generation, false, nullptr));
    CHECK(scheduler.StateOf(first.id) == State::Failed);
    CHECK(BuildNext(scheduler, generation).id == second.id);
    // The failed key is a normal miss for the draw: neither a claim nor a wait.
    CHECK(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Unavailable);
    // Nothing left to take while the second builds.
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);

    CacheStats stats;
    scheduler.FillStats(stats);
    CHECK(stats.prepare_failures == 2);
    CHECK(stats.prepared == 0);
}

TEST_CASE("The generation is done when every record has ended, and not before",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(2));
    const u64 index_bytes = scheduler.UsedBytes();
    const auto first = scheduler.Next(generation, PlentyOfRam);
    const auto second = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(first.kind == Scheduler::TaskKind::Load);
    REQUIRE(second.kind == Scheduler::TaskKind::Load);
    // Both loading: nothing to take, but the end is not here.
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);
    CHECK(scheduler.InFlight() == 2);
    CHECK(scheduler.LoadSkipped(first.id, generation));
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);
    CHECK(scheduler.LoadSkipped(second.id, generation));
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Done);
    CHECK(scheduler.InFlight() == 0);
    CHECK(scheduler.UsedBytes() == index_bytes);
}

TEST_CASE("An empty index is done at once", "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin({});
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Done);
}

TEST_CASE("WaitForChange returns at once when something changed since the epoch",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(1));
    const auto task = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(task.kind == Scheduler::TaskKind::Load);
    const auto wait = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(wait.kind == Scheduler::TaskKind::Wait);
    // The load ends after the epoch was read: the wait must not sleep through the change.
    REQUIRE(scheduler.LoadSkipped(task.id, generation));
    const auto begin = std::chrono::steady_clock::now();
    scheduler.WaitForChange(wait.epoch, std::chrono::seconds(30));
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(20));
    SUCCEED();
}

TEST_CASE("Driver blob chunking splits at exactly the chunk size", "[video_core][pipeline_prepare]") {
    const auto chunks_of = [](size_t size) {
        std::vector<u8> blob(size, 7);
        std::vector<std::vector<u8>> chunks;
        REQUIRE(ForEachDriverBlobChunk(blob, [&](std::span<const u8> chunk) {
            chunks.emplace_back(chunk.begin(), chunk.end());
            return true;
        }));
        return chunks;
    };
    CHECK(chunks_of(0).size() == 1);
    CHECK(chunks_of(1).size() == 1);
    CHECK(chunks_of(DriverBlobChunkBytes).size() == 1);
    const auto two = chunks_of(DriverBlobChunkBytes + 1);
    REQUIRE(two.size() == 2);
    CHECK(two[0].size() == DriverBlobChunkHeaderBytes + DriverBlobChunkBytes);
    CHECK(two[1].size() == DriverBlobChunkHeaderBytes + 1);
    // Equal contents, different records: the writer's duplicate check cannot merge them.
    CHECK(two[0] != two[1]);
    // Every chunk fits a record.
    CHECK(two[0].size() <= VideoCommon::CacheEnvelope::MaxRecordBytes);
}

TEST_CASE("Driver blob chunking stops when a record cannot be appended",
          "[video_core][pipeline_prepare]") {
    std::vector<u8> blob(DriverBlobChunkBytes * 3, 1);
    int calls = 0;
    CHECK_FALSE(ForEachDriverBlobChunk(blob, [&](std::span<const u8>) { return ++calls < 2; }));
    CHECK(calls == 2);
}

namespace {

DeviceIdentity Device() {
    DeviceIdentity device;
    device.vendor_id = 0x10DE;
    device.device_id = 0x2684;
    for (size_t i = 0; i < device.pipeline_cache_uuid.size(); ++i) {
        device.pipeline_cache_uuid[i] = static_cast<u8>(0x40 + i);
    }
    return device;
}

std::vector<u8> BlobFor(const DeviceIdentity& device, size_t payload) {
    std::vector<u8> blob(32 + payload, 0x5A);
    const u32 header_size = 32;
    const u32 version = 1;
    std::memcpy(blob.data(), &header_size, 4);
    std::memcpy(blob.data() + 4, &version, 4);
    std::memcpy(blob.data() + 8, &device.vendor_id, 4);
    std::memcpy(blob.data() + 12, &device.device_id, 4);
    std::memcpy(blob.data() + 16, device.pipeline_cache_uuid.data(), 16);
    return blob;
}

std::vector<std::vector<u8>> Chunks(const std::vector<u8>& blob) {
    std::vector<std::vector<u8>> chunks;
    REQUIRE(ForEachDriverBlobChunk(blob, [&](std::span<const u8> chunk) {
        chunks.emplace_back(chunk.begin(), chunk.end());
        return true;
    }));
    return chunks;
}

RejectReason Join(const std::vector<std::vector<u8>>& chunks, const DeviceIdentity& device,
                  std::vector<u8>& out) {
    return JoinDriverBlob(
        chunks.size(),
        [&](size_t index, std::vector<u8>& payload) {
            payload = chunks[index];
            return RejectReason::None;
        },
        device, out);
}

} // Anonymous namespace

TEST_CASE("A driver blob survives chunking and joining unchanged", "[video_core][pipeline_prepare]") {
    const auto device = Device();
    const auto blob = BlobFor(device, DriverBlobChunkBytes * 2 + 5);
    std::vector<u8> joined;
    REQUIRE(Join(Chunks(blob), device, joined) == RejectReason::None);
    CHECK(joined == blob);
}

TEST_CASE("A driver blob made of equal chunks is not merged or reordered",
          "[video_core][pipeline_prepare]") {
    const auto device = Device();
    // Header, then three chunks of identical zero padding.
    auto blob = BlobFor(device, DriverBlobChunkBytes * 3);
    std::fill(blob.begin() + 32, blob.end(), u8{0});
    auto chunks = Chunks(blob);
    REQUIRE(chunks.size() == 4);
    std::vector<u8> joined;
    CHECK(Join(chunks, device, joined) == RejectReason::None);
    CHECK(joined == blob);
    std::swap(chunks[1], chunks[2]);
    CHECK(Join(chunks, device, joined) == RejectReason::BadRecord);
    CHECK(joined.empty());
}

TEST_CASE("A driver blob with a missing or foreign chunk is rejected", "[video_core][pipeline_prepare]") {
    const auto device = Device();
    const auto blob = BlobFor(device, DriverBlobChunkBytes + 10);
    auto chunks = Chunks(blob);
    REQUIRE(chunks.size() == 2);
    std::vector<u8> joined;

    auto missing = chunks;
    missing.pop_back(); // the first chunk says there are two
    CHECK(Join(missing, device, joined) == RejectReason::BadRecord);

    auto short_chunk = chunks;
    short_chunk[1].resize(DriverBlobChunkHeaderBytes - 1);
    CHECK(Join(short_chunk, device, joined) == RejectReason::BadRecord);

    CHECK(Join({}, device, joined) == RejectReason::BadRecord);

    // A read failure comes back as it is.
    CHECK(JoinDriverBlob(
              2, [](size_t, std::vector<u8>&) { return RejectReason::BadChecksum; }, device,
              joined) == RejectReason::BadChecksum);
}

TEST_CASE("A driver blob is checked against the device before it can reach the driver",
          "[video_core][pipeline_prepare]") {
    const auto device = Device();
    const auto blob = BlobFor(device, 64);
    CHECK(ValidateDriverBlobHeader(blob, device) == RejectReason::None);
    // Exactly the header is enough; one byte less is truncated.
    CHECK(ValidateDriverBlobHeader(std::span<const u8>(blob).first(32), device) ==
          RejectReason::None);
    CHECK(ValidateDriverBlobHeader(std::span<const u8>(blob).first(31), device) ==
          RejectReason::Truncated);
    CHECK(ValidateDriverBlobHeader({}, device) == RejectReason::Truncated);

    auto other = device;
    other.pipeline_cache_uuid[15] ^= 1; // the last byte of the UUID
    CHECK(ValidateDriverBlobHeader(blob, other) == RejectReason::PipelineUuid);
    other = device;
    other.pipeline_cache_uuid[0] ^= 1;
    CHECK(ValidateDriverBlobHeader(blob, other) == RejectReason::PipelineUuid);
    other = device;
    other.vendor_id += 1;
    CHECK(ValidateDriverBlobHeader(blob, other) == RejectReason::Device);
    other = device;
    other.device_id += 1;
    CHECK(ValidateDriverBlobHeader(blob, other) == RejectReason::Device);

    auto bad_size = blob;
    bad_size[0] = 31; // a header size under the version one header
    CHECK(ValidateDriverBlobHeader(bad_size, device) == RejectReason::BadRecord);
    auto bad_version = blob;
    bad_version[4] = 2;
    CHECK(ValidateDriverBlobHeader(bad_version, device) == RejectReason::BadRecord);

    // A declared header size reaching past the blob is not a header of this blob.
    const auto declare = [&](u32 size) {
        auto declared = blob;
        std::memcpy(declared.data(), &size, sizeof(size));
        return declared;
    };
    CHECK(ValidateDriverBlobHeader(declare(static_cast<u32>(blob.size())), device) ==
          RejectReason::None); // exactly the blob
    CHECK(ValidateDriverBlobHeader(declare(static_cast<u32>(blob.size()) + 1), device) ==
          RejectReason::BadRecord);
    CHECK(ValidateDriverBlobHeader(declare(0xFFFFFFFFu), device) == RejectReason::BadRecord);
    // A blob of exactly the header, declaring more than it holds.
    const auto header_only = declare(33);
    CHECK(ValidateDriverBlobHeader(std::span<const u8>(header_only).first(32), device) ==
          RejectReason::BadRecord);
}

TEST_CASE("A joined blob with a foreign UUID is rejected and not returned",
          "[video_core][pipeline_prepare]") {
    const auto device = Device();
    auto other = device;
    other.pipeline_cache_uuid[3] ^= 0x80;
    std::vector<u8> joined;
    CHECK(Join(Chunks(BlobFor(other, 100)), device, joined) == RejectReason::PipelineUuid);
    CHECK(joined.empty());
}

TEST_CASE("A held back build leaves its record to the draw that needs it",
          "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(2));
    LoadAll(scheduler, generation, {1, 2});

    // The worker waits out a burst of runtime compilation: the record stays queued...
    CHECK(scheduler.Next(generation, PlentyOfRam, true).kind == Scheduler::TaskKind::Wait);
    CHECK(scheduler.StateOf(0) == State::Queued);
    // ...so the draw takes it at once, instead of waiting for a build nobody has started.
    CHECK(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Claimed);
    CHECK(scheduler.StateOf(0) == State::Claimed);

    // The burst is over: the other record is built.
    const auto build = scheduler.Next(generation, PlentyOfRam, false);
    REQUIRE(build.kind == Scheduler::TaskKind::Build);
    CHECK(build.id == 1);
}

TEST_CASE("Holding builds back does not hold back decoding", "[video_core][pipeline_prepare]") {
    Scheduler scheduler{Options(), &NextWorkId};
    const u64 generation = scheduler.Begin(Run(2));
    CHECK(scheduler.Next(generation, PlentyOfRam, true).kind == Scheduler::TaskKind::Load);
}

TEST_CASE("A game that left the recorded order has its unused ready objects released, oldest first",
          "[video_core][pipeline_prepare]") {
    fake_now_ns = 10;
    Scheduler scheduler{Diverging(/*ready_cap=*/2, /*after_ns=*/1000), &NextWorkId, &FakeClock};
    const u64 generation = scheduler.Begin(Run(4));
    LoadAll(scheduler, generation, {1, 2, 3, 4});
    std::weak_ptr<int> first_watch;
    for (const int value : {1, 2}) {
        const auto build = BuildNext(scheduler, generation);
        auto object = std::make_shared<int>(value);
        if (value == 1) {
            first_watch = object;
        }
        REQUIRE(scheduler.BuildFinished(build.id, generation, true, std::move(object)));
    }
    CHECK(scheduler.ReadyCount() == 2); // the cap, and the game has asked for neither

    // Nothing has been asked for yet, or only what no record has for less than the bound: the
    // records wait.
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);
    fake_now_ns = 100;
    CHECK(scheduler.Request(KeyHash(9), Key(9)).kind == Scheduler::DemandKind::Miss);
    fake_now_ns = 1099; // one ns short of the bound
    const auto short_of = scheduler.Next(generation, PlentyOfRam);
    CHECK(short_of.kind == Scheduler::TaskKind::Wait);
    CHECK(short_of.evicted == nullptr);

    // At the bound record 3 is built: the oldest ready object makes the room.
    fake_now_ns = 1100;
    auto third = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(third.kind == Scheduler::TaskKind::Build);
    CHECK(third.id == 2);
    REQUIRE(third.evicted != nullptr);
    CHECK(*std::static_pointer_cast<int>(third.evicted) == 1);
    third.evicted.reset();
    CHECK(first_watch.expired()); // nothing in the scheduler keeps it
    CHECK(scheduler.StateOf(0) == State::Evicted);
    CHECK(scheduler.ReadyCount() == 1);
    REQUIRE(scheduler.BuildFinished(third.id, generation, true, std::make_shared<int>(3)));

    // And record 4: the game still matches nothing, so the second one goes, not the newest.
    auto fourth = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(fourth.kind == Scheduler::TaskKind::Build);
    CHECK(fourth.id == 3);
    REQUIRE(fourth.evicted != nullptr);
    CHECK(*std::static_pointer_cast<int>(fourth.evicted) == 2);
    REQUIRE(scheduler.BuildFinished(fourth.id, generation, true, std::make_shared<int>(4)));

    // Everything was visited; the last two stay ready, nothing more is released.
    const auto done = scheduler.Next(generation, PlentyOfRam);
    CHECK(done.kind == Scheduler::TaskKind::Done);
    CHECK(done.evicted == nullptr);
    CHECK(scheduler.ReadyCount() == 2);
    CacheStats stats;
    scheduler.FillStats(stats);
    CHECK(stats.prepared == 4);
    CHECK(stats.evicted == 2);
    CHECK(stats.held_back == 1);

    // A released record is built when its draw comes, and says it was ready earlier; a kept one
    // is adopted.
    CHECK(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Unavailable);
    const auto kept = scheduler.Request(KeyHash(4), Key(4));
    REQUIRE(kept.kind == Scheduler::DemandKind::Ready);
    CHECK(*std::static_pointer_cast<int>(kept.results[0]) == 4);
    scheduler.FillStats(stats);
    CHECK(stats.demand_evicted == 1);
    CHECK(stats.late_records == 1);
    const auto late = scheduler.LateRecords();
    REQUIRE(late.size() == 1);
    CHECK(late[0].outcome == Scheduler::LateOutcome::Released);
    CHECK(late[0].ready_ns == 10);
    CHECK(late[0].demanded_ns == 1100);
}

TEST_CASE("A draw that finds a record restarts the clock of a game that may have left the order",
          "[video_core][pipeline_prepare]") {
    fake_now_ns = 0;
    Scheduler scheduler{Diverging(/*ready_cap=*/2, /*after_ns=*/1000), &NextWorkId, &FakeClock};
    const u64 generation = scheduler.Begin(Run(4));
    LoadAll(scheduler, generation, {1, 2, 3, 4});
    for (const int value : {1, 2}) {
        const auto build = BuildNext(scheduler, generation);
        REQUIRE(scheduler.BuildFinished(build.id, generation, true,
                                        std::make_shared<int>(value)));
    }
    // Record 1 is used; its slot goes to record 3. Ready (record 2) plus building (3) is the cap
    // and record 4 is queued.
    fake_now_ns = 100;
    REQUIRE(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Ready);
    REQUIRE(BuildNext(scheduler, generation).id == 2);

    // Something no record has is asked for at 200. At 900 a record is found again (here an
    // object of the same record used again), so a miss 1000 ns old at 1300 is not one.
    fake_now_ns = 200;
    CHECK(scheduler.Request(KeyHash(9), Key(9)).kind == Scheduler::DemandKind::Miss);
    fake_now_ns = 900;
    CHECK(scheduler.Request(KeyHash(1), Key(1)).kind == Scheduler::DemandKind::Unavailable);
    fake_now_ns = 1300;
    const auto still_waiting = scheduler.Next(generation, PlentyOfRam);
    CHECK(still_waiting.kind == Scheduler::TaskKind::Wait);
    CHECK(still_waiting.evicted == nullptr);

    // Misses again from 1300: the bound is reached at 2300.
    CHECK(scheduler.Request(KeyHash(9), Key(9)).kind == Scheduler::DemandKind::Miss);
    fake_now_ns = 2299;
    CHECK(scheduler.Next(generation, PlentyOfRam).kind == Scheduler::TaskKind::Wait);
    fake_now_ns = 2300;
    const auto released = scheduler.Next(generation, PlentyOfRam);
    REQUIRE(released.kind == Scheduler::TaskKind::Build);
    CHECK(released.id == 3);
    REQUIRE(released.evicted != nullptr);
    CHECK(*std::static_pointer_cast<int>(released.evicted) == 2);
}

TEST_CASE("A ready object is released only for a record that is waiting to be built",
          "[video_core][pipeline_prepare]") {
    fake_now_ns = 0;
    Scheduler scheduler{Diverging(/*ready_cap=*/2, /*after_ns=*/1000), &NextWorkId, &FakeClock};
    const u64 generation = scheduler.Begin(Run(3));
    LoadAll(scheduler, generation, {1, 2, 3});
    for (const int value : {1, 2}) {
        const auto build = BuildNext(scheduler, generation);
        REQUIRE(scheduler.BuildFinished(build.id, generation, true,
                                        std::make_shared<int>(value)));
    }
    // The game has gone its own way for longer than the bound.
    fake_now_ns = 100;
    REQUIRE(scheduler.Request(KeyHash(9), Key(9)).kind == Scheduler::DemandKind::Miss);
    fake_now_ns = 5000;
    // Builds held back for runtime compilation do not release anything.
    const auto held = scheduler.Next(generation, PlentyOfRam, true);
    CHECK(held.kind == Scheduler::TaskKind::Wait);
    CHECK(held.evicted == nullptr);
    CHECK(scheduler.ReadyCount() == 2);
    // The last record goes to a draw: nothing is left to prepare, so nothing is released.
    REQUIRE(scheduler.Request(KeyHash(3), Key(3)).kind == Scheduler::DemandKind::Claimed);
    const auto done = scheduler.Next(generation, PlentyOfRam);
    CHECK(done.kind == Scheduler::TaskKind::Done);
    CHECK(done.evicted == nullptr);
    CHECK(scheduler.ReadyCount() == 2);
}

TEST_CASE("A checkpoint is due after the interval, which grows with the file",
          "[video_core][pipeline_prepare]") {
    constexpr u64 Mib = u64{1} << 20;
    // Nothing uncommitted: never, whatever else holds.
    CHECK_FALSE(CheckpointDue(0, ~u64{0}, 0, true));
    // The minimum interval, to the nanosecond.
    CHECK_FALSE(CheckpointDue(1, CheckpointMinIntervalNs - 1, 0, false));
    CHECK(CheckpointDue(1, CheckpointMinIntervalNs, 0, false));
    // A small file does not lengthen it.
    CHECK(CheckpointDue(1, CheckpointMinIntervalNs, 10 * Mib, false));
    // A large file waits one second per MiB.
    const u64 interval = 100 * Mib * CheckpointNsPerByte;
    CHECK_FALSE(CheckpointDue(1, interval - 1, 100 * Mib, false));
    CHECK(CheckpointDue(1, interval, 100 * Mib, false));
    // The end of the first window commits at once.
    CHECK(CheckpointDue(1, 0, 100 * Mib, true));
    // A size that would overflow the scaling saturates instead of wrapping to "due".
    CHECK_FALSE(CheckpointDue(1, ~u64{0} - 1, ~u64{0}, false));
}

TEST_CASE("Records are found again in the committed generation by what they are",
          "[video_core][pipeline_prepare]") {
    using VideoCommon::CacheEnvelope::IndexEntry;
    const auto entry = [](RecordKind kind, u32 length, u64 checksum) {
        IndexEntry index;
        index.meta.kind = kind;
        index.length = length;
        index.checksum = checksum;
        return index;
    };
    const std::vector<IndexEntry> before{entry(RecordKind::Graphics, 10, 111),
                                         entry(RecordKind::Graphics, 20, 222),
                                         entry(RecordKind::Compute, 30, 333)};
    // Unchanged order.
    CHECK(RemapRecords(before, before) == std::vector<u32>{0, 1, 2});
    // Reordered, with a new record in front.
    const std::vector<IndexEntry> reordered{entry(RecordKind::Graphics, 99, 999), before[2],
                                            before[1], before[0]};
    CHECK(RemapRecords(before, reordered) == std::vector<u32>{3, 2, 1});
    // A record the new generation dropped has no place; the others keep theirs.
    const std::vector<IndexEntry> dropped{before[0], before[2]};
    CHECK(RemapRecords(before, dropped) == std::vector<u32>{0, NoRecord, 1});
    // Same checksum is not the same record: length and kind count too.
    const std::vector<IndexEntry> lookalikes{entry(RecordKind::Graphics, 11, 111),
                                             entry(RecordKind::Compute, 20, 222)};
    CHECK(RemapRecords(before, lookalikes) == std::vector<u32>{NoRecord, NoRecord, NoRecord});
    CHECK(RemapRecords({}, before).empty());
    CHECK(RemapRecords(before, {}) == std::vector<u32>{NoRecord, NoRecord, NoRecord});
}

namespace {

namespace CE = VideoCommon::CacheEnvelope;
namespace fs = std::filesystem;

/// A directory of its own under the temporary one, removed with the object.
struct ScratchDir {
    ScratchDir() {
        static std::atomic<u64> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("eglantine-pipeline-prepare-" + std::to_string(stamp) +
                                            "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

constexpr u64 RecordBytes = u64{1} << 20;
constexpr u32 LargeRecordCount = 24; // 24 MiB: its interval is longer than the minimum one

CE::CacheIdentity CacheIdentityOf(CE::PayloadKind kind) {
    CE::CacheIdentity identity;
    identity.kind = kind;
    identity.shader_format_version = 35;
    identity.host.build_id = 0x1111222233334444ULL;
    identity.host.title_id = 0x0100000000001000ULL;
    identity.host.game_version = 7;
    identity.host.content_stamp = 0xabcdef0123456789ULL;
    identity.host.settings_hash = 0x5555;
    identity.device.vendor_id = 0x10de;
    identity.device.device_id = 0x2206;
    identity.device.driver_id = 4;
    identity.device.driver_version = 0x00800123;
    for (u8 i = 0; i < 16; ++i) {
        identity.device.pipeline_cache_uuid[i] = static_cast<u8>(0xa0 + i);
    }
    return identity;
}

/// A committed generation of `count` records of RecordBytes each, as an earlier run left it.
void WriteGeneration(const fs::path& path, const CE::CacheIdentity& identity, u32 count) {
    auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxBytes(identity.kind));
    REQUIRE(writer);
    for (u32 i = 0; i < count; ++i) {
        const std::vector<u8> payload(RecordBytes, static_cast<u8>(i + 1));
        REQUIRE(writer->Append({}, payload) == CE::AppendResult::Ok);
    }
    REQUIRE(writer->Commit());
}

/// Opens one file through the function the pipeline cache opens its files with.
std::unique_ptr<CE::EnvelopeReader> OpenFile(CommittedSizes& sizes, const fs::path& path,
                                             const CE::CacheIdentity& expected) {
    CE::CacheLoadResult load;
    return OpenCommittedFile(sizes, path, expected, load);
}

/// What a file left by a build that is not this one looks like: rejected on opening.
void WriteForeignGeneration(const fs::path& path, CE::PayloadKind kind, u32 count) {
    CE::CacheIdentity foreign = CacheIdentityOf(kind);
    foreign.host.build_id += 1;
    WriteGeneration(path, foreign, count);
}

} // Anonymous namespace

TEST_CASE("The first checkpoint of a loaded large generation waits for the size of that generation",
          "[video_core][pipeline_prepare]") {
    const ScratchDir dir;
    const fs::path path = dir.path / "transferable.bin";
    const CE::CacheIdentity identity = CacheIdentityOf(CE::PayloadKind::Transferable);
    WriteGeneration(path, identity, LargeRecordCount);
    const u64 on_disk = fs::file_size(path);

    // This run accepts the file. Before anything is committed, the size it counts is the file's.
    CommittedSizes sizes;
    auto reader = OpenFile(sizes, path, identity);
    REQUIRE(reader);
    CHECK(sizes.FileBytes(0) == on_disk);
    CHECK(sizes.Total() == on_disk);
    const u64 interval = sizes.Total() * CheckpointNsPerByte;
    REQUIRE(interval > CheckpointMinIntervalNs);

    // The first new record of the run goes into the next generation, which starts as a copy.
    auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxBytes(identity.kind));
    REQUIRE(writer);
    REQUIRE(writer->Seed(*reader) == LargeRecordCount);
    const std::vector<u8> fresh(RecordBytes, static_cast<u8>(0xee));
    REQUIRE(writer->Append({}, fresh) == CE::AppendResult::Ok);

    // One uncommitted record: not due at the minimum interval, due when one second per MiB of
    // the accepted file has passed, and due at the end of the first window whatever the size.
    CHECK_FALSE(CheckpointDue(1, CheckpointMinIntervalNs, sizes.Total(), false));
    CHECK_FALSE(CheckpointDue(1, interval - 1, sizes.Total(), false));
    CHECK(CheckpointDue(1, interval, sizes.Total(), false));
    CHECK(CheckpointDue(1, 0, sizes.Total(), true));
}

TEST_CASE("Only an accepted file counts toward the committed size, main or mesh",
          "[video_core][pipeline_prepare]") {
    const ScratchDir dir;
    const fs::path main_path = dir.path / "transferable.bin";
    const fs::path mesh_path = dir.path / "transferable_mesh.bin";
    const CE::CacheIdentity main_identity = CacheIdentityOf(CE::PayloadKind::Transferable);
    const CE::CacheIdentity mesh_identity = CacheIdentityOf(CE::PayloadKind::TransferableMesh);

    // Accepted main, rejected mesh: only the main file counts.
    {
        WriteGeneration(main_path, main_identity, LargeRecordCount);
        WriteForeignGeneration(mesh_path, CE::PayloadKind::TransferableMesh, LargeRecordCount);
        CommittedSizes sizes;
        const auto main_reader = OpenFile(sizes, main_path, main_identity);
        const auto mesh_reader = OpenFile(sizes, mesh_path, mesh_identity);
        REQUIRE(main_reader);
        REQUIRE_FALSE(mesh_reader);
        CHECK(sizes.FileBytes(0) == fs::file_size(main_path));
        CHECK(sizes.FileBytes(1) == 0);
        CHECK(sizes.Total() == fs::file_size(main_path));
    }
    // Rejected main, accepted mesh: only the mesh file counts.
    {
        WriteForeignGeneration(main_path, CE::PayloadKind::Transferable, LargeRecordCount);
        WriteGeneration(mesh_path, mesh_identity, LargeRecordCount);
        CommittedSizes sizes;
        const auto main_reader = OpenFile(sizes, main_path, main_identity);
        const auto mesh_reader = OpenFile(sizes, mesh_path, mesh_identity);
        REQUIRE_FALSE(main_reader);
        REQUIRE(mesh_reader);
        CHECK(sizes.FileBytes(0) == 0);
        CHECK(sizes.FileBytes(1) == fs::file_size(mesh_path));
        CHECK(sizes.Total() == fs::file_size(mesh_path));
    }
}

TEST_CASE("A rejected or missing file does not delay the first checkpoint",
          "[video_core][pipeline_prepare]") {
    const ScratchDir dir;
    const fs::path rejected_path = dir.path / "transferable.bin";
    const fs::path missing_path = dir.path / "transferable_mesh.bin";
    WriteForeignGeneration(rejected_path, CE::PayloadKind::Transferable, LargeRecordCount);
    REQUIRE(fs::file_size(rejected_path) * CheckpointNsPerByte > CheckpointMinIntervalNs);
    // A generation that never committed left its temporary file next to the missing one.
    const fs::path stale = fs::path(missing_path).concat(".tmp");
    {
        std::ofstream(stale, std::ios::binary) << "x";
    }
    REQUIRE(fs::exists(stale));

    CommittedSizes sizes;
    const auto rejected = OpenFile(sizes, rejected_path,
                                   CacheIdentityOf(CE::PayloadKind::Transferable));
    const auto missing = OpenFile(sizes, missing_path,
                                  CacheIdentityOf(CE::PayloadKind::TransferableMesh));
    REQUIRE_FALSE(rejected);
    REQUIRE_FALSE(missing);
    CHECK(sizes.Total() == 0);
    CHECK_FALSE(fs::exists(stale));
    // The next generation starts empty, so the minimum interval is the whole wait.
    CHECK_FALSE(CheckpointDue(1, CheckpointMinIntervalNs - 1, sizes.Total(), false));
    CHECK(CheckpointDue(1, CheckpointMinIntervalNs, sizes.Total(), false));
}

TEST_CASE("A commit replaces the committed size of its file and the total is the sum of the files",
          "[video_core][pipeline_prepare]") {
    const ScratchDir dir;
    const fs::path main_path = dir.path / "transferable.bin";
    const fs::path mesh_path = dir.path / "transferable_mesh.bin";
    const CE::CacheIdentity main_identity = CacheIdentityOf(CE::PayloadKind::Transferable);
    const CE::CacheIdentity mesh_identity = CacheIdentityOf(CE::PayloadKind::TransferableMesh);
    WriteGeneration(main_path, main_identity, LargeRecordCount);
    WriteGeneration(mesh_path, mesh_identity, 4);
    const u64 first_main = fs::file_size(main_path);
    const u64 mesh = fs::file_size(mesh_path);

    CommittedSizes sizes;
    auto main_reader = OpenFile(sizes, main_path, main_identity);
    const auto mesh_reader = OpenFile(sizes, mesh_path, mesh_identity);
    REQUIRE(main_reader);
    REQUIRE(mesh_reader);
    REQUIRE(sizes.Total() == first_main + mesh);

    // The next generation of the main file holds the old records and one more.
    auto writer = CE::EnvelopeWriter::Create(main_path, main_identity,
                                             CE::MaxBytes(main_identity.kind));
    REQUIRE(writer);
    REQUIRE(writer->Seed(*main_reader) == LargeRecordCount);
    const std::vector<u8> fresh(RecordBytes, static_cast<u8>(0xee));
    REQUIRE(writer->Append({}, fresh) == CE::AppendResult::Ok);
    const u64 second_main = writer->Bytes();
    REQUIRE(second_main > first_main);
    main_reader.reset(); // an open file cannot be replaced on every host
    REQUIRE(CommitGeneration(sizes, 0, *writer));

    // The count of the file is the size of the new file, not of the two generations together,
    // the other file is untouched and the total is their sum.
    CHECK(sizes.FileBytes(0) == fs::file_size(main_path));
    CHECK(sizes.FileBytes(0) == second_main);
    CHECK(sizes.FileBytes(1) == mesh);
    CHECK(sizes.Total() == sizes.FileBytes(0) + sizes.FileBytes(1));
    CHECK(sizes.Total() == fs::file_size(main_path) + fs::file_size(mesh_path));
}

TEST_CASE("A commit that fails keeps the committed size and leaves no partial file",
          "[video_core][pipeline_prepare]") {
    const ScratchDir dir;
    const fs::path path = dir.path / "transferable.bin";
    const CE::CacheIdentity identity = CacheIdentityOf(CE::PayloadKind::Transferable);
    WriteGeneration(path, identity, LargeRecordCount);
    const u64 first_generation = fs::file_size(path);

    CommittedSizes sizes;
    auto reader = OpenFile(sizes, path, identity);
    REQUIRE(reader);
    REQUIRE(sizes.Total() == first_generation);

    auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxBytes(identity.kind));
    REQUIRE(writer);
    REQUIRE(writer->Seed(*reader) == LargeRecordCount);
    const std::vector<u8> fresh(RecordBytes, static_cast<u8>(0xee));
    REQUIRE(writer->Append({}, fresh) == CE::AppendResult::Ok);
    REQUIRE(writer->Bytes() > first_generation);
    reader.reset();

    // The place of the file is taken by a directory that is not empty, so the new generation
    // cannot replace it (the old file is moved aside to stay intact).
    const fs::path aside = dir.path / "previous.bin";
    fs::rename(path, aside);
    fs::create_directory(path);
    {
        std::ofstream(path / "occupied", std::ios::binary) << "x";
    }

    CHECK_FALSE(CommitGeneration(sizes, 0, *writer));
    CHECK(sizes.FileBytes(0) == first_generation);
    CHECK(sizes.Total() == first_generation);
    // Neither the temporary file of the failed generation nor a partial file is left behind.
    CHECK_FALSE(fs::exists(fs::path(path).concat(".tmp")));
    CHECK(fs::is_directory(path));
    CHECK(fs::file_size(aside) == first_generation);
    size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(dir.path)) {
        static_cast<void>(entry);
        ++entries;
    }
    CHECK(entries == 2); // the directory and the old file
}
