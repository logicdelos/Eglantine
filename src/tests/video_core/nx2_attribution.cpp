// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_spikes.h"

namespace VideoCommon::Nx2Spikes::Detail {
size_t PeakRetainedHistoryBytes();
size_t PeakResolverScratchBytes();
size_t ResolverScratchAllowance();
u64 UndrainedRingEvents();
u64 DrainedRingEvents();
bool HoldConsumer(bool hold);
void ForceHistoryRelief(bool on);
} // namespace VideoCommon::Nx2Spikes::Detail

namespace {

using namespace VideoCommon::Nx2Spikes;
using Times = std::array<u64, CauseCount>;

constexpr u64 Ms = 1'000'000;

std::vector<LongFrame> g_frames;
std::thread::id g_sink_thread;

bool CollectSink(void*, const LongFrame& frame) {
    g_sink_thread = std::this_thread::get_id();
    g_frames.push_back(frame);
    return true;
}

bool FailingSink(void*, const LongFrame&) {
    return false;
}

u64 At(const Times& times, Cause cause) {
    return times[static_cast<size_t>(cause)];
}

u64 Total(const Times& times) {
    u64 total{};
    for (const u64 value : times) {
        total += value;
    }
    return total;
}

/// The strict decomposition most tests state: time the presenting thread has no span for is
/// Unknown. The tests of the gap rule use a SessionConfig of their own (charging on, the default).
SessionConfig Strict() {
    SessionConfig config;
    config.charge_presenter_gaps = false;
    return config;
}

SessionConfig Config(u64 threshold_ns) {
    SessionConfig config = Strict();
    config.threshold_ns = threshold_ns;
    return config;
}

void WaitUntil(u64 timestamp_ns) {
    while (NowNs() < timestamp_ns) {
        std::this_thread::yield();
    }
}

template <typename F>
void OnThread(F&& function) {
    std::thread thread{std::forward<F>(function)};
    thread.join();
}

/// A session with the collecting sink. `base` is a timestamp taken after the session began, so
/// the synthetic spans and presents of a test (base + offsets) are never before the session.
/// Spans of the root (the test thread) are numbered from `root_seq`: the constructor spends the
/// first sequence number on a probe span that ends before `base`.
struct Run {
    explicit Run(SessionConfig config = Strict()) {
        g_frames.clear();
        SetLongFrameSink({CollectSink, nullptr});
        REQUIRE(BeginSession(config));
        const SpanToken probe = BeginSpan(Cause::Unknown);
        EndSpan(probe);
        generation = probe.session_generation;
        root = probe.thread_registration_id;
        root_seq = probe.sequence + 1;
        base = NowNs();
    }
    ~Run() {
        EndSession();
        SetLongFrameSink({});
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    [[nodiscard]] u64 T(u64 offset_ns) const {
        return base + offset_ns;
    }
    /// The token of the next span the test thread records with RecordSpan.
    SpanToken NextRootToken() {
        return {generation, root, root_seq++};
    }
    /// The token of the first span a fresh thread records.
    [[nodiscard]] SpanToken FirstSpanOf(u32 thread) const {
        return {generation, thread, 1};
    }
    AttributionSnapshot Finish() {
        EndSession();
        return ReadAttributionSnapshot();
    }

    u32 generation{};
    u32 root{};
    u64 root_seq{};
    u64 base{};
};

u64 Present(u64 timestamp_ns, PresentResult result = PresentResult::Success, bool real = true) {
    return OnHostPresent({0, 0, timestamp_ns, real, result});
}

/// Returns true once the consumer has taken the `recorded` events this thread has recorded, so
/// the next batch cannot overflow its ring.
bool AwaitDrained(u64 recorded) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (Detail::DrainedRingEvents() < recorded && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return Detail::DrainedRingEvents() >= recorded;
}

/// Returns true once the consumer has taken everything this thread has recorded so far.
bool AwaitIdle() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (Detail::UndrainedRingEvents() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return Detail::UndrainedRingEvents() == 0;
}

} // Anonymous namespace

TEST_CASE("Present results classify and filter", "[nx2][attribution]") {
    CHECK(ClassifyPresent(0) == PresentResult::Success);
    CHECK(ClassifyPresent(1000001003) == PresentResult::Suboptimal);
    CHECK(ClassifyPresent(-1000001004) == PresentResult::OutOfDate);
    CHECK(ClassifyPresent(-4) == PresentResult::Error); // VK_ERROR_DEVICE_LOST
    CHECK(ClassifyPresent(1) == PresentResult::Error);
    CHECK(QualifiesAsHostPresent(PresentResult::Success, true));
    CHECK(QualifiesAsHostPresent(PresentResult::Suboptimal, true));
    CHECK_FALSE(QualifiesAsHostPresent(PresentResult::Success, false));
    CHECK_FALSE(QualifiesAsHostPresent(PresentResult::Suboptimal, false));
    CHECK_FALSE(QualifiesAsHostPresent(PresentResult::OutOfDate, true));
    CHECK_FALSE(QualifiesAsHostPresent(PresentResult::Error, true));

    Run run;
    CHECK(Present(run.T(0)) == 1);
    CHECK(Present(run.T(50 * Ms), PresentResult::Success, false) == 0); // generated frame
    CHECK(Present(run.T(60 * Ms), PresentResult::OutOfDate) == 0);
    CHECK(Present(run.T(70 * Ms), PresentResult::Error) == 0);
    // The rejected attempts did not end the interval: 200 ms since the first present.
    CHECK(Present(run.T(200 * Ms), PresentResult::Suboptimal) == 2);
    const AttributionSnapshot snap = run.Finish();
    CHECK(snap.rejected_presents == 3);
    CHECK(snap.frames == 1);
    REQUIRE(g_frames.size() == 1);
    CHECK(g_frames[0].id == 2);
    CHECK(g_frames[0].duration_ns == 200 * Ms);
    CHECK(g_frames[0].present_result == PresentResult::Suboptimal);
}

TEST_CASE("A frame of exactly the threshold is not long, one nanosecond more is",
          "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    Present(run.T(100 * Ms));              // exactly 100 ms
    Present(run.T(200 * Ms + 1));          // 100 ms + 1 ns
    const AttributionSnapshot snap = run.Finish();
    CHECK(snap.frames == 2);
    CHECK(snap.long_frames == 1);
    REQUIRE(g_frames.size() == 1);
    CHECK(g_frames[0].id == 3);
    CHECK(g_frames[0].duration_ns == 100 * Ms + 1);
    CHECK(snap.first_120s.frames_over_threshold == 1);
    CHECK(snap.first_120s.max_ns == 100 * Ms + 1);
}

TEST_CASE("Nothing covering a frame is reported as unknown", "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    Present(run.T(250 * Ms));
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    CHECK(At(frame.cause_ns, Cause::Unknown) == 250 * Ms);
    CHECK(Total(frame.cause_ns) == frame.duration_ns);
    CHECK(frame.primary_cause == Cause::Unknown);
    CHECK(frame.confidence_permille == 0);
    CHECK((frame.unknown_reasons & UnknownReason::UnlinkedGuest) != 0);
    CHECK(frame.instrumentation_complete);
    CHECK(snap.unknown_wall_fraction == 1.0);
    CHECK(snap.unknown_primary_frame_fraction == 1.0);
    CHECK(snap.complete);
    const std::string line = LongFrameLine(frame);
    CHECK(line.find("primary=unknown") != std::string::npos);
    CHECK(line.find("unknown_ms=250.0") != std::string::npos);
}

TEST_CASE("Nested spans subtract from their parent", "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    // zstd decompression 0-100 ms around a file read 20-50 ms (recorded inner first, as a scope
    // that ends first does).
    RecordSpan(Cause::Disk, run.T(20 * Ms), run.T(50 * Ms));
    RecordSpan(Cause::Decompression, run.T(0), run.T(100 * Ms));
    Present(run.T(150 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const Times& cause = g_frames[0].cause_ns;
    CHECK(At(cause, Cause::Disk) == 30 * Ms);
    CHECK(At(cause, Cause::Decompression) == 70 * Ms);
    CHECK(At(cause, Cause::Unknown) == 50 * Ms);
    CHECK(Total(cause) == 150 * Ms);
}

TEST_CASE("Overlapping spans on one thread are cut where the later one begins",
          "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    RecordSpan(Cause::Shader, run.T(0), run.T(60 * Ms));
    RecordSpan(Cause::Texture, run.T(40 * Ms), run.T(120 * Ms));
    Present(run.T(120 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const Times& cause = g_frames[0].cause_ns;
    CHECK(At(cause, Cause::Shader) == 40 * Ms);
    CHECK(At(cause, Cause::Texture) == 80 * Ms);
    CHECK(At(cause, Cause::Unknown) == 0);
    // The activity of the thread is the same exclusive sum.
    CHECK(At(g_frames[0].activity_ns, Cause::Shader) == 40 * Ms);
}

TEST_CASE("Spans are clipped at the frame boundaries", "[nx2][attribution]") {
    Run run{Config(1)};
    Present(run.T(0));
    // Starts 50 ms before the first frame and ends 50 ms into the second.
    RecordSpan(Cause::Shader, run.T(0) - 50 * Ms, run.T(250 * Ms));
    Present(run.T(200 * Ms));
    Present(run.T(300 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 2);
    CHECK(g_frames[0].duration_ns == 200 * Ms);
    CHECK(At(g_frames[0].cause_ns, Cause::Shader) == 200 * Ms);
    CHECK(At(g_frames[0].cause_ns, Cause::Unknown) == 0);
    CHECK(At(g_frames[1].cause_ns, Cause::Shader) == 50 * Ms);
    CHECK(At(g_frames[1].cause_ns, Cause::Unknown) == 50 * Ms);
}

TEST_CASE("The part of a span past a frame end is charged to the next frame",
          "[nx2][attribution]") {
    Run run{Config(1)};
    Present(run.T(0));
    RecordSpan(Cause::Pipeline, run.T(10 * Ms), run.T(70 * Ms));
    Present(run.T(40 * Ms));
    Present(run.T(100 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 2);
    CHECK(At(g_frames[0].cause_ns, Cause::Pipeline) == 30 * Ms);
    CHECK(At(g_frames[0].cause_ns, Cause::Unknown) == 10 * Ms);
    CHECK(At(g_frames[1].cause_ns, Cause::Pipeline) == 30 * Ms);
    CHECK(At(g_frames[1].cause_ns, Cause::Unknown) == 30 * Ms);
}

TEST_CASE("A span open at a frame end is clipped there and finishes in the next frame",
          "[nx2][attribution]") {
    Run run{Config(1)};
    Present(NowNs());
    const SpanToken span = BeginSpan(Cause::Shader);
    WaitUntil(NowNs() + 10 * Ms);
    Present(NowNs()); // the span is still open
    WaitUntil(NowNs() + 5 * Ms);
    EndSpan(span);
    Present(NowNs());
    run.Finish();
    REQUIRE(g_frames.size() == 2);
    // Open at the end of the first frame is not an incomplete record.
    CHECK(g_frames[0].instrumentation_complete);
    CHECK(At(g_frames[0].cause_ns, Cause::Shader) >= 10 * Ms);
    CHECK(Total(g_frames[0].cause_ns) == g_frames[0].duration_ns);
    CHECK(g_frames[1].instrumentation_complete);
    CHECK(At(g_frames[1].cause_ns, Cause::Shader) >= 5 * Ms);
    CHECK(Total(g_frames[1].cause_ns) == g_frames[1].duration_ns);
}

TEST_CASE("The plan example: a linked shader build, unrelated disk work, a wait and unknown",
          "[nx2][attribution]") {
    Run run;
    const WorkId build = NewWorkId();
    const WorkId read = NewWorkId();
    Present(run.T(0));

    u32 worker_id{};
    OnThread([&] {
        worker_id = RegisterThread("shader worker");
        RecordSpan(Cause::Shader, run.T(0), run.T(80 * Ms), build);
    });
    OnThread([&] {
        RegisterThread("disk worker");
        RecordSpan(Cause::Disk, run.T(10 * Ms), run.T(80 * Ms), read);
    });
    // The present thread waited 100 ms for the build; the build ran 80 ms of it.
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms));
    LinkWait(wait, build, DependencyKind::Job);
    Present(run.T(150 * Ms));
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(worker_id != 0);
    REQUIRE(g_frames.size() == 1);
    CHECK(g_sink_thread != std::this_thread::get_id());
    const LongFrame& frame = g_frames[0];
    CHECK(frame.duration_ns == 150 * Ms);
    CHECK(At(frame.cause_ns, Cause::Shader) == 80 * Ms);
    CHECK(At(frame.cause_ns, Cause::Waiting) == 20 * Ms);
    CHECK(At(frame.cause_ns, Cause::Unknown) == 50 * Ms);
    CHECK(At(frame.cause_ns, Cause::Disk) == 0); // on no critical path
    CHECK(Total(frame.cause_ns) == 150 * Ms);
    CHECK(At(frame.activity_ns, Cause::Disk) == 70 * Ms);
    CHECK(At(frame.activity_ns, Cause::Shader) == 80 * Ms);
    CHECK(frame.primary_cause == Cause::Shader);
    CHECK(frame.confidence_permille == 666); // (150 - 50) / 150
    CHECK(frame.instrumentation_complete);
    REQUIRE(frame.critical_work_count == 1);
    CHECK(frame.critical_work_ids[0] == build);
    CHECK(snap.producer_unknown_wait_ns == 0);
}

TEST_CASE("A wait chain is followed through the producer's own wait", "[nx2][attribution]") {
    Run run;
    const WorkId outer = NewWorkId();
    const WorkId inner = NewWorkId();
    Present(run.T(0));
    // The root waits for `outer`; the thread running it waits for `inner`, which runs 20-80 ms.
    OnThread([&] {
        const u32 id = RegisterThread("outer");
        RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms), outer);
        LinkWait(run.FirstSpanOf(id), inner, DependencyKind::Job);
    });
    OnThread([&] {
        RegisterThread("inner");
        RecordSpan(Cause::Disk, run.T(20 * Ms), run.T(80 * Ms), inner);
    });
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms));
    LinkWait(wait, outer, DependencyKind::Join);
    Present(run.T(150 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const Times& cause = g_frames[0].cause_ns;
    CHECK(At(cause, Cause::Disk) == 60 * Ms);
    CHECK(At(cause, Cause::Waiting) == 40 * Ms);
    CHECK(At(cause, Cause::Unknown) == 50 * Ms);
    CHECK(Total(cause) == 150 * Ms);
    CHECK((g_frames[0].unknown_reasons & UnknownReason::AmbiguousDependency) == 0);
}

TEST_CASE("A wait on a task's work is charged to the spans nested in it that carry other work",
          "[nx2][attribution]") {
    Run run;
    const WorkId task = NewWorkId();
    const WorkId driver = NewWorkId();
    Present(run.T(0));
    // A synchronous build: the task span runs 10-90 ms and the driver call inside it, which has
    // its own work id, runs 30-70 ms. The root waits for the task for 0-100 ms.
    OnThread([&] {
        RegisterThread("pipeline worker");
        RecordSpan(Cause::Shader, run.T(10 * Ms), run.T(90 * Ms), task);
        RecordSpan(Cause::Pipeline, run.T(30 * Ms), run.T(70 * Ms), driver);
    });
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms));
    LinkWait(wait, task, DependencyKind::Job);
    Present(run.T(150 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    CHECK(At(frame.cause_ns, Cause::Shader) == 40 * Ms);
    CHECK(At(frame.cause_ns, Cause::Pipeline) == 40 * Ms);
    CHECK(At(frame.cause_ns, Cause::Waiting) == 20 * Ms);
    CHECK(At(frame.cause_ns, Cause::Unknown) == 50 * Ms);
    CHECK(Total(frame.cause_ns) == 150 * Ms);
    // Activity counts every span once, whatever work ids enclose it.
    CHECK(At(frame.activity_ns, Cause::Shader) == 40 * Ms);
    CHECK(At(frame.activity_ns, Cause::Pipeline) == 40 * Ms);
    CHECK(At(frame.activity_ns, Cause::Waiting) == 100 * Ms);
}

TEST_CASE("A wait on a worker's work reaches spans nested two levels down, past a span without work",
          "[nx2][attribution]") {
    Run run;
    const WorkId worker = NewWorkId();
    const WorkId driver = NewWorkId();
    Present(run.T(0));
    // The present thread waits for the worker 0-100 ms. The worker's span covers all of it, a span
    // without work id runs 20-90 ms inside it and a driver call with its own work id 40-60 ms
    // inside that.
    OnThread([&] {
        RegisterThread("present worker");
        RecordSpan(Cause::Texture, run.T(0), run.T(100 * Ms), worker);
        RecordSpan(Cause::Disk, run.T(20 * Ms), run.T(90 * Ms));
        RecordSpan(Cause::Pipeline, run.T(40 * Ms), run.T(60 * Ms), driver);
    });
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms));
    LinkWait(wait, worker, DependencyKind::Join);
    Present(run.T(150 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const Times& cause = g_frames[0].cause_ns;
    CHECK(At(cause, Cause::Texture) == 30 * Ms);
    CHECK(At(cause, Cause::Disk) == 50 * Ms);
    CHECK(At(cause, Cause::Pipeline) == 20 * Ms);
    CHECK(At(cause, Cause::Waiting) == 0);
    CHECK(At(cause, Cause::Unknown) == 50 * Ms);
    CHECK(Total(cause) == 150 * Ms);
}

TEST_CASE("A wait on the nested work itself still sees only that work", "[nx2][attribution]") {
    Run run;
    const WorkId task = NewWorkId();
    const WorkId driver = NewWorkId();
    Present(run.T(0));
    OnThread([&] {
        RegisterThread("pipeline worker");
        RecordSpan(Cause::Shader, run.T(10 * Ms), run.T(90 * Ms), task);
        RecordSpan(Cause::Pipeline, run.T(30 * Ms), run.T(70 * Ms), driver);
    });
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms));
    LinkWait(wait, driver, DependencyKind::Job);
    Present(run.T(150 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const Times& cause = g_frames[0].cause_ns;
    CHECK(At(cause, Cause::Pipeline) == 40 * Ms);
    CHECK(At(cause, Cause::Shader) == 0);
    CHECK(At(cause, Cause::Waiting) == 60 * Ms);
    CHECK(Total(cause) == 150 * Ms);
}

TEST_CASE("A stretch under a span without work is not charged to work that ended before it",
          "[nx2][attribution]") {
    Run run;
    const WorkId first = NewWorkId();
    const WorkId second = NewWorkId();
    Present(run.T(0));
    // All on the present thread: a Disk span with work `first` runs 0-50 ms, a Shader span without
    // work 10-100 ms (it starts inside the first one and outlives it), a Shader span with work
    // `second` 100-160 ms. Nothing runs 160-200 ms.
    RecordSpan(Cause::Disk, run.T(0), run.T(50 * Ms), first);
    RecordSpan(Cause::Shader, run.T(10 * Ms), run.T(100 * Ms));
    RecordSpan(Cause::Shader, run.T(100 * Ms), run.T(160 * Ms), second);
    Present(run.T(200 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    CHECK(At(frame.cause_ns, Cause::Disk) == 10 * Ms);
    CHECK(At(frame.cause_ns, Cause::Shader) == 150 * Ms);
    CHECK(At(frame.cause_ns, Cause::Unknown) == 40 * Ms);
    CHECK(Total(frame.cause_ns) == 200 * Ms);
    // `first` is charged 0-10 ms (Disk) and 10-50 ms (Shader, inside it): 50 ms. `second` is
    // charged 60 ms, so it ranks first. A stretch merged across 50 ms into the earlier Shader
    // piece would give `first` 100 ms and the other order.
    REQUIRE(frame.critical_work_count == 2);
    CHECK(frame.critical_work_ids[0] == second);
    CHECK(frame.critical_work_ids[1] == first);
}

TEST_CASE("A circular wait terminates and is marked ambiguous", "[nx2][attribution]") {
    Run run;
    const WorkId a = NewWorkId();
    const WorkId b = NewWorkId();
    Present(run.T(0));
    OnThread([&] {
        const u32 id = RegisterThread("a");
        RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms), a);
        LinkWait(run.FirstSpanOf(id), b, DependencyKind::Job);
    });
    OnThread([&] {
        const u32 id = RegisterThread("b");
        RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms), b);
        LinkWait(run.FirstSpanOf(id), a, DependencyKind::Job);
    });
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms));
    LinkWait(wait, a, DependencyKind::Job);
    Present(run.T(150 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    CHECK(At(g_frames[0].cause_ns, Cause::Waiting) == 100 * Ms);
    CHECK(At(g_frames[0].cause_ns, Cause::Unknown) == 50 * Ms);
    CHECK((g_frames[0].unknown_reasons & UnknownReason::AmbiguousDependency) != 0);
}

TEST_CASE("A wait nobody linked stays waiting and is counted as producer unknown",
          "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    RecordSpan(Cause::Waiting, run.T(10 * Ms), run.T(90 * Ms));
    Present(run.T(120 * Ms));
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(g_frames.size() == 1);
    CHECK(At(g_frames[0].cause_ns, Cause::Waiting) == 80 * Ms);
    CHECK(At(g_frames[0].cause_ns, Cause::Unknown) == 40 * Ms);
    CHECK((g_frames[0].unknown_reasons & UnknownReason::ProducerUnknownWait) != 0);
    CHECK(snap.producer_unknown_wait_ns == 80 * Ms);
}

TEST_CASE("The largest cause wins, a tie is unknown", "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    RecordSpan(Cause::Disk, run.T(0), run.T(75 * Ms));
    RecordSpan(Cause::Shader, run.T(75 * Ms), run.T(150 * Ms));
    Present(run.T(150 * Ms));
    RecordSpan(Cause::Disk, run.T(150 * Ms), run.T(250 * Ms));
    RecordSpan(Cause::Shader, run.T(250 * Ms), run.T(260 * Ms));
    Present(run.T(300 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 2);
    CHECK(g_frames[0].primary_cause == Cause::Unknown); // 75 ms each
    CHECK(g_frames[0].confidence_permille == 1000);
    CHECK(g_frames[1].primary_cause == Cause::Disk); // 100 ms against 10 and 40 unknown
    CHECK(At(g_frames[1].cause_ns, Cause::Unknown) == 40 * Ms);
}

TEST_CASE("Counter deltas are the events counted during the frame", "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    Note(Event::FileRead, {}, 10);
    Note(Event::FileRead, {}, 10);
    Note(Event::FileRead, {}, 10);
    Present(run.T(150 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    CHECK(g_frames[0].counter_deltas[static_cast<size_t>(Event::FileRead)] == 3);
}

TEST_CASE("Threads register once with distinct ids and the table is bounded", "[nx2][attribution]") {
    CHECK(RegisterThread("no session") == 0);
    Run run;
    const u32 first = RegisterThread("test");
    CHECK(first != 0);
    CHECK(first == run.root);
    CHECK(RegisterThread("again") == first);

    // A thread that records without registering is registered as unnamed.
    u32 unnamed{};
    u32 unnamed_again{};
    OnThread([&] {
        const SpanToken token = BeginSpan(Cause::Disk);
        unnamed = token.thread_registration_id;
        EndSpan(token);
        unnamed_again = RegisterThread("late");
    });
    CHECK(unnamed != 0);
    CHECK(unnamed != first);
    CHECK(unnamed_again == unnamed);

    constexpr size_t Count = 40;
    std::array<u32, Count> ids{};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < Count; ++i) {
        threads.emplace_back([&ids, &run, i] {
            ids[i] = RegisterThread("worker");
            RecordSpan(Cause::Disk, run.T(0), run.T(Ms));
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    size_t refused = 0;
    std::vector<u32> seen{first, unnamed};
    for (const u32 id : ids) {
        if (id == 0) {
            ++refused;
            continue;
        }
        for (const u32 other : seen) {
            CHECK(id != other);
        }
        seen.push_back(id);
    }
    CHECK(refused > 0);
    CHECK(refused < Count);
    // Every span of a thread without a ring is a counted loss, not a silent one.
    const AttributionSnapshot snap = run.Finish();
    CHECK(snap.lost_events == refused);
    CHECK_FALSE(snap.complete);
}

TEST_CASE("A full buffer marks the frame incomplete instead of dropping silently",
          "[nx2][attribution]") {
    // 32 threads * 4 records * 64 bytes: a ring of four records per thread.
    SessionConfig config = Config(100 * Ms);
    config.buffer_bytes = 32 * 4 * 64;
    Run run{config};
    Present(run.T(0));
    for (size_t batch = 0; batch < 100000 && ReadAttributionSnapshot().lost_events == 0; ++batch) {
        for (int i = 0; i < 64; ++i) {
            RecordSpan(Cause::Shader, run.T(10 * Ms), run.T(20 * Ms));
        }
    }
    Present(run.T(200 * Ms));
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(g_frames.size() == 1);
    CHECK(snap.lost_events > 0);
    CHECK_FALSE(snap.complete);
    CHECK_FALSE(g_frames[0].instrumentation_complete);
    CHECK((g_frames[0].unknown_reasons & UnknownReason::LostSpans) != 0);
    CHECK(Total(g_frames[0].cause_ns) == g_frames[0].duration_ns);
}

TEST_CASE("A new session starts empty and ignores tokens of the old one", "[nx2][attribution]") {
    SpanToken stale;
    {
        Run run;
        stale = BeginSpan(Cause::Shader);
        Present(run.T(0));
        Present(run.T(200 * Ms));
        CHECK_FALSE(BeginSession({})); // one at a time
        run.Finish();
        CHECK(g_frames.size() == 1);
    }
    REQUIRE(stale.Valid());
    Run run;
    const AttributionSnapshot fresh = ReadAttributionSnapshot();
    CHECK(fresh.enabled);
    CHECK(fresh.frames == 0);
    CHECK(fresh.long_frames == 0);
    CHECK(fresh.lost_events == 0);
    CHECK(fresh.first_host_present_ns == 0);
    // Same thread, same sequence number, other generation: it must not close this session's span.
    const SpanToken current = BeginSpan(Cause::Shader);
    CHECK(current.session_generation != stale.session_generation);
    EndSpan(stale);
    LinkWait(stale, NewWorkId(), DependencyKind::Job);
    CHECK(Present(run.T(0)) == 1); // ids restart
    Present(run.T(200 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    CHECK(g_frames[0].id == 2);
    CHECK(At(g_frames[0].cause_ns, Cause::Shader) > 150 * Ms); // open all along, not closed early
}

namespace {

struct BlockingSink {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    int calls{};
};
BlockingSink g_block;

bool BlockFirstFrame(void*, const LongFrame& frame) {
    g_frames.push_back(frame);
    if (g_block.calls++ == 0) {
        g_block.entered.store(true);
        while (!g_block.release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    return true;
}

void ResetBlock() {
    g_frames.clear();
    g_block.entered = false;
    g_block.release = false;
    g_block.calls = 0;
}

/// Returns once the consumer is inside the sink of the first long frame (so everything the test
/// does next is queued or published while it cannot drain).
void AwaitBlocked() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!g_block.entered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(g_block.entered.load());
}

/// Returns once the consumer has resolved `frames` intervals.
void AwaitFrames(u64 frames) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (ReadCrashStatus().frames < frames && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(ReadCrashStatus().frames >= frames);
}

} // Anonymous namespace

TEST_CASE("Ending a session with open spans marks the frames they touch incomplete",
          "[nx2][attribution]") {
    ResetBlock();
    SetLongFrameSink({BlockFirstFrame, nullptr});
    REQUIRE(BeginSession({}));
    const u64 base = NowNs();
    Present(base);
    Present(base + 150 * Ms); // the consumer stops in the sink with this frame
    AwaitBlocked();
    const SpanToken open = BeginSpan(Cause::Shader); // never ended
    REQUIRE(open.Valid());
    Present(base + 300 * Ms); // queued behind the blocked one
    std::thread releaser{[] {
        // EndSession has begun (recording is off, the stop follows at once); the sink polls every
        // millisecond, so the consumer sees the stop before it takes the queued present.
        while (AttributionEnabled()) {
            std::this_thread::yield();
        }
        g_block.release.store(true);
    }};
    EndSession();
    releaser.join();
    SetLongFrameSink({});
    EndSpan(open); // after the end of the session: ignored
    const AttributionSnapshot snap = ReadAttributionSnapshot();
    CHECK(snap.enabled); // a session was recorded, and its snapshot says so after it ended
    CHECK_FALSE(AttributionEnabled());
    REQUIRE(g_frames.size() == 2);
    // The span began (in real time) before the second frame started: open all through it.
    CHECK(At(g_frames[1].cause_ns, Cause::Shader) == 150 * Ms);
    CHECK_FALSE(g_frames[1].instrumentation_complete);
    CHECK_FALSE(snap.complete);
}

TEST_CASE("A cross-thread end drained before its begin is kept until the begin arrives",
          "[nx2][attribution]") {
    ResetBlock();
    SetLongFrameSink({BlockFirstFrame, nullptr});
    REQUIRE(BeginSession(Config(0)));
    const u32 ender = RegisterThread("ender"); // registered before the owner: drained before it
    const u64 base = NowNs();
    Present(base);
    Present(base + 1); // long for a zero threshold: the consumer blocks in the sink
    AwaitBlocked();
    SpanToken token;
    OnThread([&token] {
        RegisterThread("owner");
        token = BeginSpan(Cause::Shader);
    });
    REQUIRE(token.Valid());
    REQUIRE(token.thread_registration_id > ender);
    EndSpan(token); // the End goes into the ender's ring, the Begin sits in a higher one
    Present(base + 10'000 * Ms);
    g_block.release.store(true);
    EndSession();
    SetLongFrameSink({});
    const AttributionSnapshot snap = ReadAttributionSnapshot();
    REQUIRE(g_frames.size() == 2);
    // Closed after a few microseconds, not open for the 10 s of the frame.
    CHECK(At(g_frames[1].cause_ns, Cause::Shader) < 1'000 * Ms);
    CHECK(g_frames[1].instrumentation_complete);
    CHECK(snap.lost_events == 0);
}

TEST_CASE("A cross-thread end whose begin never arrives is counted as lost at the end",
          "[nx2][attribution]") {
    Run run;
    SpanToken stale{run.generation, run.root + 5, 77}; // no such registration or span
    EndSpan(stale);
    const AttributionSnapshot snap = run.Finish();
    CHECK(snap.lost_events == 1);
    CHECK_FALSE(snap.complete);
}

TEST_CASE("Dropped present boundaries never merge into an invented long frame",
          "[nx2][attribution]") {
    constexpr u64 QueueItems = 4096; // the recorder's bound on queued boundaries
    constexpr u64 Dropped = 904;
    ResetBlock();
    SetLongFrameSink({BlockFirstFrame, nullptr});
    REQUIRE(BeginSession(Config(50 * Ms)));
    const u64 base = NowNs();
    Present(base);
    Present(base + 60 * Ms); // long: the consumer blocks in the sink, the queue is empty
    AwaitBlocked();
    u64 now = base + 60 * Ms;
    for (u64 i = 0; i < QueueItems + Dropped; ++i) {
        now += 10 * Ms; // every interval is far below the threshold
        REQUIRE(Present(now) != 0);
    }
    g_block.release.store(true);
    AwaitFrames(1 + QueueItems);
    now += 10 * Ms;
    Present(now); // its predecessor was dropped: the interval is still 10 ms
    EndSession();
    SetLongFrameSink({});
    const AttributionSnapshot snap = ReadAttributionSnapshot();
    CHECK(snap.frames == 1 + QueueItems + 1);
    CHECK(snap.long_frames == 1);
    REQUIRE(g_frames.size() == 1);
    CHECK(g_frames[0].duration_ns == 60 * Ms);
    CHECK(snap.first_120s.frames_over_threshold == 1);
    CHECK(snap.first_120s.max_ns == 60 * Ms);
    CHECK(snap.lost_events == Dropped); // the lost boundaries are counted, never silent
    CHECK(snap.dropped_boundaries == Dropped); // and reported on their own for the report
    CHECK_FALSE(snap.complete);
}

TEST_CASE("A marker is never queued before the first present", "[nx2][attribution]") {
    // The interleaving is a few instructions wide, so many rounds; the barrier releases both
    // threads together and the marker thread acts the moment the first present is visible.
    for (int round = 0; round < 200; ++round) {
        REQUIRE(BeginSession({}));
        std::atomic<bool> go{false};
        MarkerResult result = MarkerResult::NoSession;
        std::thread marker_thread{[&go, &result] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            while (ReadCrashStatus().first_host_present_ns == 0) {
            }
            InteractiveMarker marker;
            marker.scene_id = "scene";
            marker.origin = "lab";
            marker.marker_ns = NowNs();
            result = MarkInteractive(marker);
        }};
        go.store(true);
        Present(NowNs());
        marker_thread.join();
        EndSession();
        const AttributionSnapshot snap = ReadAttributionSnapshot();
        REQUIRE(result == MarkerResult::Accepted);
        REQUIRE(snap.interactive_status == IntervalStatus::Complete);
        REQUIRE(snap.first_present_to_interactive.status == IntervalStatus::Complete);
    }
}

TEST_CASE("Rings and retained history share one fixed budget", "[nx2][attribution]") {
    SessionConfig config = Config(100 * Ms);
    config.buffer_bytes = 64 * 1024;
    Run run{config};
    constexpr u64 Presents = 100;
    constexpr u64 PerPresent = 4;
    for (u64 k = 1; k <= Presents; ++k) {
        // Spans that end after every present, so none is pruned or late; the consumer has drained
        // the rings before the next batch.
        for (u64 i = 0; i < PerPresent; ++i) {
            RecordSpan(Cause::Shader, run.T(1'000 * Ms), run.T(100'000 * Ms));
        }
        Present(run.T(k * Ms));
        AwaitFrames(k - 1);
    }
    const AttributionSnapshot snap = run.Finish();
    // Half of 64 KiB for the history leaves room for far fewer than 400 spans; the rest is lost
    // and counted, not silently kept beyond the budget.
    CHECK(snap.lost_events > 0);
    CHECK(snap.lost_events < Presents * PerPresent);
    CHECK_FALSE(snap.complete);
}

TEST_CASE("Span storage held by many producers over pruning windows stays inside the budget",
          "[nx2][attribution]") {
    SessionConfig config = Config(100 * Ms);
    config.buffer_bytes = 1024 * 1024;
    constexpr u64 Producers = 16;
    constexpr u64 SpansPerBurst = 600;
    constexpr u64 RingChunk = 64; // a quarter of a ring (256 events), recorded between two drains
    // Pruning only ever removes spans more than two seconds old on the recorder clock.
    WaitUntil(5'000 * Ms);
    Run run{config};
    // Spans and presents far enough in the past that each present prunes the burst before it.
    const u64 origin = run.base - 4'000 * Ms;
    Present(origin);
    std::atomic<bool> drained{true};
    for (u64 round = 0; round < Producers; ++round) {
        const u64 begin = origin + (round + 1) * 20 * Ms;
        // One thread per burst: every producer keeps its own span vectors for the whole session.
        OnThread([&] {
            RegisterThread("burst producer");
            u64 recorded = 0;
            for (u64 done = 0; done < SpansPerBurst; done += RingChunk) {
                for (u64 i = 0; i < RingChunk; ++i) {
                    RecordSpan(Cause::Shader, begin, begin + 5 * Ms);
                }
                recorded += RingChunk;
                if (!AwaitDrained(recorded)) {
                    drained = false;
                    return;
                }
            }
        });
        REQUIRE(drained.load());
        Present(begin + 15 * Ms);
        AwaitFrames(round + 1);
    }
    const AttributionSnapshot snap = run.Finish();
    // Every burst was recorded in full, so the storage was exercised and nothing was refused...
    CHECK(snap.lost_events == 0);
    const size_t peak = Detail::PeakRetainedHistoryBytes();
    CHECK(peak >= SpansPerBurst * 40);
    // ...and the storage of all producers together never exceeds the share of the budget that
    // is set aside for the history, although every producer filled its own vectors in turn.
    CHECK(peak <= config.buffer_bytes / 2);
}

namespace {

struct CloseOutcome {
    LongFrame frame;
    u64 lost_events{};
};

enum class CloseCase {
    Room,         ///< The span ends while the history has room
    FullEnded,    ///< It ends after the history was filled
    FullNotEnded, ///< The history is filled the same way and it does not end
};

/// A span begun on a fresh thread and ended from the test thread inside one long frame. Unless the
/// case is `Room`, another thread first records spans, in the next frame's time, until the
/// history has no room left, so the end of the first span finds none for its closed record.
CloseOutcome RunCloseScenario(CloseCase which) {
    SessionConfig config = Config(100 * Ms);
    config.buffer_bytes = 256 * 1024; // rings of 64 events, a history of about 200 records
    Run run{config};
    // The first two presents are resolved, and the history pruned for the first time, before
    // anything below is recorded: nothing frees capacity between the fill and the end.
    Present(run.T(0));
    Present(run.T(1 * Ms));
    AwaitFrames(1);
    WaitUntil(run.T(2 * Ms)); // the span begins after the end of the first frame
    std::atomic<bool> drained{true};
    SpanToken open{};
    OnThread([&] {
        RegisterThread("owner");
        open = BeginSpan(Cause::Shader);
        drained = AwaitDrained(1); // its open record is in the history before anything else
    });
    REQUIRE(drained.load());
    REQUIRE(open.Valid());
    if (which != CloseCase::Room) {
        constexpr u64 Fill = 640; // more records than the history can hold
        constexpr u64 Chunk = 32; // half a ring
        OnThread([&] {
            RegisterThread("filler");
            u64 recorded = 0;
            for (u64 done = 0; done < Fill; done += Chunk) {
                for (u64 i = 0; i < Chunk; ++i) {
                    RecordSpan(Cause::Shader, run.T(20'000 * Ms), run.T(20'005 * Ms));
                }
                recorded += Chunk;
                if (!AwaitDrained(recorded)) {
                    drained = false;
                    return;
                }
            }
        });
        REQUIRE(drained.load());
    }
    if (which != CloseCase::FullNotEnded) {
        EndSpan(open); // the owner's closed vector has to be allocated now
    }
    Present(run.T(10'000 * Ms)); // the long frame holds the span, not the filler's time
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(g_frames.size() == 1);
    return {g_frames[0], snap.lost_events};
}

} // Anonymous namespace

TEST_CASE("A span that ends while the history has room is kept and its frame is complete",
          "[nx2][attribution]") {
    const CloseOutcome outcome = RunCloseScenario(CloseCase::Room);
    CHECK(outcome.lost_events == 0);
    CHECK(outcome.frame.instrumentation_complete);
    CHECK((outcome.frame.unknown_reasons & UnknownReason::LostSpans) == 0);
}

TEST_CASE("A span whose end finds no room in the history is counted as lost and marks its frame",
          "[nx2][attribution]") {
    // The same fill twice, once with the end and once without it: the records the filler loses
    // are the same, so the difference is the end alone, and exactly one.
    const CloseOutcome not_ended = RunCloseScenario(CloseCase::FullNotEnded);
    const CloseOutcome ended = RunCloseScenario(CloseCase::FullEnded);
    REQUIRE(not_ended.lost_events > 0); // the history really was full
    CHECK(ended.lost_events == not_ended.lost_events + 1);
    // The filler's refused records lie after the frame, so only the refused close can touch it.
    CHECK_FALSE(ended.frame.instrumentation_complete);
    CHECK((ended.frame.unknown_reasons & UnknownReason::LostSpans) != 0);
}

TEST_CASE("Deeply nested spans with distinct work ids keep the resolver scratch linear",
          "[nx2][attribution]") {
    SessionConfig config = Config(10 * Ms);
    // The rings are 8192 events each, so the 4096 spans below are recorded whatever the consumer
    // is doing.
    config.buffer_bytes = size_t{32} << 20;
    Run run{config};
    constexpr u64 Spans = 4096;
    constexpr u64 Unit = 1000; // ns
    std::vector<WorkId> works;
    for (u64 i = 0; i < Spans; ++i) {
        works.push_back(NewWorkId());
    }
    Present(run.T(0));
    // Span i runs from (i + 1) to (2 * Spans - i) units: each contains the next, the outermost
    // owns all of them. The root waits 0-10 ms for the outermost work.
    OnThread([&] {
        RegisterThread("nested producer");
        for (u64 i = 0; i < Spans; ++i) {
            RecordSpan(Cause::Shader, run.T((i + 1) * Unit), run.T((2 * Spans - i) * Unit),
                       works[i]);
        }
    });
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(0), run.T(10 * Ms));
    LinkWait(wait, works[0], DependencyKind::Job);
    Present(run.T(20 * Ms));
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    const u64 covered = (2 * Spans - 1) * Unit;
    CHECK(snap.lost_events == 0);
    CHECK(frame.instrumentation_complete);
    CHECK(At(frame.cause_ns, Cause::Shader) == covered);
    CHECK(At(frame.cause_ns, Cause::Waiting) == 10 * Ms - covered);
    CHECK(At(frame.cause_ns, Cause::Unknown) == 10 * Ms);
    CHECK(Total(frame.cause_ns) == 20 * Ms);
    CHECK(At(frame.activity_ns, Cause::Shader) == covered);
    // The peak of everything the resolver held at once is inside the allowance that the same
    // accounting works out for the retained spans, and that allowance inside the budget. It is a
    // few hundred bytes per span, not one entry per span and ancestor pair (which would be
    // 4096 * 4096 entries here).
    const size_t scratch = Detail::PeakResolverScratchBytes();
    const size_t allowance = Detail::ResolverScratchAllowance();
    CHECK(scratch >= Spans * 128);
    CHECK(scratch <= allowance);
    CHECK(allowance <= config.buffer_bytes / 2);
    CHECK(Detail::PeakRetainedHistoryBytes() + scratch <= config.buffer_bytes / 2);
}

TEST_CASE("A full history with every ring in use and a large wait stays inside the scratch bound",
          "[nx2][attribution]") {
    SessionConfig config = Config(10 * Ms);
    config.buffer_bytes = size_t{2} << 20; // rings of 1024 events, a history of about 1700 slots
    Run run{config};
    constexpr u64 Idle = 24;    // registrations that each take a ring and a first vector
    constexpr u64 Nested = 500; // distinct work ids charged by the one wait
    constexpr u64 Fill = 2000;  // more records than the history has room left for
    constexpr u64 Chunk = 256;  // a quarter of a ring
    constexpr u64 Unit = 1000;  // ns
    std::vector<WorkId> works;
    for (u64 i = 0; i < Nested; ++i) {
        works.push_back(NewWorkId());
    }
    Present(run.T(0));
    // The wait of the present thread comes first, and is in the history before anything is
    // recorded that could use up the room.
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(0), run.T(10 * Ms));
    LinkWait(wait, works[0], DependencyKind::Job);
    REQUIRE(AwaitIdle());
    std::atomic<bool> drained{true};
    for (u64 i = 0; i < Idle; ++i) {
        OnThread([&] {
            RegisterThread("idle producer");
            RecordSpan(Cause::Shader, run.T(20'000 * Ms), run.T(20'001 * Ms));
            drained = drained.load() && AwaitDrained(1);
        });
    }
    // Span i runs from (i + 1) to (2 * Nested - i) units: each contains the next, the outermost
    // owns all of them, and every one has a work id of its own.
    OnThread([&] {
        RegisterThread("nested producer");
        for (u64 i = 0; i < Nested; ++i) {
            RecordSpan(Cause::Shader, run.T((i + 1) * Unit), run.T((2 * Nested - i) * Unit),
                       works[i]);
        }
        drained = drained.load() && AwaitDrained(Nested);
    });
    // Then spans after the frame, more than the history can take: it is full, and what does not
    // fit is counted as lost, outside the frame.
    OnThread([&] {
        RegisterThread("filler");
        u64 recorded = 0;
        for (u64 done = 0; done < Fill; done += Chunk) {
            for (u64 i = 0; i < Chunk; ++i) {
                RecordSpan(Cause::Shader, run.T(20'000 * Ms), run.T(20'005 * Ms));
            }
            recorded += Chunk;
            if (!AwaitDrained(recorded)) {
                drained = false;
                return;
            }
        }
    });
    REQUIRE(drained.load());
    Present(run.T(20 * Ms));
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    const u64 covered = (2 * Nested - 1) * Unit;
    CHECK(snap.lost_events > 0); // the history was full
    CHECK(frame.instrumentation_complete);
    CHECK((frame.unknown_reasons & UnknownReason::LostSpans) == 0);
    CHECK(At(frame.cause_ns, Cause::Shader) == covered);
    CHECK(At(frame.cause_ns, Cause::Waiting) == 10 * Ms - covered);
    CHECK(At(frame.cause_ns, Cause::Unknown) == 10 * Ms);
    CHECK(Total(frame.cause_ns) == 20 * Ms);
    // Everything alive at once: the leaves and work spans, the candidates and points of the wait,
    // and the totals of 500 distinct work ids.
    const size_t scratch = Detail::PeakResolverScratchBytes();
    const size_t allowance = Detail::ResolverScratchAllowance();
    CHECK(scratch >= Nested * 128);
    CHECK(scratch <= allowance);
    CHECK(allowance <= config.buffer_bytes / 2);
    CHECK(Detail::PeakRetainedHistoryBytes() + scratch <= config.buffer_bytes / 2);
}

TEST_CASE("An ordinary wait through a worker to a pipeline build is resolved in full",
          "[nx2][attribution]") {
    Run run;
    const WorkId worker = NewWorkId();
    const WorkId build = NewWorkId();
    Present(run.T(0));
    // The present thread waits 0-100 ms for a worker. The worker runs 0-10 ms and 90-100 ms and
    // waits 10-90 ms for a pipeline build on a third thread, which runs a Pipeline span 20-50 ms
    // and a Shader span 50-80 ms. Two levels of waiting, three threads, six leaves in all.
    OnThread([&] {
        const u32 id = RegisterThread("worker");
        RecordSpan(Cause::Waiting, run.T(10 * Ms), run.T(90 * Ms), worker);
        RecordSpan(Cause::Texture, run.T(0), run.T(10 * Ms), worker);
        RecordSpan(Cause::Texture, run.T(90 * Ms), run.T(100 * Ms), worker);
        LinkWait(run.FirstSpanOf(id), build, DependencyKind::Job);
    });
    OnThread([&] {
        RegisterThread("pipeline build");
        RecordSpan(Cause::Pipeline, run.T(20 * Ms), run.T(50 * Ms), build);
        RecordSpan(Cause::Shader, run.T(50 * Ms), run.T(80 * Ms), build);
    });
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms));
    LinkWait(wait, worker, DependencyKind::Join);
    Present(run.T(150 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    CHECK(At(frame.cause_ns, Cause::Texture) == 20 * Ms);
    CHECK(At(frame.cause_ns, Cause::Pipeline) == 30 * Ms);
    CHECK(At(frame.cause_ns, Cause::Shader) == 30 * Ms);
    CHECK(At(frame.cause_ns, Cause::Waiting) == 20 * Ms); // the build was not running 10-20, 80-90
    CHECK(At(frame.cause_ns, Cause::Unknown) == 50 * Ms);
    CHECK(Total(frame.cause_ns) == 150 * Ms);
    CHECK((frame.unknown_reasons & UnknownReason::AmbiguousDependency) == 0);
    CHECK(frame.instrumentation_complete);
    // The build is charged 80 ms (its spans and the gaps in its wait), the worker 20 ms.
    REQUIRE(frame.critical_work_count == 2);
    CHECK(frame.critical_work_ids[0] == build);
    CHECK(frame.critical_work_ids[1] == worker);
}

TEST_CASE("Nested spans that overflow a small ring are counted as lost and the frame says so",
          "[nx2][attribution]") {
    ResetBlock();
    SetLongFrameSink({BlockFirstFrame, nullptr});
    SessionConfig config = Config(10 * Ms);
    config.buffer_bytes = 16 * 1024; // rings of 4 events
    REQUIRE(BeginSession(config));
    constexpr u64 Spans = 256;
    constexpr u64 Unit = 1000; // ns
    std::vector<WorkId> works;
    for (u64 i = 0; i < Spans; ++i) {
        works.push_back(NewWorkId());
    }
    const u64 base = NowNs();
    Present(base);
    Present(base + 20 * Ms); // the consumer stops in the sink with this frame
    AwaitBlocked();
    // It cannot drain now, so the 4-event ring takes four of the spans and loses the rest.
    OnThread([&] {
        RegisterThread("nested producer");
        for (u64 i = 0; i < Spans; ++i) {
            RecordSpan(Cause::Shader, base + 30 * Ms + (i + 1) * Unit,
                       base + 30 * Ms + (2 * Spans - i) * Unit, works[i]);
        }
    });
    Present(base + 60 * Ms);
    g_block.release.store(true);
    AwaitFrames(2);
    EndSession();
    SetLongFrameSink({});
    const AttributionSnapshot snap = ReadAttributionSnapshot();
    REQUIRE(g_frames.size() == 2);
    const LongFrame& frame = g_frames[1];
    CHECK(snap.lost_events >= Spans - 4);
    CHECK_FALSE(snap.complete);
    CHECK_FALSE(frame.instrumentation_complete);
    CHECK((frame.unknown_reasons & UnknownReason::LostSpans) != 0);
    CHECK(Total(frame.cause_ns) == 40 * Ms);
    // What was kept is never more than the spans that were recorded cover.
    CHECK(At(frame.cause_ns, Cause::Shader) <= (2 * Spans - 1) * Unit);
}

TEST_CASE("A wait chain whose candidates outnumber the leaves is reported, not resolved",
          "[nx2][attribution]") {
    Run run;
    const WorkId first = NewWorkId();
    const WorkId second = NewWorkId();
    Present(run.T(0));
    // The thread that runs `first` waits for `second`. Two workers run both works at once (a span
    // of `first` around a span of `second`), so each of their leaves is a candidate at both
    // levels of the chain: 3 + 2 candidate pieces against 4 leaves.
    OnThread([&] {
        const u32 id = RegisterThread("chain wait");
        RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms), first);
        LinkWait(run.FirstSpanOf(id), second, DependencyKind::Job);
    });
    for (int worker = 0; worker < 2; ++worker) {
        OnThread([&] {
            RegisterThread("worker");
            RecordSpan(Cause::Shader, run.T(0), run.T(90 * Ms), first);
            RecordSpan(Cause::Shader, run.T(0), run.T(90 * Ms), second);
        });
    }
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(0), run.T(100 * Ms));
    LinkWait(wait, first, DependencyKind::Job);
    Present(run.T(150 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    CHECK(At(frame.cause_ns, Cause::Waiting) == 100 * Ms);
    CHECK(At(frame.cause_ns, Cause::Shader) == 0);
    CHECK(At(frame.cause_ns, Cause::Unknown) == 50 * Ms);
    CHECK(Total(frame.cause_ns) == 150 * Ms);
    CHECK((frame.unknown_reasons & UnknownReason::AmbiguousDependency) != 0);
    CHECK_FALSE(frame.instrumentation_complete);
}

TEST_CASE("The first 120 seconds roll over at the window end", "[nx2][attribution]") {
    SECTION("A present exactly at the end completes it") {
        Run run;
        Present(run.T(0));
        Present(run.T(30'000 * Ms));
        Present(run.T(120'000 * Ms)); // exactly anchor + 120 s: inside
        Present(run.T(121'000 * Ms)); // after the window: not counted in it
        Present(run.T(125'000 * Ms));
        const AttributionSnapshot snap = run.Finish();
        CHECK(snap.frames == 4);
        CHECK(snap.first_120s.anchor_ns == run.T(0));
        CHECK(snap.first_120s.complete);
        CHECK(snap.first_120s.elapsed_ns == FirstWindowNs);
        CHECK(snap.first_120s.frames == 2);
        CHECK(snap.first_120s.frames_over_threshold == 2);
        CHECK(snap.first_120s.max_ns == 90'000 * Ms);
        CHECK(snap.first_120s.crossing_tail_interval_ns == 0); // nothing crossed, it ended on it
    }
    SECTION("A present past the end splits nothing and keeps the crossing interval") {
        Run run;
        Present(run.T(0));
        Present(run.T(90'000 * Ms));
        Present(run.T(130'000 * Ms)); // 40 s interval crossing the window end
        const AttributionSnapshot snap = run.Finish();
        CHECK(snap.first_120s.complete);
        CHECK(snap.first_120s.frames == 1);
        CHECK(snap.first_120s.crossing_tail_interval_ns == 40'000 * Ms);
        CHECK(snap.frames == 2);
    }
    SECTION("A shorter run is not complete") {
        Run run;
        Present(run.T(0));
        Present(run.T(10 * Ms));
        Present(run.T(300 * Ms));
        const AttributionSnapshot snap = run.Finish();
        CHECK_FALSE(snap.first_120s.complete);
        CHECK(snap.first_120s.elapsed_ns == 300 * Ms);
        CHECK(snap.first_120s.frames == 2);
        CHECK(snap.first_120s.frames_over_threshold == 1);
        CHECK(snap.first_120s.crossing_tail_interval_ns == 0);
    }
}

TEST_CASE("The cold start before the first present is attributed, time before the session is "
          "unknown",
          "[nx2][attribution]") {
    // Reads before the session exists are pending and the session takes them.
    const u64 created = NowNs();
    SetProcessCreated(created, 1234);
    BeginBootPhase(BootPhase::CacheValidate); // no session: ignored
    Run run;
    RecordSpan(Cause::Disk, run.T(2 * Ms), run.T(7 * Ms));
    Present(run.T(10 * Ms));
    const AttributionSnapshot snap = run.Finish();
    CHECK(snap.process_created_ns == created);
    CHECK(snap.process_created_uncertainty_ns == 1234);
    CHECK(snap.phases.empty());
    const IntervalAttribution& startup = snap.process_to_first_present;
    CHECK(startup.status == IntervalStatus::Complete);
    CHECK(startup.duration_ns == run.T(10 * Ms) - created);
    CHECK(At(startup.cause_ns, Cause::Disk) == 5 * Ms);
    CHECK(Total(startup.cause_ns) == startup.duration_ns);
    // Everything but the read is unknown, including what happened before the session began.
    CHECK(At(startup.cause_ns, Cause::Unknown) == startup.duration_ns - 5 * Ms);
    CHECK(At(startup.cause_ns, Cause::Unknown) >= run.base - created);
}

TEST_CASE("A process creation time that is not set or not before the first present is not observed",
          "[nx2][attribution]") {
    {
        Run run;
        Present(run.T(0));
        const AttributionSnapshot snap = run.Finish();
        CHECK(snap.process_created_ns == 0);
        CHECK(snap.process_to_first_present.status == IntervalStatus::NotObserved);
    }
    {
        Run run;
        SetProcessCreated(run.T(50 * Ms), 0); // after the first present
        Present(run.T(0));
        const AttributionSnapshot snap = run.Finish();
        CHECK(snap.process_to_first_present.status == IntervalStatus::NotObserved);
    }
}

TEST_CASE("Boot phases are recorded in order and bounded", "[nx2][attribution]") {
    Run run;
    BeginBootPhase(BootPhase::CacheValidate);
    EndBootPhase(BootPhase::WarmRefresh, PhaseOutcome::Ok); // never begun: nothing to close
    EndBootPhase(BootPhase::CacheValidate, PhaseOutcome::Cancelled);
    BeginBootPhase(BootPhase::PipelineReplay);
    for (int i = 0; i < 100; ++i) {
        BeginBootPhase(BootPhase::GameRun);
    }
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(snap.phases.size() == 64);
    CHECK(snap.phases[0].phase == BootPhase::CacheValidate);
    CHECK(snap.phases[0].outcome == PhaseOutcome::Cancelled);
    CHECK(snap.phases[0].end_ns >= snap.phases[0].begin_ns);
    CHECK(snap.phases[0].end_ns != 0);
    CHECK(snap.phases[1].phase == BootPhase::PipelineReplay);
    CHECK(snap.phases[1].end_ns == 0); // still running when the session ended
}

TEST_CASE("The interactive marker is accepted once, never backdated", "[nx2][attribution]") {
    InteractiveMarker marker;
    marker.scene_id = "scene";
    marker.origin = "lab";
    marker.marker_ns = NowNs();
    CHECK(MarkInteractive(marker) == MarkerResult::NoSession);

    Run run;
    CHECK(MarkInteractive(marker) == MarkerResult::Backdated); // before any present
    Present(run.T(0));
    Present(run.T(10 * Ms));
    RecordSpan(Cause::Shader, run.T(12 * Ms), run.T(14 * Ms));
    Present(run.T(20 * Ms));
    WaitUntil(run.T(25 * Ms));

    marker.marker_ns = run.T(0) - 1;
    CHECK(MarkInteractive(marker) == MarkerResult::Backdated); // before the first present
    marker.marker_ns = NowNs() + 1'000 * Ms;
    CHECK(MarkInteractive(marker) == MarkerResult::Backdated); // in the future
    marker.marker_ns = run.T(15 * Ms); // between the second and third present
    CHECK(MarkInteractive(marker) == MarkerResult::Accepted);
    CHECK(MarkInteractive(marker) == MarkerResult::Duplicate);

    const AttributionSnapshot snap = run.Finish();
    CHECK(MarkInteractive(marker) == MarkerResult::NoSession);
    CHECK(snap.interactive.scene_id == "scene");
    CHECK(snap.interactive_status == IntervalStatus::Complete);
    const IntervalAttribution& interval = snap.first_present_to_interactive;
    CHECK(interval.status == IntervalStatus::Complete);
    CHECK(interval.duration_ns == 15 * Ms);
    CHECK(At(interval.cause_ns, Cause::Shader) == 2 * Ms);
    CHECK(At(interval.cause_ns, Cause::Unknown) == 13 * Ms);
    CHECK(Total(interval.cause_ns) == interval.duration_ns);
}

TEST_CASE("Unknown reasons noted while a frame is open land in that frame", "[nx2][attribution]") {
    NoteUnknownReason(UnknownReason::MissingGpuTimestamp); // no session: ignored
    Run run;
    Present(run.T(0));
    NoteUnknownReason(UnknownReason::MissingGpuTimestamp);
    Present(run.T(150 * Ms));
    Present(run.T(300 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 2);
    CHECK((g_frames[0].unknown_reasons & UnknownReason::MissingGpuTimestamp) != 0);
    CHECK((g_frames[1].unknown_reasons & UnknownReason::MissingGpuTimestamp) == 0);
}

TEST_CASE("A sink that fails is counted and makes the record incomplete", "[nx2][attribution]") {
    Run run;
    SetLongFrameSink({FailingSink, nullptr});
    Present(run.T(0));
    Present(run.T(150 * Ms));
    const AttributionSnapshot snap = run.Finish();
    CHECK(snap.long_frames == 1);
    CHECK(snap.sink_write_errors == 1);
    CHECK_FALSE(snap.complete);
}

TEST_CASE("The crash status mirrors the session", "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    Present(run.T(150 * Ms));
    run.Finish();
    const CrashStatus status = ReadCrashStatus();
    CHECK_FALSE(status.session_active);
    CHECK(status.threshold_ns == 100 * Ms);
    CHECK(status.frames == 1);
    CHECK(status.long_frames == 1);
    CHECK(status.first_host_present_ns == run.T(0));
    CHECK(status.last_host_present_ns == run.T(150 * Ms));
    CHECK(status.last_long_frame_id == 2);
    CHECK(status.last_long_frame_ns == 150 * Ms);
    CHECK(status.complete);
}

TEST_CASE("The long frame line prints every cause in order", "[nx2][attribution]") {
    LongFrame frame;
    frame.id = 123;
    frame.duration_ns = 154'200'000;
    frame.cause_ns[static_cast<size_t>(Cause::Shader)] = 92'100'000;
    frame.cause_ns[static_cast<size_t>(Cause::Waiting)] = 40'000'000;
    frame.cause_ns[static_cast<size_t>(Cause::Unknown)] = 22'100'000;
    frame.primary_cause = Cause::Shader;
    frame.instrumentation_complete = true;
    CHECK(LongFrameLine(frame) ==
          "Nx2LongFrame frame=123 dt_ms=154.2 primary=shader disk_ms=0.0 shader_ms=92.1 "
          "pipeline_ms=0.0 texture_ms=0.0 decompression_ms=0.0 waiting_ms=40.0 "
          "code_translation_ms=0.0 unknown_ms=22.1 complete=1");
    frame.instrumentation_complete = false;
    CHECK(LongFrameLine(frame).ends_with(" complete=0"));
}

TEST_CASE("Derived work ids never meet allocated ones", "[nx2][attribution]") {
    const WorkId a = NewWorkId();
    const WorkId b = NewWorkId();
    CHECK(a != 0);
    CHECK(b > a);
    const WorkId derived = DerivedWorkId(WorkKind::Submission, 77);
    CHECK(derived != a);
    CHECK(derived != DerivedWorkId(WorkKind::GraphicsPipelineBuild, 77));
    CHECK(DerivedWorkId(WorkKind::Submission, 0) != 0);
}

TEST_CASE("A sustained burst of spans over many frames loses nothing and stays in the budget",
          "[nx2][attribution]") {
    const SessionConfig config = Config(100 * Ms); // the default budget: a history of about 14000
    constexpr u64 Frames = 40;
    constexpr u64 SpansPerFrame = 1000; // 40000 in all: nearly three histories
    constexpr u64 Step = 20'000;        // ns between spans
    constexpr u64 SpanNs = 10'000;
    constexpr u64 FrameNs = 150 * Ms;
    constexpr u64 RingChunk = 512; // an eighth of the ring, recorded between two drains
    Run run{config};
    const u64 origin = run.base;
    Present(origin);
    std::atomic<u64> go{0};
    std::atomic<u64> done{0};
    std::atomic<bool> failed{false};
    std::thread burst{[&] {
        RegisterThread("burst producer");
        u64 recorded = 0;
        for (u64 frame = 1; frame <= Frames; ++frame) {
            while (go.load() < frame) {
                std::this_thread::yield();
            }
            const u64 begin = origin + (frame - 1) * FrameNs + Ms;
            for (u64 i = 0; i < SpansPerFrame; ++i) {
                RecordSpan(Cause::Shader, begin + i * Step, begin + i * Step + SpanNs);
                if (++recorded % RingChunk == 0 && !AwaitDrained(recorded)) {
                    failed = true;
                    return;
                }
            }
            if (!AwaitDrained(recorded)) {
                failed = true;
                return;
            }
            done.store(frame);
        }
    }};
    for (u64 frame = 1; frame <= Frames && !failed.load(); ++frame) {
        const u64 start = origin + (frame - 1) * FrameNs;
        go.store(frame);
        while (done.load() < frame && !failed.load()) {
            std::this_thread::yield();
        }
        // The root's own span in this frame, recorded in the same time as the burst.
        RecordSpan(Cause::Texture, start, start + 60 * Ms);
        REQUIRE(AwaitIdle());
        Present(start + FrameNs);
    }
    burst.join();
    REQUIRE_FALSE(failed.load());
    AwaitFrames(Frames);
    const AttributionSnapshot snap = run.Finish();
    CHECK(snap.lost_events == 0);
    CHECK(snap.complete);
    CHECK(snap.frames == Frames);
    REQUIRE(g_frames.size() == Frames);
    for (u64 i = 0; i < Frames; ++i) {
        const LongFrame& frame = g_frames[i];
        CHECK(frame.id == i + 2);
        CHECK(frame.duration_ns == FrameNs);
        CHECK(frame.instrumentation_complete);
        CHECK((frame.unknown_reasons & UnknownReason::LostSpans) == 0);
        CHECK(At(frame.cause_ns, Cause::Texture) == 60 * Ms);
        CHECK(At(frame.cause_ns, Cause::Unknown) == FrameNs - 60 * Ms);
        CHECK(Total(frame.cause_ns) == FrameNs);
        CHECK(At(frame.activity_ns, Cause::Shader) == SpansPerFrame * SpanNs);
        CHECK(At(frame.activity_ns, Cause::Texture) == 60 * Ms);
    }
    // The spans of all the frames never sat in the history together.
    CHECK(Detail::PeakRetainedHistoryBytes() <= config.buffer_bytes / 2);
}

TEST_CASE("One frame with more spans than the history holds is resolved in part, in full",
          "[nx2][attribution]") {
    Run run{Config(Ms)};
    constexpr u64 Spans = 40000; // nearly three histories, inside one frame
    constexpr u64 RingChunk = 256;
    const u64 start = NowNs();
    Present(start);
    std::atomic<bool> failed{false};
    u64 expected = 0;
    OnThread([&] {
        RegisterThread("burst producer");
        for (u64 i = 0; i < Spans; ++i) {
            // Real stamps, one after the other: each span lasts at least a microsecond and ends
            // before the next begins, so none of them is cut by another.
            const u64 begin = NowNs();
            u64 end = NowNs();
            while (end - begin < 1000) {
                end = NowNs();
            }
            RecordSpan(Cause::Shader, begin, end);
            expected += end - begin;
            if ((i + 1) % RingChunk == 0 && !AwaitDrained(i + 1)) {
                failed = true;
                return;
            }
        }
    });
    REQUIRE_FALSE(failed.load());
    const u64 end = NowNs();
    Present(end);
    AwaitFrames(1);
    const AttributionSnapshot snap = run.Finish();
    CHECK(snap.lost_events == 0);
    CHECK(snap.complete);
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    CHECK(frame.duration_ns == end - start);
    CHECK(frame.instrumentation_complete);
    CHECK((frame.unknown_reasons & UnknownReason::LostSpans) == 0);
    CHECK(Total(frame.cause_ns) == end - start);
    CHECK(At(frame.activity_ns, Cause::Shader) == expected);
    CHECK(Detail::PeakRetainedHistoryBytes() <= Config(Ms).buffer_bytes / 2);
}

TEST_CASE("Rings that deliver more at once than the history holds lose nothing",
          "[nx2][attribution]") {
    Run run{Config(Ms)};
    constexpr u32 Producers = 6;
    constexpr u64 SpansEach = 4000; // inside a ring (4096), together nearly twice the history
    const u64 start = NowNs();
    Present(start);
    REQUIRE(Detail::HoldConsumer(true)); // every event waits in its ring until all are published
    std::array<u64, Producers> expected{};
    std::vector<std::thread> threads;
    for (u32 t = 0; t < Producers; ++t) {
        threads.emplace_back([&expected, t] {
            RegisterThread("burst producer");
            for (u64 i = 0; i < SpansEach; ++i) {
                const u64 begin = NowNs();
                u64 end = NowNs();
                while (end - begin < 1000) {
                    end = NowNs();
                }
                RecordSpan(Cause::Shader, begin, end);
                expected[t] += end - begin;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const u64 end = NowNs();
    Present(end);
    REQUIRE(Detail::HoldConsumer(false));
    AwaitFrames(1);
    const AttributionSnapshot snap = run.Finish();
    u64 sum = 0;
    for (const u64 value : expected) {
        sum += value;
    }
    CHECK(snap.lost_events == 0);
    CHECK(snap.complete);
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    CHECK(frame.duration_ns == end - start);
    CHECK(frame.instrumentation_complete);
    CHECK((frame.unknown_reasons & UnknownReason::LostSpans) == 0);
    CHECK(Total(frame.cause_ns) == end - start);
    CHECK(At(frame.activity_ns, Cause::Shader) == sum);
    CHECK(Detail::PeakRetainedHistoryBytes() <= Config(Ms).buffer_bytes / 2);
}

TEST_CASE("A link whose owner is still in its ring does not age between the rounds of a pass",
          "[nx2][attribution]") {
    Run run{Config(Ms)};
    constexpr u64 Spans = 4000; // 500 rounds of 8 before the owner's record, far past 8 attempts
    const WorkId work = NewWorkId();
    Present(run.T(0));
    REQUIRE(Detail::HoldConsumer(true));
    Detail::ForceHistoryRelief(true);
    for (u64 i = 0; i < Spans; ++i) {
        (void)run.NextRootToken();
        RecordSpan(Cause::Texture, run.T(i * 1000), run.T(i * 1000 + 1000));
    }
    const SpanToken wait = run.NextRootToken();
    RecordSpan(Cause::Waiting, run.T(10 * Ms), run.T(100 * Ms));
    OnThread([&] {
        RegisterThread("producer");
        RecordSpan(Cause::Shader, run.T(20 * Ms), run.T(60 * Ms), work);
    });
    OnThread([&] {
        RegisterThread("linker");
        LinkWait(wait, work, DependencyKind::Job);
    });
    Present(run.T(150 * Ms));
    REQUIRE(Detail::HoldConsumer(false));
    AwaitFrames(1);
    const AttributionSnapshot snap = run.Finish();
    CHECK(snap.lost_events == 0);
    CHECK(snap.complete);
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    CHECK(At(frame.cause_ns, Cause::Shader) == 40 * Ms); // the link reached its wait
    CHECK(At(frame.cause_ns, Cause::Waiting) == 50 * Ms);
    CHECK(At(frame.cause_ns, Cause::Texture) == Spans * 1000);
    CHECK(Total(frame.cause_ns) == 150 * Ms);
}

TEST_CASE("Ending a session resolves nothing in part before the open spans are marked",
          "[nx2][attribution]") {
    Run run{Config(Ms)};
    Present(run.T(0));
    Present(run.T(5 * Ms));
    AwaitFrames(1); // the first present is behind the consumer: only the last interval is left
    REQUIRE(Detail::HoldConsumer(true));
    (void)BeginSpan(Cause::Shader); // never ended
    Present(run.T(60 * Ms));
    WaitUntil(run.T(62 * Ms)); // the pass runs past the interval's end plus the margin
    OnThread([&] {
        RegisterThread("later work");
        for (u64 i = 0; i < 300; ++i) {
            const u64 begin = NowNs();
            u64 end = NowNs();
            while (end - begin < 1000) {
                end = NowNs();
            }
            RecordSpan(Cause::Disk, begin, end);
        }
    });
    Detail::ForceHistoryRelief(true); // the rounds of the final pass relieve the history
    run.Finish();
    REQUIRE(g_frames.size() == 2);
    CHECK(g_frames[1].end_ns == run.T(60 * Ms));
    CHECK_FALSE(g_frames[1].instrumentation_complete); // the open span was marked first
}

TEST_CASE("Losses mark only the frames they fall in, however many there are",
          "[nx2][attribution]") {
    SessionConfig config = Config(10 * Ms);
    config.buffer_bytes = 16 * 1024; // rings of 4 events and a history of 64 spans
    Run run{config};
    constexpr u64 Spans = 1300; // refused for room one by one: more episodes than are kept
    constexpr u64 Spacing = 2 * Ms;
    Present(run.T(0));
    Present(run.T(20 * Ms)); // the frame before: complete
    std::atomic<bool> failed{false};
    OnThread([&] {
        RegisterThread("lossy producer");
        u64 recorded = 0;
        const auto record = [&](u64 at) {
            RecordSpan(Cause::Shader, at, at + 1000);
            ++recorded;
            if (!AwaitDrained(recorded)) {
                failed = true;
            }
        };
        // In the second frame, further apart than a loss episode reaches.
        for (u64 i = 0; i < Spans && !failed.load(); ++i) {
            record(run.T(100 * Ms + i * Spacing));
        }
        // One more in the fifth, with the third and fourth frames between: they are not touched.
        record(run.T(3050 * Ms));
    });
    REQUIRE_FALSE(failed.load());
    Present(run.T(2800 * Ms));
    Present(run.T(2900 * Ms));
    Present(run.T(3000 * Ms));
    Present(run.T(3100 * Ms));
    AwaitFrames(5);
    const AttributionSnapshot snap = run.Finish();
    CHECK(snap.lost_events > 1024);
    CHECK_FALSE(snap.complete);
    REQUIRE(g_frames.size() == 5);
    for (const LongFrame& frame : g_frames) {
        CHECK(Total(frame.cause_ns) == frame.duration_ns);
    }
    CHECK(g_frames[0].instrumentation_complete);
    CHECK_FALSE(g_frames[1].instrumentation_complete);
    CHECK((g_frames[1].unknown_reasons & UnknownReason::LostSpans) != 0);
    CHECK(g_frames[2].instrumentation_complete);
    CHECK((g_frames[2].unknown_reasons & UnknownReason::LostSpans) == 0);
    CHECK(g_frames[3].instrumentation_complete);
    CHECK((g_frames[3].unknown_reasons & UnknownReason::LostSpans) == 0);
    CHECK_FALSE(g_frames[4].instrumentation_complete);
    CHECK((g_frames[4].unknown_reasons & UnknownReason::LostSpans) != 0);
}

TEST_CASE("A wait for the guest is charged to the guest's translation", "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    OnThread([&] {
        RegisterThread("guest core");
        MarkGuestThread();
        RecordSpan(Cause::CodeTranslation, run.T(0), run.T(120 * Ms), 0, 1);
    });
    RecordSpan(Cause::Waiting, run.T(0), run.T(150 * Ms), 0, 0, SpanFlags::GuestWait);
    Present(run.T(150 * Ms));
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    CHECK(At(frame.cause_ns, Cause::CodeTranslation) == 120 * Ms);
    CHECK(At(frame.cause_ns, Cause::Waiting) == 30 * Ms);
    CHECK(At(frame.cause_ns, Cause::Unknown) == 0);
    CHECK(Total(frame.cause_ns) == frame.duration_ns);
    CHECK(frame.primary_cause == Cause::CodeTranslation);
    // The 30 ms the guest was not translating is Waiting with no known producer: the reason bit
    // is the one that goes with producer_unknown_wait_ns, and no other reason applies.
    CHECK(frame.unknown_reasons == UnknownReason::ProducerUnknownWait);
    CHECK(frame.instrumentation_complete);
    CHECK(snap.producer_unknown_wait_ns == 30 * Ms);
}

TEST_CASE("A wait for a guest that is idle or waiting itself stays Waiting",
          "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    OnThread([&] {
        RegisterThread("guest core");
        MarkGuestThread();
        // Its own wait is not work: it must not be taken for a cause.
        RecordSpan(Cause::Waiting, run.T(20 * Ms), run.T(90 * Ms));
    });
    RecordSpan(Cause::Waiting, run.T(0), run.T(150 * Ms), 0, 0, SpanFlags::GuestWait);
    Present(run.T(150 * Ms));
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    CHECK(At(frame.cause_ns, Cause::Waiting) == 150 * Ms);
    CHECK(At(frame.cause_ns, Cause::Unknown) == 0);
    CHECK(Total(frame.cause_ns) == frame.duration_ns);
    CHECK(frame.primary_cause == Cause::Waiting);
    CHECK(snap.producer_unknown_wait_ns == 150 * Ms);
}

TEST_CASE("A wait for the guest splits between guest causes and waiting, and sums exactly",
          "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    OnThread([&] {
        RegisterThread("guest core 0");
        MarkGuestThread();
        RecordSpan(Cause::Disk, run.T(10 * Ms), run.T(40 * Ms));
    });
    OnThread([&] {
        RegisterThread("guest core 1");
        MarkGuestThread();
        RecordSpan(Cause::CodeTranslation, run.T(30 * Ms), run.T(90 * Ms), 0, 1);
    });
    // A host worker is not a guest thread: its span is no reason for the wait.
    OnThread([&] {
        RegisterThread("host worker");
        RecordSpan(Cause::Shader, run.T(0), run.T(150 * Ms));
    });
    RecordSpan(Cause::Waiting, run.T(0), run.T(150 * Ms), 0, 0, SpanFlags::GuestWait);
    Present(run.T(150 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    // 10-30 ms disk alone, 30-90 ms translation (it ends last where both run), 0-10 and 90-150
    // ms nobody on the guest side.
    CHECK(At(frame.cause_ns, Cause::Disk) == 20 * Ms);
    CHECK(At(frame.cause_ns, Cause::CodeTranslation) == 60 * Ms);
    CHECK(At(frame.cause_ns, Cause::Waiting) == 70 * Ms);
    CHECK(At(frame.cause_ns, Cause::Shader) == 0);
    CHECK(At(frame.cause_ns, Cause::Unknown) == 0);
    CHECK(Total(frame.cause_ns) == 150 * Ms);
    CHECK(At(frame.activity_ns, Cause::Shader) == 150 * Ms);
}

TEST_CASE("Without a wait, guest spans or a missing wait stay Unknown", "[nx2][attribution]") {
    SECTION("no spans at all") {
        Run run;
        Present(run.T(0));
        Present(run.T(150 * Ms));
        run.Finish();
        REQUIRE(g_frames.size() == 1);
        CHECK(At(g_frames[0].cause_ns, Cause::Unknown) == 150 * Ms);
        CHECK((g_frames[0].unknown_reasons & UnknownReason::UnlinkedGuest) != 0);
        CHECK(Total(g_frames[0].cause_ns) == 150 * Ms);
    }
    SECTION("guest work while the render thread has no wait recorded") {
        Run run;
        Present(run.T(0));
        OnThread([&] {
            RegisterThread("guest core");
            MarkGuestThread();
            RecordSpan(Cause::CodeTranslation, run.T(0), run.T(100 * Ms), 0, 1);
        });
        Present(run.T(150 * Ms));
        run.Finish();
        REQUIRE(g_frames.size() == 1);
        CHECK(At(g_frames[0].cause_ns, Cause::Unknown) == 150 * Ms);
        CHECK(At(g_frames[0].cause_ns, Cause::CodeTranslation) == 0);
        CHECK((g_frames[0].unknown_reasons & UnknownReason::UnlinkedGuest) != 0);
    }
}

TEST_CASE("An unlinked wait without the guest flag keeps its meaning", "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    OnThread([&] {
        RegisterThread("guest core");
        MarkGuestThread();
        RecordSpan(Cause::CodeTranslation, run.T(0), run.T(120 * Ms), 0, 1);
    });
    RecordSpan(Cause::Waiting, run.T(0), run.T(150 * Ms));
    Present(run.T(150 * Ms));
    const AttributionSnapshot snap = run.Finish();
    REQUIRE(g_frames.size() == 1);
    CHECK(At(g_frames[0].cause_ns, Cause::Waiting) == 150 * Ms);
    CHECK(At(g_frames[0].cause_ns, Cause::CodeTranslation) == 0);
    CHECK(snap.producer_unknown_wait_ns == 150 * Ms);
}

TEST_CASE("A long wait scope records one guest wait span", "[nx2][attribution]") {
    Run run;
    Present(run.T(0));
    OnThread([&] {
        RegisterThread("guest core");
        MarkGuestThread();
        RecordSpan(Cause::CodeTranslation, run.T(0), run.T(200 * Ms), 0, 1);
    });
    {
        LongWaitScope wait{SpanFlags::GuestWait};
        WaitUntil(NowNs() + LongWaitMinNs);
    }
    Present(run.T(200 * Ms));
    run.Finish();
    REQUIRE(g_frames.size() == 1);
    const LongFrame& frame = g_frames[0];
    // The scope's span lies at real time inside the frame: the guest's translation takes it.
    CHECK(At(frame.cause_ns, Cause::CodeTranslation) >= LongWaitMinNs);
    CHECK(At(frame.cause_ns, Cause::CodeTranslation) < 200 * Ms);
    CHECK(Total(frame.cause_ns) == frame.duration_ns);
}

TEST_CASE("Time the presenting thread has no span for is charged by what the others were doing",
          "[nx2][attribution]") {
    SECTION("charged to the other threads' causes, the rest is Waiting") {
        Run run{SessionConfig{}};
        Present(run.T(0));
        RecordSpan(Cause::Shader, run.T(0), run.T(30 * Ms));
        OnThread([&] {
            RegisterThread("worker");
            RecordSpan(Cause::Disk, run.T(40 * Ms), run.T(70 * Ms));
        });
        OnThread([&] {
            RegisterThread("guest core");
            MarkGuestThread();
            RecordSpan(Cause::CodeTranslation, run.T(60 * Ms), run.T(100 * Ms), 0, 1);
        });
        Present(run.T(150 * Ms));
        const AttributionSnapshot snap = run.Finish();
        REQUIRE(g_frames.size() == 1);
        const LongFrame& frame = g_frames[0];
        // [30, 40) nobody works; [40, 60) the disk; [60, 100) the translation, which ends last of
        // the two that overlap in [60, 70); [100, 150) nobody.
        CHECK(At(frame.cause_ns, Cause::Shader) == 30 * Ms);
        CHECK(At(frame.cause_ns, Cause::Disk) == 20 * Ms);
        CHECK(At(frame.cause_ns, Cause::CodeTranslation) == 40 * Ms);
        CHECK(At(frame.cause_ns, Cause::Waiting) == 60 * Ms);
        CHECK(At(frame.cause_ns, Cause::Unknown) == 0);
        CHECK(Total(frame.cause_ns) == 150 * Ms);
        CHECK(frame.primary_cause == Cause::Waiting);
        CHECK(frame.unknown_reasons == UnknownReason::PresenterGap);
        CHECK(At(frame.activity_ns, Cause::Disk) == 30 * Ms);
        // What the spans cover is observed; only the gaps are inferred, per cause.
        CHECK(At(frame.inferred_ns, Cause::Shader) == 0);
        CHECK(At(frame.inferred_ns, Cause::Disk) == 20 * Ms);
        CHECK(At(frame.inferred_ns, Cause::CodeTranslation) == 40 * Ms);
        CHECK(At(frame.inferred_ns, Cause::Waiting) == 60 * Ms);
        CHECK(At(frame.inferred_ns, Cause::Unknown) == 0);
        CHECK(Total(frame.inferred_ns) == 120 * Ms);
        CHECK(At(snap.inferred_ns, Cause::Disk) == 20 * Ms);
        CHECK(At(snap.inferred_ns, Cause::Waiting) == 60 * Ms);
        CHECK(Total(snap.inferred_ns) == 120 * Ms);
    }
    SECTION("with nobody else working the gap is the presenter's own Waiting") {
        Run run{SessionConfig{}};
        Present(run.T(0));
        RecordSpan(Cause::Shader, run.T(0), run.T(20 * Ms));
        Present(run.T(150 * Ms));
        run.Finish();
        REQUIRE(g_frames.size() == 1);
        CHECK(At(g_frames[0].cause_ns, Cause::Shader) == 20 * Ms);
        CHECK(At(g_frames[0].cause_ns, Cause::Waiting) == 130 * Ms);
        CHECK(At(g_frames[0].cause_ns, Cause::Unknown) == 0);
        CHECK(g_frames[0].unknown_reasons == UnknownReason::PresenterGap);
        CHECK(At(g_frames[0].inferred_ns, Cause::Shader) == 0);
        CHECK(At(g_frames[0].inferred_ns, Cause::Waiting) == 130 * Ms);
    }
    SECTION("the strict decomposition leaves the gap Unknown") {
        Run run{Strict()};
        Present(run.T(0));
        RecordSpan(Cause::Shader, run.T(0), run.T(30 * Ms));
        OnThread([&] {
            RegisterThread("worker");
            RecordSpan(Cause::Disk, run.T(40 * Ms), run.T(70 * Ms));
        });
        Present(run.T(150 * Ms));
        run.Finish();
        REQUIRE(g_frames.size() == 1);
        CHECK(At(g_frames[0].cause_ns, Cause::Unknown) == 120 * Ms);
        CHECK(At(g_frames[0].cause_ns, Cause::Disk) == 0);
        CHECK((g_frames[0].unknown_reasons & UnknownReason::PresenterGap) == 0);
        CHECK(Total(g_frames[0].inferred_ns) == 0);
    }
    SECTION("a window nobody recorded anything in stays Unknown") {
        Run run{SessionConfig{}};
        Present(run.T(0));
        Present(run.T(150 * Ms));
        run.Finish();
        REQUIRE(g_frames.size() == 1);
        CHECK(At(g_frames[0].cause_ns, Cause::Unknown) == 150 * Ms);
        CHECK(g_frames[0].unknown_reasons == UnknownReason::UnlinkedGuest);
    }
    SECTION("a decompression with a file read inside it shows as both, each time once") {
        Run run{SessionConfig{}};
        Present(run.T(0));
        RecordSpan(Cause::Shader, run.T(0), run.T(30 * Ms));
        OnThread([&] {
            RegisterThread("guest core");
            MarkGuestThread();
            RecordSpan(Cause::Decompression, run.T(40 * Ms), run.T(100 * Ms), 0, 1 << 20);
            RecordSpan(Cause::Disk, run.T(50 * Ms), run.T(60 * Ms), 0, 4096);
        });
        Present(run.T(150 * Ms));
        run.Finish();
        REQUIRE(g_frames.size() == 1);
        const LongFrame& frame = g_frames[0];
        CHECK(At(frame.cause_ns, Cause::Disk) == 10 * Ms);
        CHECK(At(frame.cause_ns, Cause::Decompression) == 50 * Ms);
        CHECK(At(frame.cause_ns, Cause::Waiting) == 60 * Ms);
        CHECK(At(frame.cause_ns, Cause::Unknown) == 0);
        CHECK(Total(frame.cause_ns) == 150 * Ms);
        CHECK(At(frame.activity_ns, Cause::Decompression) == 50 * Ms);
    }
}
