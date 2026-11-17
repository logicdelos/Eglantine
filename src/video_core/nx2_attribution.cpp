// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The attribution recorder of milestone 0.5 (plan_0.5.md section 2, contracts_0.5.md slice B).
//
// Producers (any thread) append fixed-size events to a per-thread single-producer ring: Begin and
// End for a span, Closed for an already finished one, Link for a wait released by a producer. They
// take no lock and allocate nothing; a thread allocates once, when it registers. One consumer thread
// drains the rings into its own history, and resolves every host present (a frame) and the cold
// start intervals against that history:
//
//  * A frame (P(i-1), P(i)] is decomposed on the critical path of the thread that presented. On that
//    thread the innermost span wins, so nested time is subtracted from the parent. A wait that was
//    linked to a producer work id is replaced, for the time it overlaps the producer's spans, by
//    the producer's cause (recursively, through waits of the producer). An unlinked wait stays
//    Waiting and is counted as producer unknown; time with no span is Unknown. The result sums
//    exactly to the frame's duration. activity_ns is the clipped sum over all threads.
//  * A span open when a frame ends is clipped at the frame end. Its start was recorded, so the part
//    that falls into the next frame is charged there when it closes.
//  * Quiescence: a producer sets `busy` before it reads the clock for a Begin or End and clears it
//    after the event is published. The consumer waits (bounded) for every busy flag to clear after
//    it dequeued a present, then drains the rings, so every event stamped before the present is
//    visible when the frame is resolved.
//  * Rings and the history are bounded by SessionConfig::buffer_bytes. What does not fit is counted
//    (lost_events) and its time range marks every frame it touches incomplete, and only those
//    frames: loss episodes are kept sorted and merged, never widened to cover the session.
//  * Memory is bounded per interval, not by loss. Spans that ended before the last resolved time
//    are pruned as soon as the history is half full, and an interval that is still open when the
//    history is three quarters full is resolved in part (ResolveChunk): its decomposition so far
//    is kept as a carry, its finished spans are freed, and the present that ends the interval
//    adds the rest. Time is additive over disjoint windows, so the sum is the one a whole-interval
//    decomposition would give, except for a wait expansion that straddles the cut.
//
// The crash path (ReadCrashStatus) reads mirrored relaxed atomics only.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/nx2_spikes.h"

namespace VideoCommon::Nx2Spikes {

namespace Detail {
void ReadEventCounts(std::array<u64, static_cast<size_t>(Event::Count)>& counts);
void SetAttributionCounting(bool on);
size_t PeakRetainedHistoryBytes();
size_t PeakResolverScratchBytes();
size_t ResolverScratchAllowance();
u64 DrainedRingEvents();
u64 UndrainedRingEvents();
bool HoldConsumer(bool hold);
} // namespace Detail

namespace {

constexpr size_t EventCount = static_cast<size_t>(Event::Count);
using Counters = std::array<u64, EventCount>;
using CauseTimes = std::array<u64, CauseCount>;

constexpr size_t MaxThreads = 32;
constexpr size_t MaxPhases = 64;
constexpr size_t MaxQueueItems = 4096;
constexpr size_t MaxPendingLinks = 4096;
constexpr size_t MaxPendingEnds = 4096;
constexpr size_t MaxLossEpisodes = 1024;
constexpr size_t MaxCumulative = 32768;
constexpr size_t MaxWaitChain = 6;
constexpr size_t LinkSearchDepth = 4096;
constexpr u32 MaxLinkAttempts = 8;
constexpr u64 RetainNs = 2'000'000'000;
constexpr u64 OpenEnd = std::numeric_limits<u64>::max();
constexpr auto ConsumerPeriod = std::chrono::milliseconds{2};
constexpr auto QuiesceLimit = std::chrono::milliseconds{5};
/// Below this many history slots the session never resolves in part (tiny budgets of tests and
/// diagnostics): the decomposition costs more than the memory it would free.
constexpr size_t MinChunkHistory = 4096;
/// Loss episodes closer than this are one episode.
constexpr u64 LossMergeNs = 1'000'000;
/// A partial resolution stops this far before now, so that spans stamped before it are published.
constexpr u64 ChunkMarginNs = 1'000'000;

constexpr size_t CauseIdx(Cause cause) noexcept {
    return static_cast<size_t>(cause);
}

u64 Sub(u64 a, u64 b) noexcept {
    return a > b ? a - b : 0;
}

// ---------------------------------------------------------------------------------------------
// Producer side
// ---------------------------------------------------------------------------------------------

enum class EventType : u8 { Begin, End, Closed, Link };

struct RingEvent {
    u64 seq{};      ///< The span's sequence number (on the thread that began it)
    u64 begin_ns{}; ///< Begin, Closed: when it began. Link: when the wait was released
    u64 end_ns{};   ///< End, Closed
    u64 work{};     ///< Begin, Closed: the span's work. Link: the producer
    u64 amount{};
    u32 owner{}; ///< End, Link: registration id of the thread that began the span
    u32 flags{};
    EventType type{EventType::Begin};
    Cause cause{Cause::Unknown};
    DependencyKind kind{DependencyKind::Job};
};
static_assert(sizeof(RingEvent) <= 64);

struct ThreadBuf {
    u32 id{};
    std::array<char, 24> name{};
    std::unique_ptr<RingEvent[]> events;
    u64 mask{};
    // Producer: tail, its cache of head, span sequence. Consumer: head.
    alignas(64) std::atomic<u64> tail{};
    u64 head_cache{};
    u64 next_seq{1};
    alignas(64) std::atomic<u64> head{};
    alignas(64) std::atomic<u32> busy{};
    std::atomic<u32> open_count{};
    std::atomic<u64> lost{};
    /// Events published per EventType (producer only; the consumer reads them for diagnostics).
    std::array<std::atomic<u64>, 4> kinds{};
    /// Set by MarkGuestThread: this thread runs guest code (set by the producer, read at drain).
    std::atomic<bool> guest{};
    // Overflow range [min, max] of the events this thread could not record. Only the owning thread
    // writes it (compare-exchange, no lock); the consumer takes it with exchange.
    std::atomic<u64> loss_min{OpenEnd};
    std::atomic<u64> loss_max{0};
};

/// One item for the consumer: a host present or an accepted marker.
struct Item {
    bool marker{};
    u64 id{};
    u64 prev_id{}; ///< The qualifying present before this one (0 for the first), dropped or not
    u64 prev_ts{};
    u64 guest_frame_id{};
    u64 ts{};
    u32 root{}; ///< Registration id of the presenting thread
    u32 reasons{};
    PresentResult result{PresentResult::Success};
    Counters counters{};
};

struct SpanRec {
    u32 thread{};
    u64 seq{};
    u64 begin_ns{};
    u64 end_ns{};
    WorkId work{};
    u64 amount{};
    u32 flags{};
    Cause cause{Cause::Unknown};
    bool open{true};
    bool linked{};
    WorkId producer{};
    DependencyKind kind{DependencyKind::Job};
};

struct ThreadState {
    std::vector<SpanRec> open;
    std::vector<SpanRec> closed;
    bool guest{}; ///< Copied from the ring at each drain
};

struct PendingLink {
    u32 owner{};
    u64 seq{};
    WorkId producer{};
    DependencyKind kind{DependencyKind::Job};
    u32 attempts{};
};

/// A cross-thread End whose Begin has not been ingested yet.
struct PendingEnd {
    u32 owner{};
    u64 seq{};
    u64 end_ns{};
    u64 amount{};
    u32 flags{};
};

struct Leaf {
    u64 t0{};
    u64 t1{};
    Cause cause{Cause::Unknown};
    WorkId work{}; ///< The nearest enclosing work id on the thread (0 when none)
    const SpanRec* span{};
    u32 thread{};
    u64 span_end{};
};

/// One span that carries a work id, clipped to the window of a decomposition.
struct WorkSpan {
    WorkId work{};
    u32 thread{};
    u64 t0{};
    u64 t1{};
};

/// One begin or end of a span in the sweep that builds the leaves of a thread.
struct SweepEvent {
    u64 t;
    bool begin;
    u32 idx;
};

/// What one node of an ordered or hashed map costs here, an estimate: key, value, links, allocator.
constexpr size_t MapNodeBytes = 64;
/// What one bucket of the hashed work totals costs, an estimate (some libraries keep two pointers).
constexpr size_t BucketBytes = 2 * sizeof(void*);

/// How much of each kind of storage a decomposition holds at one moment. The peak the tests read
/// and the allowance per retained span are both worked out from counts like these by
/// ScratchBytes, so they cannot drift apart. The byte figures are estimates (MapNodeBytes,
/// BucketBytes), not allocator measurements.
struct ScratchCounts {
    size_t leaves{};       ///< Leaf slots of all threads
    size_t work_spans{};   ///< WorkSpan slots
    size_t sweep_spans{};  ///< Span pointers of the sweep that builds the leaves of a thread
    size_t sweep_events{}; ///< Its events
    size_t sweep_nodes{};  ///< Its active-map nodes
    size_t root_points{};  ///< Points of the root sweep
    size_t pieces{};       ///< Candidate pieces of the wait expansions that are running
    size_t wait_points{};  ///< Their sweep points
    size_t work_nodes{};   ///< Entries of the work totals
    size_t work_buckets{}; ///< Buckets of the work totals
};

constexpr size_t ScratchBytes(const ScratchCounts& c) {
    return c.leaves * sizeof(Leaf) + c.work_spans * sizeof(WorkSpan) +
           c.sweep_spans * sizeof(const SpanRec*) + c.sweep_events * sizeof(SweepEvent) +
           c.sweep_nodes * MapNodeBytes + c.root_points * sizeof(u64) + c.pieces * sizeof(Leaf) +
           c.wait_points * sizeof(u64) + c.work_nodes * MapNodeBytes +
           c.work_buckets * BucketBytes;
}

/// Storage that does not grow with the number of spans, as an upper estimate: the per-thread and
/// per-level vectors (at most 32 threads, 6 levels of a chain), the two extra sweep points of each
/// level, and the bucket array of the work totals while it is small (up to 512 buckets).
constexpr size_t ScratchFixedBytes = 16 * 1024;

/// The counts of one retained span in the worst case, in the two phases of a decomposition. A span
/// gives at most two leaves, one work span and two active-map nodes. While the leaves of a thread
/// are built they are joined by the sweep's pointer, two events and the two map nodes. While the
/// waits are resolved they are joined by the root sweep's points (two per leaf of a root, two
/// ends each), the candidate pieces (at most one per leaf) with their points (two per piece, two
/// ends each), and the work totals: at most two ids per span (its own and the one it waits for),
/// with two buckets per entry.
constexpr ScratchCounts SweepPhasePerSpan() {
    ScratchCounts c;
    c.leaves = 2;
    c.work_spans = 1;
    c.sweep_spans = 1;
    c.sweep_events = 2;
    c.sweep_nodes = 2;
    return c;
}
constexpr ScratchCounts WaitPhasePerSpan() {
    ScratchCounts c;
    c.leaves = 2;
    c.work_spans = 1;
    c.root_points = 4;
    c.pieces = 2;
    c.wait_points = 4;
    c.work_nodes = 2;
    c.work_buckets = 4;
    return c;
}
constexpr size_t ScratchBytesPerSpan =
    std::max(ScratchBytes(SweepPhasePerSpan()), ScratchBytes(WaitPhasePerSpan()));

/// The scratch a decomposition may hold when `spans` slots of span storage are retained.
constexpr size_t ScratchAllowance(size_t spans) {
    return spans * ScratchBytesPerSpan + ScratchFixedBytes;
}

/// What one slot of retained span storage costs against SessionConfig::buffer_bytes: the record and
/// the worst-case scratch of decomposing it, so rings plus history stay inside the one budget.
/// Slots are counted by capacity, not by use.
constexpr size_t HistoryBytesPerSpan = sizeof(SpanRec) + ScratchBytesPerSpan;
/// The first allocation of a span vector, in records.
constexpr size_t MinSpanCapacity = 16;

struct Decomp {
    CauseTimes cause{};
    CauseTimes activity{};
    CauseTimes inferred{}; ///< The part of `cause` that is an inference (UnknownReason::PresenterGap)
    u32 reasons{};
    bool complete{true};
    u64 producer_unknown_wait_ns{};
    std::unordered_map<WorkId, u64> work_ns;

    /// Adds the decomposition of the neighbouring part of the same interval.
    void Add(const Decomp& part) {
        for (size_t i = 0; i < CauseCount; ++i) {
            cause[i] += part.cause[i];
            activity[i] += part.activity[i];
            inferred[i] += part.inferred[i];
        }
        reasons |= part.reasons;
        complete = complete && part.complete;
        producer_unknown_wait_ns += part.producer_unknown_wait_ns;
        for (const auto& [work, ns] : part.work_ns) {
            work_ns[work] += ns;
        }
    }
};

struct CumulativeEntry {
    u64 ts{};
    CauseTimes causes{};
    bool complete{true};
};

/// The decomposition of one window of the history. Consumer thread only.
class Resolver {
public:
    Resolver(std::span<const ThreadState> threads, std::span<const std::pair<u64, u64>> losses,
             u64 start, u64 end, bool charge_presenter_gaps)
        : states{threads}, loss_episodes{losses}, s{start}, e{end},
          charge_gaps{charge_presenter_gaps} {}

    Decomp Run(std::span<const u32> roots) {
        for (const auto& [begin, finish] : loss_episodes) {
            if (finish > s && begin <= e) {
                out.reasons |= UnknownReason::LostSpans;
                out.complete = false;
                break;
            }
        }
        leaves.resize(states.size());
        size_t span_count{};
        for (const ThreadState& state : states) {
            span_count += state.open.size() + state.closed.size();
        }
        work_spans.reserve(span_count);
        for (size_t i = 0; i < states.size(); ++i) {
            BuildLeaves(static_cast<u32>(i + 1), states[i], leaves[i]);
            leaf_total += leaves[i].size();
            for (const Leaf& leaf : leaves[i]) {
                out.activity[CauseIdx(leaf.cause)] += leaf.t1 - leaf.t0;
            }
        }
        std::sort(work_spans.begin(), work_spans.end(), [](const WorkSpan& a, const WorkSpan& b) {
            if (a.work != b.work) {
                return a.work < b.work;
            }
            if (a.thread != b.thread) {
                return a.thread < b.thread;
            }
            return a.t0 < b.t0;
        });
        ResolveRoots(roots);
        u64 total{};
        for (const u64 value : out.cause) {
            total += value;
        }
        if (total < e - s) {
            out.cause[CauseIdx(Cause::Unknown)] += e - s - total;
            out.reasons |= UnknownReason::LostSpans;
            out.complete = false;
        }
        NoteScratch({});
        return std::move(out);
    }

    /// The most bytes of scratch (leaves, work spans, sweep and wait storage) held at once by Run.
    [[nodiscard]] size_t PeakScratchBytes() const {
        return peak_scratch;
    }

private:
    void BuildLeaves(u32 thread, const ThreadState& state, std::vector<Leaf>& result) {
        std::vector<const SpanRec*> spans;
        spans.reserve(state.open.size() + state.closed.size());
        const auto consider = [&](const SpanRec& rec) {
            const u64 finish = rec.open ? OpenEnd : rec.end_ns;
            if (rec.begin_ns >= e || finish <= s) {
                return;
            }
            if (std::max(rec.begin_ns, s) >= std::min(finish, e)) {
                return;
            }
            if ((rec.flags & SpanFlags::Incomplete) != 0) {
                out.complete = false;
            }
            spans.push_back(&rec);
            if (rec.work != 0) {
                work_spans.push_back(
                    {rec.work, thread, std::max(rec.begin_ns, s), std::min(finish, e)});
            }
        };
        for (const SpanRec& rec : state.open) {
            consider(rec);
        }
        for (const SpanRec& rec : state.closed) {
            consider(rec);
        }
        if (spans.empty()) {
            return;
        }
        using Ev = SweepEvent;
        std::vector<Ev> events;
        events.reserve(spans.size() * 2);
        for (size_t i = 0; i < spans.size(); ++i) {
            const SpanRec& rec = *spans[i];
            const u64 finish = rec.open ? OpenEnd : rec.end_ns;
            events.push_back({std::max(rec.begin_ns, s), true, static_cast<u32>(i)});
            events.push_back({std::min(finish, e), false, static_cast<u32>(i)});
        }
        std::sort(events.begin(), events.end(), [](const Ev& a, const Ev& b) {
            if (a.t != b.t) {
                return a.t < b.t;
            }
            if (a.begin != b.begin) {
                return !a.begin; // ends before begins at the same time
            }
            return a.idx < b.idx;
        });
        // Innermost is last: the latest begin; at equal begins the one that ends first.
        using Key = std::array<u64, 3>;
        const auto key_of = [](const SpanRec& rec) {
            return Key{rec.begin_ns, OpenEnd - (rec.open ? OpenEnd : rec.end_ns), rec.seq};
        };
        std::map<Key, u32> active;
        std::map<Key, u32> active_work; // the same, only the spans that carry a work id
        size_t most_active = 0;
        result.reserve(events.size());
        u64 cur = s;
        for (size_t i = 0; i < events.size();) {
            const u64 t = events[i].t;
            if (t > cur && !active.empty()) {
                const SpanRec* top = spans[active.rbegin()->second];
                const WorkId work =
                    active_work.empty() ? WorkId{0} : spans[active_work.rbegin()->second]->work;
                if (!result.empty() && result.back().span == top && result.back().work == work &&
                    result.back().t1 == cur) {
                    result.back().t1 = t;
                } else {
                    result.push_back({cur, t, top->cause, work, top, thread,
                                      top->open ? OpenEnd : top->end_ns});
                }
            }
            for (; i < events.size() && events[i].t == t; ++i) {
                const SpanRec& rec = *spans[events[i].idx];
                if (events[i].begin) {
                    active.emplace(key_of(rec), events[i].idx);
                    if (rec.work != 0) {
                        active_work.emplace(key_of(rec), events[i].idx);
                    }
                } else {
                    active.erase(key_of(rec));
                    active_work.erase(key_of(rec));
                }
            }
            most_active = std::max(most_active, active.size() + active_work.size());
            cur = t;
        }
        ScratchCounts sweep;
        sweep.sweep_spans = spans.capacity();
        sweep.sweep_events = events.capacity();
        sweep.sweep_nodes = most_active;
        NoteScratch(sweep);
    }

    void Charge(Cause cause, u64 from, u64 to, WorkId work) {
        out.cause[CauseIdx(cause)] += to - from;
        if (inferring) {
            out.inferred[CauseIdx(cause)] += to - from;
        }
        if (work != 0) {
            out.work_ns[work] += to - from;
        }
    }

    void ResolveRoots(std::span<const u32> roots) {
        root_ids.assign(roots.begin(), roots.end());
        std::vector<const std::vector<Leaf>*> lists;
        for (const u32 root : roots) {
            if (root >= 1 && root <= leaves.size()) {
                lists.push_back(&leaves[root - 1]);
            }
        }
        if (lists.empty()) {
            if (e > s) {
                Charge(Cause::Unknown, s, e, 0);
                out.reasons |= UnknownReason::LostSpans;
                out.complete = false;
            }
            return;
        }
        std::vector<u64> points;
        size_t reserved = 2;
        for (const auto* list : lists) {
            reserved += 2 * list->size();
        }
        points.reserve(reserved);
        root_points = points.capacity();
        points.push_back(s);
        points.push_back(e);
        for (const auto* list : lists) {
            for (const Leaf& leaf : *list) {
                points.push_back(leaf.t0);
                points.push_back(leaf.t1);
            }
        }
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        std::vector<size_t> cursor(lists.size(), 0);
        for (size_t p = 0; p + 1 < points.size(); ++p) {
            const u64 x = points[p];
            const u64 y = points[p + 1];
            const Leaf* best = nullptr;
            for (size_t k = 0; k < lists.size(); ++k) {
                const std::vector<Leaf>& list = *lists[k];
                while (cursor[k] < list.size() && list[cursor[k]].t1 <= x) {
                    ++cursor[k];
                }
                if (cursor[k] >= list.size() || list[cursor[k]].t0 > x) {
                    continue;
                }
                const Leaf* leaf = &list[cursor[k]];
                if (best == nullptr || (best->cause == Cause::Waiting && leaf->cause != Cause::Waiting)) {
                    best = leaf;
                }
            }
            if (best == nullptr) {
                ExpandRootGap(x, y);
            } else if (best->cause == Cause::Waiting) {
                visited.clear();
                ExpandWait(x, y, *best);
            } else {
                Charge(best->cause, x, y, best->work);
            }
        }
        NoteScratch({}); // the work totals have grown while the points were held
        root_points = 0;
    }

    /// Charges [x, y) of a wait: the part covered by the spans of the work it was linked to goes to
    /// their causes, the rest stays Waiting.
    void ExpandWait(u64 x, u64 y, const Leaf& wait) {
        const SpanRec& span = *wait.span;
        if (!span.linked) {
            if ((span.flags & SpanFlags::GuestWait) != 0) {
                ExpandGuestWait(x, y, wait);
                return;
            }
            Charge(Cause::Waiting, x, y, 0);
            out.reasons |= UnknownReason::ProducerUnknownWait;
            out.producer_unknown_wait_ns += y - x;
            return;
        }
        const WorkId producer = span.producer;
        if (visited.size() >= MaxWaitChain ||
            std::find(visited.begin(), visited.end(), producer) != visited.end()) {
            Charge(Cause::Waiting, x, y, producer);
            out.reasons |= UnknownReason::AmbiguousDependency;
            return;
        }
        size_t count{};
        ForEachOwned(producer, wait.thread, x, y, [&count](const Leaf&) { ++count; });
        if (count == 0) {
            Charge(Cause::Waiting, x, y, producer);
            return;
        }
        if (live_pieces + count > leaf_total) {
            // The producers of the chain would hold more candidate pieces than there are leaves:
            // not resolved, kept as waiting and reported.
            Charge(Cause::Waiting, x, y, producer);
            out.reasons |= UnknownReason::AmbiguousDependency;
            out.complete = false;
            return;
        }
        std::vector<Leaf> cands;
        cands.reserve(count);
        std::vector<size_t> group_first; // where the pieces of each thread begin
        ForEachOwned(producer, wait.thread, x, y, [&](const Leaf& piece) {
            if (cands.empty() || cands.back().thread != piece.thread) {
                group_first.push_back(cands.size());
            }
            cands.push_back(piece);
        });
        std::vector<u64> points;
        points.reserve(2 * count + 2);
        points.push_back(x);
        points.push_back(y);
        for (const Leaf& leaf : cands) {
            points.push_back(leaf.t0);
            points.push_back(leaf.t1);
        }
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        live_pieces += count;
        wait_pieces += cands.capacity();
        wait_points += points.capacity();
        NoteScratch({});
        // The pieces of one thread are sorted and do not overlap, so one cursor per thread finds
        // the piece covering a segment.
        std::vector<size_t> cursor = group_first;
        visited.push_back(producer);
        for (size_t p = 0; p + 1 < points.size(); ++p) {
            const u64 a = points[p];
            const u64 b = points[p + 1];
            const Leaf* best = nullptr;
            for (size_t g = 0; g < group_first.size(); ++g) {
                const size_t last = g + 1 < group_first.size() ? group_first[g + 1] : cands.size();
                while (cursor[g] < last && cands[cursor[g]].t1 <= a) {
                    ++cursor[g];
                }
                if (cursor[g] >= last || cands[cursor[g]].t0 > a) {
                    continue;
                }
                const Leaf& leaf = cands[cursor[g]];
                // The wait ends when the last producer span ends: that one is the critical one.
                if (best == nullptr || leaf.span_end > best->span_end ||
                    (leaf.span_end == best->span_end &&
                     (leaf.thread < best->thread ||
                      (leaf.thread == best->thread && leaf.span->seq < best->span->seq)))) {
                    best = &leaf;
                }
            }
            if (best == nullptr) {
                Charge(Cause::Waiting, a, b, producer);
            } else if (best->cause == Cause::Waiting) {
                ExpandWait(a, b, *best);
            } else {
                Charge(best->cause, a, b, best->work);
            }
        }
        visited.pop_back();
        NoteScratch({}); // the work totals have grown while this level held its candidates
        live_pieces -= count;
        wait_pieces -= cands.capacity();
        wait_points -= points.capacity();
    }

    /// Charges [x, y) of a wait for the guest (SpanFlags::GuestWait) by what the guest threads were
    /// doing meanwhile: where one of them is in a cause that is not waiting, the time goes to that
    /// cause (when several are, the span that ends last is the critical one, as for a linked wait);
    /// where none is, the guest was idle or waiting itself and the time stays Waiting.
    void ExpandGuestWait(u64 x, u64 y, const Leaf& wait) {
        ExpandByActivity(
            x, y,
            [this, &wait](size_t t) {
                return states[t].guest && static_cast<u32>(t + 1) != wait.thread;
            },
            [this](u64 from, u64 to) {
                Charge(Cause::Waiting, from, to, 0);
                out.reasons |= UnknownReason::ProducerUnknownWait;
                out.producer_unknown_wait_ns += to - from;
            });
    }

    /// Charges [x, y) of a stretch where the presenting thread has no span (its own work between
    /// the waits it records, waits shorter than the recording threshold) by what every other thread
    /// was doing meanwhile, with the rule of a guest wait: where one is in a cause that is not
    /// waiting, the time goes to the cause of the span that ends last; where none is, it stays
    /// Waiting (the presenter's own time, not a cause of its own). It is a bounded guess, so the
    /// frame carries PresenterGap. Only when no thread recorded anything in the window is the time
    /// Unknown: there is nothing to attribute it to.
    void ExpandRootGap(u64 x, u64 y) {
        if (!charge_gaps || leaf_total == 0) {
            Charge(Cause::Unknown, x, y, 0);
            out.reasons |= UnknownReason::UnlinkedGuest;
            return;
        }
        out.reasons |= UnknownReason::PresenterGap;
        inferring = true;
        ExpandByActivity(
            x, y,
            [this](size_t t) {
                return std::find(root_ids.begin(), root_ids.end(), static_cast<u32>(t + 1)) ==
                       root_ids.end();
            },
            [this](u64 from, u64 to) { Charge(Cause::Waiting, from, to, 0); });
        inferring = false;
    }

    /// Charges [x, y) by the leaves of the threads `include` accepts that are not waiting or
    /// Unknown: the span that ends last is the critical one where several overlap, and `uncovered`
    /// takes the pieces no such leaf covers.
    template <typename Include, typename Uncovered>
    void ExpandByActivity(u64 x, u64 y, Include&& include, Uncovered&& uncovered) {
        const auto each = [&](auto&& emit) {
            for (size_t t = 0; t < leaves.size(); ++t) {
                if (!include(t)) {
                    continue;
                }
                const std::vector<Leaf>& list = leaves[t];
                auto it = std::partition_point(list.begin(), list.end(),
                                               [x](const Leaf& leaf) { return leaf.t1 <= x; });
                for (; it != list.end() && it->t0 < y; ++it) {
                    if (it->cause == Cause::Waiting || it->cause == Cause::Unknown) {
                        continue;
                    }
                    Leaf piece = *it;
                    piece.t0 = std::max(it->t0, x);
                    piece.t1 = std::min(it->t1, y);
                    emit(piece);
                }
            }
        };
        size_t count{};
        each([&count](const Leaf&) { ++count; });
        if (count == 0) {
            uncovered(x, y);
            return;
        }
        std::vector<Leaf> cands;
        cands.reserve(count);
        std::vector<size_t> group_first; // where the pieces of each thread begin
        each([&](const Leaf& piece) {
            if (cands.empty() || cands.back().thread != piece.thread) {
                group_first.push_back(cands.size());
            }
            cands.push_back(piece);
        });
        std::vector<u64> points;
        points.reserve(2 * count + 2);
        points.push_back(x);
        points.push_back(y);
        for (const Leaf& leaf : cands) {
            points.push_back(leaf.t0);
            points.push_back(leaf.t1);
        }
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        live_pieces += count;
        wait_pieces += cands.capacity();
        wait_points += points.capacity();
        NoteScratch({});
        std::vector<size_t> cursor = group_first;
        for (size_t p = 0; p + 1 < points.size(); ++p) {
            const u64 a = points[p];
            const u64 b = points[p + 1];
            const Leaf* best = nullptr;
            for (size_t g = 0; g < group_first.size(); ++g) {
                const size_t last = g + 1 < group_first.size() ? group_first[g + 1] : cands.size();
                while (cursor[g] < last && cands[cursor[g]].t1 <= a) {
                    ++cursor[g];
                }
                if (cursor[g] >= last || cands[cursor[g]].t0 > a) {
                    continue;
                }
                const Leaf& leaf = cands[cursor[g]];
                if (best == nullptr || leaf.span_end > best->span_end ||
                    (leaf.span_end == best->span_end &&
                     (leaf.thread < best->thread ||
                      (leaf.thread == best->thread && leaf.span->seq < best->span->seq)))) {
                    best = &leaf;
                }
            }
            if (best == nullptr) {
                uncovered(a, b);
            } else {
                Charge(best->cause, a, b, best->work);
            }
        }
        NoteScratch({}); // the work totals have grown while the candidates were held
        live_pieces -= count;
        wait_pieces -= cands.capacity();
        wait_points -= points.capacity();
    }

    /// Calls `emit` with every piece of a leaf of another thread than `skip_thread` that lies inside
    /// a span carrying `producer` and overlaps [x, y), clipped to the span and to [x, y). The spans of
    /// one thread are merged first, so no stretch of a leaf is emitted twice. Pieces come thread by
    /// thread, each thread's in time order.
    template <typename F>
    void ForEachOwned(WorkId producer, u32 skip_thread, u64 x, u64 y, F&& emit) const {
        const auto first = std::lower_bound(
            work_spans.begin(), work_spans.end(), producer,
            [](const WorkSpan& item, WorkId id) { return item.work < id; });
        u32 thread{};
        u64 from{};
        u64 to{};
        const auto flush = [&] {
            if (thread == 0) {
                return;
            }
            const std::vector<Leaf>& list = leaves[thread - 1];
            auto it = std::partition_point(list.begin(), list.end(),
                                           [from](const Leaf& leaf) { return leaf.t1 <= from; });
            for (; it != list.end() && it->t0 < to; ++it) {
                Leaf piece = *it;
                piece.t0 = std::max(it->t0, from);
                piece.t1 = std::min(it->t1, to);
                emit(piece);
            }
            thread = 0;
        };
        for (auto it = first; it != work_spans.end() && it->work == producer; ++it) {
            if (it->thread == skip_thread) {
                continue;
            }
            const u64 lo = std::max(it->t0, x);
            const u64 hi = std::min(it->t1, y);
            if (lo >= hi) {
                continue;
            }
            if (thread == it->thread && lo <= to) {
                to = std::max(to, hi);
                continue;
            }
            flush();
            thread = it->thread;
            from = lo;
            to = hi;
        }
        flush();
    }

    /// Notes the storage the resolver holds now: everything that is alive at the same time (the
    /// leaves, the work spans, the root sweep's points, the candidates and points of every wait
    /// level that is running, the work totals so far), and `c`, the sweep of one thread when that
    /// is what is running.
    void NoteScratch(ScratchCounts c) {
        c.work_spans = work_spans.capacity();
        for (const std::vector<Leaf>& list : leaves) {
            c.leaves += list.capacity();
        }
        c.root_points = root_points;
        c.pieces = wait_pieces;
        c.wait_points = wait_points;
        c.work_nodes = out.work_ns.size();
        c.work_buckets = out.work_ns.bucket_count();
        peak_scratch = std::max(peak_scratch, ScratchBytes(c) + ScratchFixedBytes);
    }

    std::span<const ThreadState> states;
    std::span<const std::pair<u64, u64>> loss_episodes;
    u64 s;
    u64 e;
    std::vector<u32> root_ids; ///< The roots of the sweep that is running (ResolveRoots)
    bool charge_gaps; ///< SessionConfig::charge_presenter_gaps
    bool inferring{}; ///< Charges are inferences (ExpandRootGap is running)
    std::vector<std::vector<Leaf>> leaves;
    /// The spans of the window that carry a work id, clipped to it, sorted by (work, thread, begin).
    /// A leaf belongs to a work when it lies inside one of that work's spans on its thread, which is
    /// how a producer owns the time of the spans nested in it that carry other work ids.
    std::vector<WorkSpan> work_spans;
    size_t leaf_total{};  ///< The number of leaves of all threads: the cap on live wait candidates
    size_t live_pieces{}; ///< Candidate pieces held by the wait expansions that are running
    size_t wait_pieces{}; ///< Capacity of the candidate vectors of the waits that are running
    size_t wait_points{}; ///< and of their point vectors
    size_t root_points{}; ///< Capacity of the root sweep's point vector while it is held
    size_t peak_scratch{};
    std::vector<WorkId> visited;
    Decomp out;
};

/// The part of an interval that was resolved before its end, and where the rest begins.
struct Joined {
    Decomp part;
    u64 from{};
    bool broken{}; ///< The resolved part did not fit the interval: it is Unknown and incomplete
};

struct Session {
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Producer side
    u32 generation{};
    SessionConfig config;
    u64 start_ns{};
    size_t ring_capacity{};
    std::mutex reg_mutex;
    std::array<std::unique_ptr<ThreadBuf>, MaxThreads> threads;
    std::atomic<u32> thread_count{};
    std::atomic<u64> lost_unregistered{};
    std::atomic<u64> next_present{1};
    std::atomic<u64> first_present_ns{};
    std::atomic<u64> last_present_ns{};
    std::atomic<u64> rejected{};
    std::atomic<u64> boundaries_dropped{};
    std::atomic<u32> pending_reasons{}; ///< NoteUnknownReason bits for the open frame
    std::atomic<bool> marker_accepted{};
    std::atomic<u64> process_created_ns{};
    std::atomic<u64> process_created_uncertainty_ns{};
    std::atomic<u64> lost_consumer{}; ///< Late events, history overflow, dropped links
    std::atomic<u64> sink_errors{};
    std::atomic<bool> ended{};
    // The crash status mirror (relaxed atomics, written by the consumer)
    std::atomic<u64> mirror_frames{};
    std::atomic<u64> mirror_long_frames{};
    std::atomic<u64> mirror_last_long_id{};
    std::atomic<u64> mirror_last_long_ns{};
    std::atomic<bool> mirror_complete{true};

    // Queue
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<Item> queue;
    u64 last_actual_ts{}; ///< Under queue_mutex: the last qualifying present, kept or dropped
    u64 last_actual_id{};
    bool stop{};
    std::atomic<bool> abandon{};
    std::atomic<bool> hold_consumer{}; ///< Tests: park the consumer between its passes
    std::atomic<bool> consumer_held{}; ///< The consumer is parked
    std::thread consumer;

    // Snapshot state, under stats_mutex
    std::mutex stats_mutex;
    AttributionSnapshot snap;
    std::vector<u32> boot_roots;
    u64 sum_unknown_ns{};
    u64 sum_duration_ns{};
    u64 primary_unknown{};
    u64 incomplete_long_frames{};
    AttributionSnapshot final_snap;

    // Consumer-owned
    std::array<ThreadState, MaxThreads> states;
    std::vector<PendingLink> pending_links;
    std::vector<PendingEnd> pending_ends;
    std::vector<std::pair<u64, u64>> losses;
    size_t history{}; ///< Slots of span storage owned: the capacities of every open/closed vector
    std::atomic<size_t> peak_retained_bytes{}; ///< Most bytes of span storage held at once
    mutable std::atomic<size_t> peak_scratch_bytes{}; ///< Most resolver scratch held at once
    size_t history_cap{};
    u64 seen_lost{};
    u64 seen_unregistered{};
    bool logged_loss{};
    bool quiesce_timeout{};
    bool have_prev{};
    u64 prev_ts{};
    u64 prev_id{};
    Counters prev_counters{};
    u64 last_resolved_ts{};
    u64 pruned_through{};
    u32 last_root{};
    u64 anchor{};
    bool interactive_done{};
    CauseTimes cum_causes{};
    bool cum_complete{true};
    std::vector<CumulativeEntry> cum;
    size_t chunk_slots{};  ///< History use that makes the open interval resolve in part (0: never)
    size_t retain_slots{}; ///< History use from which spans are pruned at once (0: after RetainNs)
    Decomp carry;          ///< The decomposition of (carry_from, carry_through]
    u64 carry_from{};
    u64 carry_through{}; ///< 0: nothing of the open interval is resolved yet
    u64 lost_history{};  ///< Of lost_consumer: records the history had no room for
    u64 lost_late{};     ///< Of lost_consumer: spans that ended before a resolved time
    /// The presents the consumer took from the queue and has not processed (consumer thread only).
    /// A pass that has to free history processes the ones whose events it has all taken.
    std::deque<Item> work;
    bool relief_items{true}; ///< False while the session is finalized: open spans are marked first

    void StartConsumer();
    void StopConsumer();
    void ConsumerMain();
    void DrainAll();
    bool DrainThread(u32 id, ThreadBuf& buf, u64 tail, u64 limit);
    void RelieveHistory(u64 stamp, const std::array<u64, MaxThreads>& tails, u32 count);
    bool ResolveInPart(u64 through);
    void HoldForTest();
    void ApplyLinks();
    bool CloseSpan(const PendingEnd& end);
    void ApplyEnds();
    void DropPendingEnds();
    void SkipLostInterval(u64 until);
    void TakeLosses();
    void AddLoss(u64 begin, u64 end);
    void Quiesce();
    void MarkOpenIncomplete();
    void Prune(bool force = false);
    bool Reserve(std::vector<SpanRec>& spans);
    void Trim(std::vector<SpanRec>& spans);
    void NoteRetained();
    void Process(const Item& item);
    void ResolveBoundary(const Item& item);
    void ResolveStartup(u64 first_present);
    void ResolveMarker(const Item& item);
    SpanRec* FindSpan(u32 owner, u64 seq);
    Decomp Decompose(u64 s, u64 e, std::span<const u32> roots) const;
    u64 TotalLost() const;
    AttributionSnapshot Snapshot();
    void Mirror();
    void MaybeChunk();
    bool LostBetween(u64 s, u64 e) const;
    Joined TakeCarry(u64 s, u64 e);
    void LogDiagnostics() const;
};

std::atomic<Session*> g_session{nullptr};
std::mutex g_lifecycle_mutex;
std::unique_ptr<Session> g_owned;
u32 g_generation{};
std::atomic<u64> g_next_work{1};
std::atomic<u64> g_pending_created{};
std::atomic<u64> g_pending_uncertainty{};
std::mutex g_sink_mutex;
LongFrameSink g_sink{};

struct TlsCache {
    u32 generation{};
    ThreadBuf* buf{};
};
thread_local TlsCache tls;

Session* ActiveSession() noexcept {
    if (!Detail::attribution_enabled.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return g_session.load(std::memory_order_acquire);
}

ThreadBuf* Register(Session& s, std::string_view name) {
    std::scoped_lock lock{s.reg_mutex};
    if (tls.generation == s.generation) {
        return tls.buf;
    }
    const u32 index = s.thread_count.load(std::memory_order_relaxed);
    tls.generation = s.generation;
    if (index >= MaxThreads) {
        tls.buf = nullptr;
        return nullptr;
    }
    auto buf = std::make_unique<ThreadBuf>();
    buf->id = index + 1;
    const size_t count = std::min(name.size(), buf->name.size() - 1);
    std::copy_n(name.begin(), count, buf->name.begin());
    buf->events = std::make_unique<RingEvent[]>(s.ring_capacity);
    buf->mask = s.ring_capacity - 1;
    tls.buf = buf.get();
    s.threads[index] = std::move(buf);
    s.thread_count.store(index + 1, std::memory_order_release);
    return tls.buf;
}

ThreadBuf* ThisThread(Session& s, std::string_view name = "unnamed") {
    if (tls.generation == s.generation) {
        return tls.buf;
    }
    return Register(s, name);
}

bool Push(ThreadBuf& t, const RingEvent& event, u64 reserve) {
    const u64 tail = t.tail.load(std::memory_order_relaxed);
    const u64 capacity = t.mask + 1;
    if (tail - t.head_cache + 1 + reserve > capacity) {
        t.head_cache = t.head.load(std::memory_order_acquire);
        if (tail - t.head_cache + 1 + reserve > capacity) {
            return false;
        }
    }
    t.events[tail & t.mask] = event;
    std::atomic<u64>& kind = t.kinds[static_cast<size_t>(event.type)];
    kind.store(kind.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    t.tail.store(tail + 1, std::memory_order_release);
    return true;
}

void NoteLoss(ThreadBuf& t, u64 begin, u64 end) {
    t.lost.fetch_add(1, std::memory_order_relaxed);
    u64 current = t.loss_min.load(std::memory_order_relaxed);
    while (begin < current &&
           !t.loss_min.compare_exchange_weak(current, begin, std::memory_order_relaxed)) {
    }
    current = t.loss_max.load(std::memory_order_relaxed);
    while (end > current &&
           !t.loss_max.compare_exchange_weak(current, end, std::memory_order_relaxed)) {
    }
}

/// A thread that has no ring (the table is full): nothing to record into, only the loss.
void NoteUnregisteredLoss(Session& s) {
    s.lost_unregistered.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// Consumer side
// ---------------------------------------------------------------------------------------------

Session::~Session() {
    if (consumer.joinable()) {
        abandon.store(true, std::memory_order_relaxed);
        StopConsumer();
    }
}

void Session::StartConsumer() {
    consumer = std::thread([this] { ConsumerMain(); });
}

void Session::StopConsumer() {
    {
        std::scoped_lock lock{queue_mutex};
        stop = true;
    }
    queue_cv.notify_one();
    if (consumer.joinable()) {
        consumer.join();
    }
}

void Session::ConsumerMain() {
    bool finalized = false;
    for (;;) {
        HoldForTest();
        bool stopping{};
        {
            std::unique_lock lock{queue_mutex};
            if (!stop && queue.empty()) {
                queue_cv.wait_for(lock, ConsumerPeriod, [this] { return stop || !queue.empty(); });
            }
            stopping = stop;
            work.swap(queue);
        }
        if (abandon.load(std::memory_order_relaxed)) {
            return;
        }
        const bool had_items = !work.empty();
        if (stopping && !finalized) {
            finalized = true;
            relief_items = false;
            Quiesce();
            DrainAll();
            DropPendingEnds(); // their begins never came: counted, not hidden
            MarkOpenIncomplete();
            relief_items = true;
        } else {
            if (had_items) {
                Quiesce(); // what began before the presents is published before the pass begins
            }
            DrainAll();
        }
        while (!work.empty()) {
            Quiesce();
            DrainAll();
            if (work.empty()) {
                break; // the pass processed them
            }
            Process(work.front());
            work.pop_front();
        }
        if (!stopping) {
            MaybeChunk();
        }
        if (stopping && !had_items) {
            return;
        }
    }
}

void Session::Quiesce() {
    const auto deadline = std::chrono::steady_clock::now() + QuiesceLimit;
    const u32 count = thread_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        while (threads[i]->busy.load(std::memory_order_acquire) != 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                quiesce_timeout = true;
                lost_consumer.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            std::this_thread::yield();
        }
    }
}

void Session::AddLoss(u64 begin, u64 end) {
    end = std::max(begin, end);
    // The episodes are sorted and apart: one within LossMergeNs of another is merged with it, so
    // an episode covers only the time its losses fell in and a frame is marked incomplete only
    // when an episode touches it.
    using Episode = std::pair<u64, u64>;
    const auto first = std::lower_bound(
        losses.begin(), losses.end(), begin,
        [](const Episode& episode, u64 value) { return episode.second + LossMergeNs < value; });
    auto last = first;
    while (last != losses.end() && last->first <= end + LossMergeNs) {
        ++last;
    }
    if (first == last) {
        losses.insert(first, Episode{begin, end});
    } else {
        const u64 low = std::min(begin, first->first);
        const u64 high = std::max(end, (last - 1)->second);
        const auto merged = losses.erase(first + 1, last) - 1;
        *merged = Episode{low, high};
    }
    if (losses.size() > MaxLossEpisodes) {
        // Merge the two closest, which widens the episodes by the least.
        size_t best = 0;
        u64 narrowest = ~u64{};
        for (size_t i = 0; i + 1 < losses.size(); ++i) {
            const u64 gap = losses[i + 1].first - losses[i].second;
            if (gap < narrowest) {
                narrowest = gap;
                best = i;
            }
        }
        losses[best].second = losses[best + 1].second;
        losses.erase(losses.begin() + static_cast<std::ptrdiff_t>(best) + 1);
    }
}

void Session::TakeLosses() {
    const u32 count = thread_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        ThreadBuf& buf = *threads[i];
        const u64 low = buf.loss_min.exchange(OpenEnd, std::memory_order_relaxed);
        const u64 high = buf.loss_max.exchange(0, std::memory_order_relaxed);
        if (low != OpenEnd || high != 0) {
            const u64 begin = low != OpenEnd ? low : high;
            AddLoss(begin, std::max(begin, high));
        }
    }
    const u64 lost = TotalLost();
    if (lost > seen_lost && !logged_loss) {
        logged_loss = true;
        LOG_WARNING(HW_GPU,
                    "Nx2Attribution lost events (buffer of {} bytes): long frame records are "
                    "incomplete",
                    config.buffer_bytes);
    }
    seen_lost = lost;
}

SpanRec* Session::FindSpan(u32 owner, u64 seq) {
    if (owner < 1 || owner > thread_count.load(std::memory_order_acquire)) {
        return nullptr;
    }
    ThreadState& state = states[owner - 1];
    for (auto it = state.open.rbegin(); it != state.open.rend(); ++it) {
        if (it->seq == seq) {
            return &*it;
        }
    }
    size_t searched = 0;
    for (auto it = state.closed.rbegin(); it != state.closed.rend() && searched < LinkSearchDepth;
         ++it, ++searched) {
        if (it->seq == seq) {
            return &*it;
        }
    }
    return nullptr;
}

bool Session::CloseSpan(const PendingEnd& end) {
    if (end.owner < 1 || end.owner > thread_count.load(std::memory_order_acquire)) {
        return false;
    }
    ThreadState& owner = states[end.owner - 1];
    const auto it = std::find_if(owner.open.begin(), owner.open.end(),
                                 [&](const SpanRec& r) { return r.seq == end.seq; });
    if (it == owner.open.end()) {
        return false;
    }
    SpanRec rec = *it;
    owner.open.erase(it);
    rec.open = false;
    rec.end_ns = std::max(end.end_ns, rec.begin_ns);
    rec.amount += end.amount;
    rec.flags |= end.flags;
    if (!Reserve(owner.closed)) {
        // The finished record does not fit: lost like a span refused at its Begin.
        lost_consumer.fetch_add(1, std::memory_order_relaxed);
        ++lost_history;
        AddLoss(rec.begin_ns, rec.end_ns);
        return true;
    }
    if (last_resolved_ts != 0 && rec.end_ns <= last_resolved_ts) {
        lost_consumer.fetch_add(1, std::memory_order_relaxed); // charged to its frame as open
        ++lost_late;
    }
    owner.closed.push_back(rec);
    return true;
}

/// Retries the ends whose Begin sits in a ring that was drained later than the ender's.
void Session::ApplyEnds() {
    std::erase_if(pending_ends, [this](const PendingEnd& end) { return CloseSpan(end); });
}

void Session::DropPendingEnds() {
    lost_consumer.fetch_add(pending_ends.size(), std::memory_order_relaxed);
    pending_ends.clear();
}

/// Takes the events of one ring from its head up to `tail` (a snapshot of the pass), at most
/// `limit` of them, and says whether events up to `tail` are left.
bool Session::DrainThread(u32 id, ThreadBuf& buf, u64 tail, u64 limit) {
    ThreadState& state = states[id - 1];
    state.guest = buf.guest.load(std::memory_order_relaxed);
    const u64 head = buf.head.load(std::memory_order_relaxed);
    const u64 stop_at = tail - head > limit ? head + limit : tail;
    for (u64 i = head; i < stop_at; ++i) {
        const RingEvent& event = buf.events[i & buf.mask];
        switch (event.type) {
        case EventType::Begin: {
            if (!Reserve(state.open)) {
                lost_consumer.fetch_add(1, std::memory_order_relaxed);
                ++lost_history;
                AddLoss(event.begin_ns, event.begin_ns);
                break;
            }
            SpanRec rec;
            rec.thread = id;
            rec.seq = event.seq;
            rec.begin_ns = event.begin_ns;
            rec.work = event.work;
            rec.amount = event.amount;
            rec.cause = event.cause;
            state.open.push_back(rec);
            break;
        }
        case EventType::End: {
            const PendingEnd end{event.owner, event.seq, event.end_ns, event.amount, event.flags};
            if (CloseSpan(end)) {
                break;
            }
            // A Begin of this very ring is always ingested before its End, so a miss there is a
            // Begin the history refused (counted already). Another registration's Begin may only
            // be drained after this ring: keep the end until it arrives.
            if (event.owner != id) {
                if (pending_ends.size() >= MaxPendingEnds) {
                    pending_ends.erase(pending_ends.begin());
                    lost_consumer.fetch_add(1, std::memory_order_relaxed);
                }
                pending_ends.push_back(end);
            }
            break;
        }
        case EventType::Closed: {
            if (!Reserve(state.closed)) {
                lost_consumer.fetch_add(1, std::memory_order_relaxed);
                ++lost_history;
                AddLoss(event.begin_ns, event.end_ns);
                break;
            }
            if (last_resolved_ts != 0 && event.end_ns <= last_resolved_ts) {
                // It ended before a frame that is already resolved: that frame missed it.
                lost_consumer.fetch_add(1, std::memory_order_relaxed);
                ++lost_late;
                AddLoss(event.begin_ns, event.end_ns);
                break;
            }
            SpanRec rec;
            rec.thread = id;
            rec.seq = event.seq;
            rec.begin_ns = event.begin_ns;
            rec.end_ns = std::max(event.end_ns, event.begin_ns);
            rec.work = event.work;
            rec.amount = event.amount;
            rec.flags = event.flags;
            rec.cause = event.cause;
            rec.open = false;
            state.closed.push_back(rec);
            break;
        }
        case EventType::Link: {
            if (pending_links.size() >= MaxPendingLinks) {
                pending_links.erase(pending_links.begin());
                lost_consumer.fetch_add(1, std::memory_order_relaxed);
            }
            pending_links.push_back({event.owner, event.seq, event.work, event.kind, 0});
            break;
        }
        }
    }
    buf.head.store(stop_at, std::memory_order_release);
    return stop_at < tail;
}

void Session::ApplyLinks() {
    for (size_t i = 0; i < pending_links.size();) {
        PendingLink& link = pending_links[i];
        if (SpanRec* rec = FindSpan(link.owner, link.seq)) {
            rec->linked = true;
            rec->producer = link.producer;
            rec->kind = link.kind;
        } else if (++link.attempts < MaxLinkAttempts) {
            ++i;
            continue;
        }
        pending_links[i] = pending_links.back();
        pending_links.pop_back();
    }
}

/// Takes everything the rings held when it began. With a history that can resolve in part it takes
/// the rings in rounds of a few events each, oldest first on every ring, and frees history between
/// the rounds (RelieveHistory): a ring that delivers more at once than the history has room for is
/// not lost, it is taken in as many rounds as it needs.
void Session::DrainAll() {
    const u64 stamp = NowNs();
    const u32 count = thread_count.load(std::memory_order_acquire);
    std::array<u64, MaxThreads> tails{};
    for (u32 i = 0; i < count; ++i) {
        tails[i] = threads[i]->tail.load(std::memory_order_acquire);
    }
    u64 round = std::numeric_limits<u64>::max();
    if (chunk_slots != 0) {
        round = std::clamp<u64>(static_cast<u64>((history_cap - chunk_slots) /
                                                 (2 * std::max<size_t>(count, 1))),
                                8, 256);
    }
    for (bool more = true; more;) {
        more = false;
        for (u32 i = 0; i < count; ++i) {
            more = DrainThread(i + 1, *threads[i], tails[i], round) || more;
        }
        if (more) {
            RelieveHistory(stamp, tails, count);
        }
    }
    ApplyEnds();
    ApplyLinks();
    TakeLosses();
    NoteRetained();
}

/// Frees history in the middle of a pass. `tails` are the pass's snapshots of the rings; what is
/// still in them would reach its frame too late if it began or ended before a time that is
/// resolved, so everything here stays below the oldest of it. The presents queued up to that time
/// have all their events: they are processed (they would otherwise stop everything behind them),
/// then the open interval is resolved in part, as MaybeChunk does, up to the time of the pass less
/// the margin publication needs.
void Session::RelieveHistory(u64 stamp, const std::array<u64, MaxThreads>& tails, u32 count) {
    if (chunk_slots == 0 || history < chunk_slots) {
        return;
    }
    u64 bound = OpenEnd;
    for (u32 i = 0; i < count; ++i) {
        const ThreadBuf& buf = *threads[i];
        for (u64 k = buf.head.load(std::memory_order_relaxed); k < tails[i]; ++k) {
            const RingEvent& event = buf.events[k & buf.mask];
            bound = std::min(bound, event.type == EventType::End ? event.end_ns : event.begin_ns);
        }
    }
    ApplyEnds(); // the spans of this pass are complete in what is resolved
    ApplyLinks();
    TakeLosses();
    while (relief_items && !work.empty() && work.front().ts < bound) {
        Process(work.front());
        work.pop_front();
    }
    u64 through = std::min(Sub(stamp, ChunkMarginNs), Sub(bound, 1));
    if (!work.empty()) {
        through = std::min(through, work.front().ts);
    } else {
        std::scoped_lock lock{queue_mutex};
        if (!queue.empty()) {
            through = std::min(through, queue.front().ts);
        }
    }
    ResolveInPart(through);
}

void Session::MarkOpenIncomplete() {
    for (ThreadState& state : states) {
        for (SpanRec& rec : state.open) {
            rec.flags |= SpanFlags::Incomplete;
        }
    }
}

/// Makes room for one more record in `spans`. The capacity of a vector is storage the session
/// owns whether or not it is in use, so growth is charged to `history` (in records) and refused
/// when it does not fit in `history_cap`; the charge is returned only by Trim, when the storage is
/// actually released.
bool Session::Reserve(std::vector<SpanRec>& spans) {
    if (spans.size() < spans.capacity()) {
        return true;
    }
    const size_t room = history < history_cap ? history_cap - history : 0;
    const size_t grow = std::min(std::max(spans.capacity(), MinSpanCapacity), room);
    if (grow == 0) {
        return false;
    }
    const size_t before = spans.capacity();
    spans.reserve(before + grow);
    history += spans.capacity() - before;
    return true;
}

/// Releases the capacity of `spans` once it holds less than half of it, and returns it to the
/// allowance.
void Session::Trim(std::vector<SpanRec>& spans) {
    if (spans.capacity() <= 2 * spans.size()) {
        return;
    }
    const size_t before = spans.capacity();
    std::vector<SpanRec> fit(spans.begin(), spans.end());
    spans.swap(fit);
    history -= before - spans.capacity();
}

/// Records the high-water mark of the span storage the session owns, from the real capacities.
void Session::NoteRetained() {
    const u32 count = thread_count.load(std::memory_order_acquire);
    size_t slots{};
    for (u32 i = 0; i < count; ++i) {
        slots += states[i].open.capacity() + states[i].closed.capacity();
    }
    const size_t bytes = slots * sizeof(SpanRec);
    if (bytes > peak_retained_bytes.load(std::memory_order_relaxed)) {
        peak_retained_bytes.store(bytes, std::memory_order_relaxed);
    }
}

void Session::Prune(bool force) {
    const u64 now = NowNs();
    // Spans stay RetainNs for the marker, until the history is half full: then everything before
    // the last resolved time goes, so that a busy period cannot fill the history with history.
    const bool tight = force || (retain_slots != 0 && history >= retain_slots);
    const u64 floor = tight ? last_resolved_ts : std::min(last_resolved_ts, Sub(now, RetainNs));
    if (floor <= pruned_through) {
        return;
    }
    pruned_through = floor;
    const u32 count = thread_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        ThreadState& state = states[i];
        std::erase_if(state.closed, [floor](const SpanRec& r) { return r.end_ns <= floor; });
        Trim(state.closed);
        Trim(state.open);
    }
    // The losses of an interval resolved in part stay until its present has joined the parts.
    const u64 loss_floor = carry_through != 0 ? std::min(floor, carry_from) : floor;
    std::erase_if(losses, [loss_floor](const std::pair<u64, u64>& l) {
        return l.second <= loss_floor;
    });
}

/// Resolves the open interval up to a little before now, when the history is three quarters full,
/// and frees what that part used. Nothing is lost by it: the part is kept as a carry and the
/// present that ends the interval adds the rest.
void Session::MaybeChunk() {
    if (chunk_slots == 0 || history < chunk_slots) {
        return;
    }
    const u64 stamp = NowNs();
    Quiesce();
    DrainAll();
    {
        std::scoped_lock lock{queue_mutex};
        if (!queue.empty()) {
            return; // a present is waiting: it resolves the interval whole and prunes
        }
    }
    ResolveInPart(Sub(stamp, ChunkMarginNs));
}

/// Resolves the open interval from the last resolved time up to `through`, keeps the result as
/// the carry the present that ends the interval joins, and prunes what it used. False when there
/// is nothing to resolve.
bool Session::ResolveInPart(u64 through) {
    const u64 from = last_resolved_ts != 0 ? last_resolved_ts : start_ns;
    if (through <= from) {
        return false;
    }
    std::vector<u32> roots;
    if (have_prev) {
        if (last_root != 0) {
            roots.push_back(last_root);
        }
    } else {
        std::scoped_lock lock{stats_mutex};
        roots = boot_roots;
    }
    Decomp part = Decompose(from, through, roots);
    if (carry_through == 0) {
        carry_from = from;
        carry = std::move(part);
    } else {
        carry.Add(part);
    }
    carry_through = through;
    last_resolved_ts = through;
    if (have_prev && !interactive_done && cum.size() < MaxCumulative) {
        CauseTimes sum = cum_causes;
        for (size_t i = 0; i < CauseCount; ++i) {
            sum[i] += carry.cause[i];
        }
        cum.push_back({through, sum, cum_complete && carry.complete});
    }
    Prune(true);
    return true;
}

/// Parks the consumer between its passes while a test holds it (one relaxed load otherwise).
void Session::HoldForTest() {
    if (!hold_consumer.load(std::memory_order_acquire)) {
        return;
    }
    consumer_held.store(true, std::memory_order_release);
    for (;;) {
        {
            std::scoped_lock lock{queue_mutex};
            if (stop || !hold_consumer.load(std::memory_order_acquire)) {
                break;
            }
        }
        std::this_thread::yield();
    }
    consumer_held.store(false, std::memory_order_release);
}

/// Takes the part of the interval (s, e] that MaybeChunk resolved, and says where the rest begins.
Joined Session::TakeCarry(u64 s, u64 e) {
    Joined joined;
    joined.from = s;
    if (carry_through == 0) {
        return joined;
    }
    const u64 begin = carry_from;
    const u64 through = carry_through;
    joined.part = std::move(carry);
    carry = Decomp{};
    carry_from = 0;
    carry_through = 0;
    if (begin == s && through <= e) {
        joined.from = through;
        return joined;
    }
    // The resolved part belongs to another interval, or runs past this one's end (a present
    // arrived late): this interval cannot be built from it.
    const u64 end = std::max(s, std::min(through, e));
    joined.part = Decomp{};
    joined.part.cause[CauseIdx(Cause::Unknown)] = end - s;
    joined.part.reasons |= UnknownReason::LostSpans;
    joined.part.complete = false;
    joined.from = end;
    joined.broken = true;
    lost_consumer.fetch_add(1, std::memory_order_relaxed);
    return joined;
}

/// Whether a loss episode touches (s, e].
bool Session::LostBetween(u64 s, u64 e) const {
    return std::any_of(losses.begin(), losses.end(),
                       [s, e](const std::pair<u64, u64>& l) { return l.second > s && l.first <= e; });
}

/// Names what was recorded and what was lost, by kind and by thread, when the session ends.
void Session::LogDiagnostics() const {
    std::array<u64, 4> total{};
    u64 ring_lost = 0;
    const u32 count = thread_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        const ThreadBuf& buf = *threads[i];
        std::array<u64, 4> own{};
        u64 sum = 0;
        for (size_t k = 0; k < own.size(); ++k) {
            own[k] = buf.kinds[k].load(std::memory_order_relaxed);
            total[k] += own[k];
            sum += own[k];
        }
        const u64 dropped = buf.lost.load(std::memory_order_relaxed);
        ring_lost += dropped;
        if (dropped != 0 || sum >= 10000) {
            LOG_INFO(HW_GPU,
                     "Nx2Attribution thread {}: {} begin, {} end, {} closed, {} link events, {} "
                     "lost in the ring",
                     buf.name.data(), own[0], own[1], own[2], own[3], dropped);
        }
    }
    const u64 all = TotalLost();
    const u64 known = ring_lost + lost_history + lost_late;
    LOG_INFO(HW_GPU,
             "Nx2Attribution events: {} begin, {} end, {} closed, {} link; lost: {} ring, {} "
             "history, {} late, {} other",
             total[0], total[1], total[2], total[3], ring_lost, lost_history, lost_late,
             all > known ? all - known : u64{0});
}

Decomp Session::Decompose(u64 s, u64 e, std::span<const u32> roots) const {
    const u32 count = thread_count.load(std::memory_order_acquire);
    Resolver resolver{std::span<const ThreadState>{states.data(), count}, losses, s, e,
                      config.charge_presenter_gaps};
    Decomp result = resolver.Run(roots);
    if (resolver.PeakScratchBytes() > peak_scratch_bytes.load(std::memory_order_relaxed)) {
        peak_scratch_bytes.store(resolver.PeakScratchBytes(), std::memory_order_relaxed);
    }
    return result;
}

u64 Session::TotalLost() const {
    u64 total = lost_unregistered.load(std::memory_order_relaxed) +
                boundaries_dropped.load(std::memory_order_relaxed) +
                lost_consumer.load(std::memory_order_relaxed);
    const u32 count = thread_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        total += threads[i]->lost.load(std::memory_order_relaxed);
    }
    return total;
}

void Session::Process(const Item& item) {
    if (item.marker) {
        ResolveMarker(item);
    } else {
        ResolveBoundary(item);
    }
    Mirror();
}

void Session::Mirror() {
    std::scoped_lock lock{stats_mutex};
    mirror_frames.store(snap.frames, std::memory_order_relaxed);
    mirror_long_frames.store(snap.long_frames, std::memory_order_relaxed);
    mirror_complete.store(incomplete_long_frames == 0 && sink_errors.load() == 0 && TotalLost() == 0,
                          std::memory_order_relaxed);
}

void Session::ResolveStartup(u64 first_present) {
    const u64 created = process_created_ns.load(std::memory_order_relaxed);
    std::vector<u32> roots;
    {
        std::scoped_lock lock{stats_mutex};
        roots = boot_roots;
        snap.first_host_present_ns = first_present;
        snap.first_120s.anchor_ns = first_present;
    }
    // What MaybeChunk resolved before the first present is part of this interval.
    Joined joined = TakeCarry(start_ns, first_present);
    if (created == 0 || created >= first_present) {
        return;
    }
    IntervalAttribution ia;
    ia.duration_ns = first_present - created;
    const u64 pre_end = std::min(start_ns, first_present);
    const u64 pre = created < pre_end ? pre_end - created : 0;
    u64 from = std::max(created, start_ns);
    bool complete = !quiesce_timeout && !joined.broken;
    Decomp d;
    if (joined.from > start_ns) {
        if (created <= start_ns) {
            d = std::move(joined.part);
            from = joined.from;
        } else {
            // The process was created after the recorder began: the part resolved from the
            // start cannot be cut back to its creation.
            const u64 covered = std::max(from, joined.from);
            d.cause[CauseIdx(Cause::Unknown)] = covered - from;
            d.reasons |= UnknownReason::LostSpans;
            d.complete = false;
            from = covered;
        }
        if (LostBetween(start_ns, joined.from)) {
            d.reasons |= UnknownReason::LostSpans;
            d.complete = false;
        }
    }
    if (from < first_present) {
        d.Add(Decompose(from, first_present, roots));
    }
    complete = complete && d.complete;
    ia.cause_ns = d.cause;
    ia.cause_ns[CauseIdx(Cause::Unknown)] += pre;
    ia.status = complete ? IntervalStatus::Complete : IntervalStatus::Incomplete;
    std::scoped_lock lock{stats_mutex};
    snap.process_to_first_present = ia;
    sum_duration_ns += ia.duration_ns;
    sum_unknown_ns += ia.cause_ns[CauseIdx(Cause::Unknown)];
    snap.producer_unknown_wait_ns += d.producer_unknown_wait_ns;
    for (size_t i = 0; i < CauseCount; ++i) {
        snap.inferred_ns[i] += d.inferred[i];
    }
}

void Session::ResolveBoundary(const Item& item) {
    const bool timeout = std::exchange(quiesce_timeout, false);
    const u64 unregistered = lost_unregistered.load(std::memory_order_relaxed);
    const bool unregistered_new = unregistered != seen_unregistered;
    seen_unregistered = unregistered;

    if (!have_prev) {
        // The first present anchors the intervals; its time was published with it. If its own
        // item was dropped, this item is a later one and is resolved below like any other.
        const u64 first = first_present_ns.load(std::memory_order_acquire);
        have_prev = true;
        anchor = first;
        prev_ts = first;
        prev_id = 1;
        prev_counters = item.counters;
        last_resolved_ts = first;
        last_root = item.root;
        cum.push_back({first, {}, true});
        ResolveStartup(first);
        if (item.id == 1) {
            Prune();
            return;
        }
    }

    // An interval is (the present before this one, this one) as the presenting thread saw them,
    // kept or dropped from the queue: dropped boundaries never merge two intervals into one.
    const u64 s = item.prev_ts;
    const u64 e = item.ts;
    const u64 dur = e - s;
    const bool is_long = dur > config.threshold_ns;
    const bool gap = item.prev_id != prev_id;
    if (gap) {
        SkipLostInterval(s);
    }
    const bool degraded = timeout || unregistered_new || gap;

    // What MaybeChunk resolved while the interval was open is joined with the rest here.
    Joined joined = TakeCarry(s, e);
    Decomp d;
    if (dur > 0 && (is_long || !interactive_done)) {
        const u32 root = item.root;
        if (joined.from < e) {
            d = Decompose(joined.from, e,
                          root != 0 ? std::span<const u32>{&root, 1} : std::span<const u32>{});
        }
        d.Add(joined.part);
        if (joined.from > s && LostBetween(s, joined.from)) {
            d.reasons |= UnknownReason::LostSpans;
            d.complete = false;
        }
    }
    d.reasons |= item.reasons;
    if (degraded) {
        d.complete = false;
        d.reasons |= UnknownReason::LostSpans;
    }

    if (!interactive_done) {
        for (size_t i = 0; i < CauseCount; ++i) {
            cum_causes[i] += d.cause[i];
        }
        cum_complete = cum_complete && d.complete;
        if (cum.size() < MaxCumulative) {
            cum.push_back({e, cum_causes, cum_complete});
        }
    }

    LongFrame frame;
    if (is_long) {
        frame.id = item.id;
        frame.start_ns = s;
        frame.end_ns = e;
        frame.duration_ns = dur;
        frame.cause_ns = d.cause;
        frame.activity_ns = d.activity;
        frame.inferred_ns = d.inferred;
        u64 top = 0;
        u32 at_top = 0;
        Cause primary = Cause::Unknown;
        for (size_t i = 0; i < CauseCount; ++i) {
            if (d.cause[i] > top) {
                top = d.cause[i];
                at_top = 1;
                primary = static_cast<Cause>(i);
            } else if (d.cause[i] == top && top != 0) {
                ++at_top;
            }
        }
        frame.primary_cause = at_top == 1 ? primary : Cause::Unknown;
        frame.confidence_permille = static_cast<u32>(
            (dur - d.cause[CauseIdx(Cause::Unknown)]) * 1000 / dur);
        frame.unknown_reasons = d.reasons;
        for (size_t i = 0; i < EventCount; ++i) {
            frame.counter_deltas[i] = Sub(item.counters[i], prev_counters[i]);
        }
        std::vector<std::pair<WorkId, u64>> works{d.work_ns.begin(), d.work_ns.end()};
        std::sort(works.begin(), works.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        for (size_t i = 0; i < works.size() && i < MaxCriticalWork; ++i) {
            frame.critical_work_ids[i] = works[i].first;
            ++frame.critical_work_count;
        }
        frame.present_result = item.result;
        frame.instrumentation_complete = d.complete;
    }

    {
        std::scoped_lock lock{stats_mutex};
        ++snap.frames;
        FirstWindow& w = snap.first_120s;
        const u64 since = e - anchor;
        if (since <= FirstWindowNs) {
            ++w.frames;
            if (is_long) {
                ++w.frames_over_threshold;
            }
            w.max_ns = std::max(w.max_ns, dur);
            w.elapsed_ns = since;
            if (since == FirstWindowNs) {
                w.complete = true;
            }
        } else if (!w.complete) {
            w.complete = true;
            w.elapsed_ns = FirstWindowNs;
            w.crossing_tail_interval_ns = dur;
        }
        if (is_long) {
            ++snap.long_frames;
            sum_duration_ns += dur;
            sum_unknown_ns += d.cause[CauseIdx(Cause::Unknown)];
            snap.producer_unknown_wait_ns += d.producer_unknown_wait_ns;
            for (size_t i = 0; i < CauseCount; ++i) {
                snap.inferred_ns[i] += d.inferred[i];
            }
            if (frame.primary_cause == Cause::Unknown) {
                ++primary_unknown;
            }
            if (!frame.instrumentation_complete) {
                ++incomplete_long_frames;
            }
        }
    }

    if (is_long) {
        {
            std::scoped_lock lock{g_sink_mutex};
            if (g_sink.write != nullptr && !g_sink.write(g_sink.user, frame)) {
                sink_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
        mirror_last_long_id.store(frame.id, std::memory_order_relaxed);
        mirror_last_long_ns.store(frame.duration_ns, std::memory_order_relaxed);
        LOG_INFO(HW_GPU, "{}", LongFrameLine(frame));
    }

    prev_ts = e;
    prev_id = item.id;
    prev_counters = item.counters;
    last_resolved_ts = e;
    last_root = item.root;
    Prune();
}

/// Presents between the last resolved one and `until` were dropped from the queue (counted in
/// lost_events): that time is not a frame. It is unattributed, and never a long frame.
void Session::SkipLostInterval(u64 until) {
    AddLoss(prev_ts, until);
    if (!interactive_done) {
        cum_causes[CauseIdx(Cause::Unknown)] += Sub(until, prev_ts);
        cum_complete = false;
        if (cum.size() < MaxCumulative) {
            cum.push_back({until, cum_causes, false});
        }
    }
    last_resolved_ts = until;
    std::scoped_lock lock{stats_mutex};
    FirstWindow& w = snap.first_120s;
    const u64 since = Sub(until, anchor);
    if (since <= FirstWindowNs) {
        w.elapsed_ns = since;
        w.complete = w.complete || since == FirstWindowNs;
    } else if (!w.complete) {
        w.complete = true;
        w.elapsed_ns = FirstWindowNs;
    }
}

void Session::ResolveMarker(const Item& item) {
    if (!have_prev) {
        lost_consumer.fetch_add(1, std::memory_order_relaxed); // its present was never resolved
        return;
    }
    if (interactive_done) {
        return;
    }
    const u64 m = item.ts;
    IntervalAttribution ia;
    ia.duration_ns = m - anchor;
    const auto after = std::upper_bound(cum.begin(), cum.end(), m,
                                        [](u64 value, const CumulativeEntry& c) { return value < c.ts; });
    const CumulativeEntry& base = *(after == cum.begin() ? after : std::prev(after));
    CauseTimes causes = base.causes;
    bool complete = base.complete;
    if (base.ts < m) {
        if (base.ts >= pruned_through) {
            const u32 root = last_root;
            const Decomp d = Decompose(
                base.ts, m, root != 0 ? std::span<const u32>{&root, 1} : std::span<const u32>{});
            for (size_t i = 0; i < CauseCount; ++i) {
                causes[i] += d.cause[i];
            }
            complete = complete && d.complete;
        } else {
            causes[CauseIdx(Cause::Unknown)] += m - base.ts; // its spans are no longer retained
            complete = false;
        }
    }
    ia.cause_ns = causes;
    ia.status = complete ? IntervalStatus::Complete : IntervalStatus::Incomplete;
    interactive_done = true;
    cum.clear();
    cum.shrink_to_fit();
    std::scoped_lock lock{stats_mutex};
    snap.first_present_to_interactive = ia;
    snap.interactive_status = ia.status;
    sum_duration_ns += ia.duration_ns;
    sum_unknown_ns += ia.cause_ns[CauseIdx(Cause::Unknown)];
}

AttributionSnapshot Session::Snapshot() {
    std::scoped_lock lock{stats_mutex};
    AttributionSnapshot result = snap;
    const bool finished = ended.load(std::memory_order_acquire);
    result.enabled = true; // a session was recorded; AttributionEnabled() says if it still runs
    result.threshold_ns = config.threshold_ns;
    result.lost_events = TotalLost();
    result.dropped_boundaries = boundaries_dropped.load(std::memory_order_relaxed);
    result.sink_write_errors = sink_errors.load(std::memory_order_relaxed);
    result.rejected_presents = rejected.load(std::memory_order_relaxed);
    result.process_created_ns = process_created_ns.load(std::memory_order_relaxed);
    result.process_created_uncertainty_ns =
        process_created_uncertainty_ns.load(std::memory_order_relaxed);
    result.complete = finished && result.lost_events == 0 && result.sink_write_errors == 0 &&
                      incomplete_long_frames == 0;
    result.unknown_wall_fraction =
        sum_duration_ns != 0
            ? static_cast<double>(sum_unknown_ns) / static_cast<double>(sum_duration_ns)
            : 0.0;
    result.unknown_primary_frame_fraction =
        snap.long_frames != 0
            ? static_cast<double>(primary_unknown) / static_cast<double>(snap.long_frames)
            : 0.0;
    return result;
}

} // Anonymous namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

WorkId NewWorkId() {
    return g_next_work.fetch_add(1, std::memory_order_relaxed);
}

namespace Detail {
/// The most bytes of span storage the current (or last) session held at once, for the tests.
size_t PeakRetainedHistoryBytes() {
    std::scoped_lock lock{g_lifecycle_mutex};
    return g_owned != nullptr ? g_owned->peak_retained_bytes.load(std::memory_order_relaxed) : 0;
}

/// The most bytes of resolver scratch (leaves, work spans, sweep and wait storage) one
/// decomposition of the current (or last) session held at once, for the tests.
size_t PeakResolverScratchBytes() {
    std::scoped_lock lock{g_lifecycle_mutex};
    return g_owned != nullptr ? g_owned->peak_scratch_bytes.load(std::memory_order_relaxed) : 0;
}

/// What a decomposition may hold of resolver scratch, worked out by the same function as the
/// allowance behind the history size, for the most span slots the current (or last) session
/// retained at once, for the tests.
size_t ResolverScratchAllowance() {
    std::scoped_lock lock{g_lifecycle_mutex};
    if (g_owned == nullptr) {
        return 0;
    }
    return ScratchAllowance(g_owned->peak_retained_bytes.load(std::memory_order_relaxed) /
                            sizeof(SpanRec));
}

/// The number of events of the calling thread's ring the consumer has taken so far (0 without a
/// session), for the tests that must not outrun the consumer. It reads one existing counter and is
/// on no recording path.
u64 DrainedRingEvents() {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return 0;
    }
    const ThreadBuf* t = ThisThread(*s);
    return t != nullptr ? t->head.load(std::memory_order_acquire) : 0;
}

/// The number of events in the calling thread's ring that the consumer has not taken yet (0
/// without a session), for the tests. It reads two existing counters and is on no recording path.
u64 UndrainedRingEvents() {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return 0;
    }
    const ThreadBuf* t = ThisThread(*s);
    return t != nullptr ? t->tail.load(std::memory_order_acquire) -
                              t->head.load(std::memory_order_acquire)
                        : 0;
}

/// Test hook: parks the consumer (true) until it has stopped taking events, or lets it go
/// (false). False without a session or when the consumer does not park in time.
bool HoldConsumer(bool hold) {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return false;
    }
    s->hold_consumer.store(hold, std::memory_order_release);
    if (!hold) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!s->consumer_held.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return s->consumer_held.load(std::memory_order_acquire);
}
} // namespace Detail

bool BeginSession(const SessionConfig& config) {
    std::scoped_lock lock{g_lifecycle_mutex};
    if (Detail::attribution_enabled.load(std::memory_order_acquire)) {
        return false;
    }
    auto s = std::make_unique<Session>();
    s->generation = ++g_generation;
    s->config = config;
    s->start_ns = NowNs();
    // One budget: half for the rings (allocated by the threads that register), half for the
    // retained history and the resolver's working storage for it. A slot of span storage is
    // charged the record and the worst-case scratch of resolving it (HistoryBytesPerSpan), slots
    // are counted by the capacity the session owns, and the scratch that does not depend on the
    // number of spans (ScratchFixedBytes) is taken off first. A ring never holds less than four
    // events, and the history never fewer than 64 slots, whatever the budget.
    const size_t ring_budget = config.buffer_bytes / 2;
    const size_t history_budget = config.buffer_bytes - ring_budget;
    const size_t per_thread = ring_budget / MaxThreads / sizeof(RingEvent);
    s->ring_capacity = std::bit_floor(std::max<size_t>(per_thread, 4));
    s->history_cap = std::max<size_t>(
        (history_budget > ScratchFixedBytes ? history_budget - ScratchFixedBytes : 0) /
            HistoryBytesPerSpan,
        64);
    if (s->history_cap >= MinChunkHistory) {
        s->chunk_slots = s->history_cap / 4 * 3;
        s->retain_slots = s->history_cap / 2;
    }
    s->process_created_ns.store(g_pending_created.exchange(0), std::memory_order_relaxed);
    s->process_created_uncertainty_ns.store(g_pending_uncertainty.exchange(0),
                                            std::memory_order_relaxed);
    s->snap.threshold_ns = config.threshold_ns;
    if (const ThreadBuf* caller = ThisThread(*s, "session")) {
        s->boot_roots.push_back(caller->id);
    }
    Detail::SetAttributionCounting(true);
    s->StartConsumer();
    auto old = std::move(g_owned);
    g_owned = std::move(s);
    g_session.store(g_owned.get(), std::memory_order_release);
    Detail::attribution_enabled.store(true, std::memory_order_release);
    old.reset();
    return true;
}

void EndSession() {
    std::scoped_lock lock{g_lifecycle_mutex};
    Session* s = g_owned.get();
    if (s == nullptr || s->ended.load(std::memory_order_acquire) ||
        !Detail::attribution_enabled.load(std::memory_order_acquire)) {
        return;
    }
    Detail::attribution_enabled.store(false, std::memory_order_release);
    s->StopConsumer();
    Detail::SetAttributionCounting(false);
    s->ended.store(true, std::memory_order_release);
    s->Mirror();
    LOG_INFO(HW_GPU, "Nx2Attribution session ended: {} frames, {} long, {} lost events",
             s->snap.frames, s->snap.long_frames, s->TotalLost());
    s->LogDiagnostics();
}

u32 RegisterThread(std::string_view name) {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return 0;
    }
    const ThreadBuf* buf = ThisThread(*s, name);
    return buf != nullptr ? buf->id : 0;
}

void MarkGuestThread() {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return;
    }
    ThreadBuf* t = ThisThread(*s, "guest core");
    if (t != nullptr && !t->guest.load(std::memory_order_relaxed)) {
        t->guest.store(true, std::memory_order_relaxed);
    }
}

SpanToken BeginSpan(Cause cause, WorkId work, u64 amount) {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return {};
    }
    ThreadBuf* t = ThisThread(*s);
    if (t == nullptr) {
        NoteUnregisteredLoss(*s);
        return {};
    }
    t->busy.exchange(1, std::memory_order_seq_cst);
    RingEvent event;
    event.type = EventType::Begin;
    event.cause = cause;
    event.work = work;
    event.amount = amount;
    event.seq = t->next_seq;
    event.begin_ns = NowNs();
    // The span's own End needs a slot, and so does the End of every span already open here.
    const bool ok = Push(*t, event, u64{t->open_count.load(std::memory_order_relaxed)} + 1);
    if (ok) {
        ++t->next_seq;
        t->open_count.fetch_add(1, std::memory_order_relaxed);
    }
    t->busy.store(0, std::memory_order_release);
    if (!ok) {
        NoteLoss(*t, event.begin_ns, event.begin_ns);
        return {};
    }
    return {s->generation, t->id, event.seq};
}

void EndSpan(SpanToken token, u64 amount, u32 flags) {
    if (!token.Valid()) {
        return;
    }
    Session* s = ActiveSession();
    if (s == nullptr || token.session_generation != s->generation) {
        return;
    }
    ThreadBuf* t = ThisThread(*s);
    if (t == nullptr) {
        NoteUnregisteredLoss(*s);
        return;
    }
    const bool own = token.thread_registration_id == t->id;
    t->busy.exchange(1, std::memory_order_seq_cst);
    RingEvent event;
    event.type = EventType::End;
    event.seq = token.sequence;
    event.owner = token.thread_registration_id;
    event.amount = amount;
    event.flags = flags;
    event.end_ns = NowNs();
    // An End on its own thread always has room (BeginSpan reserved it).
    const bool ok = Push(*t, event, own ? 0 : u64{t->open_count.load(std::memory_order_relaxed)});
    ThreadBuf* owner = own ? t : nullptr;
    if (!own && token.thread_registration_id >= 1 &&
        token.thread_registration_id <= s->thread_count.load(std::memory_order_acquire)) {
        owner = s->threads[token.thread_registration_id - 1].get();
    }
    if (owner != nullptr && owner->open_count.load(std::memory_order_relaxed) != 0) {
        owner->open_count.fetch_sub(1, std::memory_order_relaxed);
    }
    t->busy.store(0, std::memory_order_release);
    if (!ok) {
        NoteLoss(*t, event.end_ns, event.end_ns);
    }
}

void RecordSpan(Cause cause, u64 begin_ns, u64 end_ns, WorkId work, u64 amount, u32 flags) {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return;
    }
    ThreadBuf* t = ThisThread(*s);
    if (t == nullptr) {
        NoteUnregisteredLoss(*s);
        return;
    }
    // The consumer waits for a publication in progress (see Quiesce); the stamps are the
    // caller's, so what this covers is the push, not the time before it.
    t->busy.exchange(1, std::memory_order_seq_cst);
    RingEvent event;
    event.type = EventType::Closed;
    event.cause = cause;
    event.work = work;
    event.amount = amount;
    event.flags = flags;
    event.begin_ns = begin_ns;
    event.end_ns = std::max(begin_ns, end_ns);
    event.seq = t->next_seq;
    if (Push(*t, event, t->open_count.load(std::memory_order_relaxed))) {
        ++t->next_seq;
    } else {
        NoteLoss(*t, event.begin_ns, event.end_ns);
    }
    t->busy.store(0, std::memory_order_release);
}

void LinkWait(SpanToken wait, WorkId producer, DependencyKind kind) {
    if (!wait.Valid() || producer == 0) {
        return;
    }
    Session* s = ActiveSession();
    if (s == nullptr || wait.session_generation != s->generation) {
        return;
    }
    ThreadBuf* t = ThisThread(*s);
    if (t == nullptr) {
        NoteUnregisteredLoss(*s);
        return;
    }
    RingEvent event;
    event.type = EventType::Link;
    event.seq = wait.sequence;
    event.owner = wait.thread_registration_id;
    event.work = producer;
    event.kind = kind;
    event.begin_ns = NowNs();
    if (!Push(*t, event, t->open_count.load(std::memory_order_relaxed))) {
        NoteLoss(*t, event.begin_ns, event.begin_ns);
    }
}

u64 OnHostPresent(const FrameBoundary& boundary) {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return 0;
    }
    if (!QualifiesAsHostPresent(boundary.result, boundary.real_frame)) {
        s->rejected.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const ThreadBuf* t = ThisThread(*s, "present");
    Item item;
    item.guest_frame_id = boundary.guest_frame_id;
    item.root = t != nullptr ? t->id : 0;
    item.result = boundary.result;
    item.reasons = s->pending_reasons.exchange(0, std::memory_order_relaxed);
    Detail::ReadEventCounts(item.counters);
    bool queued = false;
    {
        // The id, the link to the present before it and the publication of the first present are
        // one step, in queue order, whether the item is kept or dropped (a marker is admitted under
        // the same lock, so it can never be queued before the first present).
        std::scoped_lock lock{s->queue_mutex};
        item.id = s->next_present.fetch_add(1, std::memory_order_relaxed);
        item.prev_id = s->last_actual_id;
        item.prev_ts = s->last_actual_ts;
        item.ts = std::max(boundary.timestamp_ns, s->last_actual_ts);
        s->last_actual_id = item.id;
        s->last_actual_ts = item.ts;
        if (s->first_present_ns.load(std::memory_order_relaxed) == 0) {
            s->first_present_ns.store(item.ts, std::memory_order_release);
        }
        s->last_present_ns.store(item.ts, std::memory_order_relaxed);
        if (s->queue.size() < MaxQueueItems) {
            s->queue.push_back(item);
            queued = true;
        }
    }
    if (queued) {
        s->queue_cv.notify_one();
    } else {
        s->boundaries_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return item.id;
}

void NoteUnknownReason(u32 unknown_reason_bits) {
    if (Session* s = ActiveSession()) {
        s->pending_reasons.fetch_or(unknown_reason_bits, std::memory_order_relaxed);
    }
}

void BeginBootPhase(BootPhase phase) {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return;
    }
    const ThreadBuf* t = ThisThread(*s);
    std::scoped_lock lock{s->stats_mutex};
    if (t != nullptr &&
        std::find(s->boot_roots.begin(), s->boot_roots.end(), t->id) == s->boot_roots.end()) {
        s->boot_roots.push_back(t->id);
    }
    if (s->snap.phases.size() < MaxPhases) {
        s->snap.phases.push_back({phase, PhaseOutcome::NotRun, NowNs(), 0});
    }
}

void EndBootPhase(BootPhase phase, PhaseOutcome outcome) {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return;
    }
    const u64 now = NowNs();
    std::scoped_lock lock{s->stats_mutex};
    for (auto it = s->snap.phases.rbegin(); it != s->snap.phases.rend(); ++it) {
        if (it->phase == phase && it->end_ns == 0) {
            it->end_ns = std::max(now, it->begin_ns);
            it->outcome = outcome;
            return;
        }
    }
}

void SetProcessCreated(u64 created_ns, u64 uncertainty_ns) {
    if (Session* s = ActiveSession()) {
        s->process_created_ns.store(created_ns, std::memory_order_relaxed);
        s->process_created_uncertainty_ns.store(uncertainty_ns, std::memory_order_relaxed);
        return;
    }
    g_pending_created.store(created_ns, std::memory_order_relaxed);
    g_pending_uncertainty.store(uncertainty_ns, std::memory_order_relaxed);
}

MarkerResult MarkInteractive(const InteractiveMarker& marker) {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return MarkerResult::NoSession;
    }
    const u64 now = NowNs();
    bool queued = false;
    {
        // Validated and queued under the lock that publishes the first present, so a marker is
        // always behind the first present's item.
        std::scoped_lock lock{s->queue_mutex};
        if (s->marker_accepted.load(std::memory_order_relaxed)) {
            return MarkerResult::Duplicate;
        }
        const u64 first = s->first_present_ns.load(std::memory_order_relaxed);
        if (first == 0 || marker.marker_ns < first || marker.marker_ns > now) {
            return MarkerResult::Backdated;
        }
        s->marker_accepted.store(true, std::memory_order_relaxed);
        {
            std::scoped_lock stats{s->stats_mutex};
            s->snap.interactive = marker;
            s->snap.interactive_status = IntervalStatus::Incomplete; // until it is resolved
        }
        if (s->queue.size() < MaxQueueItems) {
            Item item;
            item.marker = true;
            item.ts = marker.marker_ns;
            s->queue.push_back(item);
            queued = true;
        }
    }
    if (queued) {
        s->queue_cv.notify_one();
    } else {
        s->lost_consumer.fetch_add(1, std::memory_order_relaxed);
    }
    return MarkerResult::Accepted;
}

AttributionSnapshot ReadAttributionSnapshot() {
    std::scoped_lock lock{g_lifecycle_mutex};
    Session* s = g_owned.get();
    if (s == nullptr) {
        return {};
    }
    return s->Snapshot();
}

void SetLongFrameSink(LongFrameSink sink) {
    std::scoped_lock lock{g_sink_mutex};
    g_sink = sink;
}

std::string LongFrameLine(const LongFrame& frame) {
    const auto ms = [](u64 ns) { return static_cast<double>(ns) / 1e6; };
    std::string line = fmt::format("Nx2LongFrame frame={} dt_ms={:.1f} primary={}", frame.id,
                                   ms(frame.duration_ns), CauseName(frame.primary_cause));
    for (size_t i = 0; i < CauseCount; ++i) {
        fmt::format_to(std::back_inserter(line), " {}_ms={:.1f}", CauseNames[i],
                       ms(frame.cause_ns[i]));
    }
    fmt::format_to(std::back_inserter(line), " complete={}",
                   frame.instrumentation_complete ? 1 : 0);
    return line;
}

CrashStatus ReadCrashStatus() noexcept {
    CrashStatus status;
    const Session* s = g_session.load(std::memory_order_acquire);
    if (s == nullptr) {
        return status;
    }
    status.session_active = Detail::attribution_enabled.load(std::memory_order_relaxed);
    status.complete = s->mirror_complete.load(std::memory_order_relaxed);
    status.threshold_ns = s->config.threshold_ns;
    status.lost_events = s->TotalLost();
    status.frames = s->mirror_frames.load(std::memory_order_relaxed);
    status.long_frames = s->mirror_long_frames.load(std::memory_order_relaxed);
    status.first_host_present_ns = s->first_present_ns.load(std::memory_order_relaxed);
    status.last_host_present_ns = s->last_present_ns.load(std::memory_order_relaxed);
    status.last_long_frame_id = s->mirror_last_long_id.load(std::memory_order_relaxed);
    status.last_long_frame_ns = s->mirror_last_long_ns.load(std::memory_order_relaxed);
    return status;
}

} // namespace VideoCommon::Nx2Spikes
