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
//    (lost_events) and its time range marks every frame it touches incomplete.
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

/// What one retained span costs against SessionConfig::buffer_bytes: its record (a vector may hold
/// twice what it uses) and the resolver's copies of it while a frame is decomposed (leaves and the
/// sweep events), so rings plus history stay inside the one budget.
constexpr size_t HistoryBytesPerSpan = 2 * sizeof(SpanRec) + 2 * sizeof(Leaf) + 48;

struct Decomp {
    CauseTimes cause{};
    CauseTimes activity{};
    u32 reasons{};
    bool complete{true};
    u64 producer_unknown_wait_ns{};
    std::unordered_map<WorkId, u64> work_ns;
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
             u64 start, u64 end)
        : states{threads}, loss_episodes{losses}, s{start}, e{end} {}

    Decomp Run(std::span<const u32> roots) {
        for (const auto& [begin, finish] : loss_episodes) {
            if (finish > s && begin <= e) {
                out.reasons |= UnknownReason::LostSpans;
                out.complete = false;
                break;
            }
        }
        leaves.resize(states.size());
        for (size_t i = 0; i < states.size(); ++i) {
            BuildLeaves(static_cast<u32>(i + 1), states[i], leaves[i]);
            for (const Leaf& leaf : leaves[i]) {
                out.activity[CauseIdx(leaf.cause)] += leaf.t1 - leaf.t0;
                if (leaf.work != 0) {
                    by_work[leaf.work].push_back(&leaf);
                }
            }
        }
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
        return std::move(out);
    }

private:
    void BuildLeaves(u32 thread, const ThreadState& state, std::vector<Leaf>& result) {
        std::vector<const SpanRec*> spans;
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
        struct Ev {
            u64 t;
            bool begin;
            u32 idx;
        };
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
        u64 cur = s;
        for (size_t i = 0; i < events.size();) {
            const u64 t = events[i].t;
            if (t > cur && !active.empty()) {
                const SpanRec* top = spans[active.rbegin()->second];
                WorkId work{};
                for (auto it = active.rbegin(); it != active.rend(); ++it) {
                    if (spans[it->second]->work != 0) {
                        work = spans[it->second]->work;
                        break;
                    }
                }
                if (!result.empty() && result.back().span == top && result.back().t1 == cur) {
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
                } else {
                    active.erase(key_of(rec));
                }
            }
            cur = t;
        }
    }

    void Charge(Cause cause, u64 from, u64 to, WorkId work) {
        out.cause[CauseIdx(cause)] += to - from;
        if (work != 0) {
            out.work_ns[work] += to - from;
        }
    }

    void ResolveRoots(std::span<const u32> roots) {
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
        std::vector<u64> points{s, e};
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
                Charge(Cause::Unknown, x, y, 0);
                out.reasons |= UnknownReason::UnlinkedGuest;
            } else if (best->cause == Cause::Waiting) {
                visited.clear();
                ExpandWait(x, y, *best);
            } else {
                Charge(best->cause, x, y, best->work);
            }
        }
    }

    /// Charges [x, y) of a wait: the part covered by the spans of the work it was linked to goes to
    /// their causes, the rest stays Waiting.
    void ExpandWait(u64 x, u64 y, const Leaf& wait) {
        const SpanRec& span = *wait.span;
        if (!span.linked) {
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
        const auto found = by_work.find(producer);
        std::vector<Leaf> cands;
        if (found != by_work.end()) {
            for (const Leaf* leaf : found->second) {
                if (leaf->thread == wait.thread || leaf->t1 <= x || leaf->t0 >= y) {
                    continue;
                }
                Leaf clipped = *leaf;
                clipped.t0 = std::max(leaf->t0, x);
                clipped.t1 = std::min(leaf->t1, y);
                cands.push_back(clipped);
            }
        }
        if (cands.empty()) {
            Charge(Cause::Waiting, x, y, producer);
            return;
        }
        std::vector<u64> points{x, y};
        for (const Leaf& leaf : cands) {
            points.push_back(leaf.t0);
            points.push_back(leaf.t1);
        }
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        visited.push_back(producer);
        for (size_t p = 0; p + 1 < points.size(); ++p) {
            const u64 a = points[p];
            const u64 b = points[p + 1];
            const Leaf* best = nullptr;
            for (const Leaf& leaf : cands) {
                if (leaf.t0 > a || leaf.t1 < b) {
                    continue;
                }
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
    }

    std::span<const ThreadState> states;
    std::span<const std::pair<u64, u64>> loss_episodes;
    u64 s;
    u64 e;
    std::vector<std::vector<Leaf>> leaves;
    std::unordered_map<WorkId, std::vector<const Leaf*>> by_work;
    std::vector<WorkId> visited;
    Decomp out;
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
    size_t history{};
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

    void StartConsumer();
    void StopConsumer();
    void ConsumerMain();
    void DrainAll();
    void DrainThread(u32 id, ThreadBuf& buf);
    void ApplyLinks();
    bool CloseSpan(const PendingEnd& end);
    void ApplyEnds();
    void DropPendingEnds();
    void SkipLostInterval(u64 until);
    void TakeLosses();
    void AddLoss(u64 begin, u64 end);
    void Quiesce();
    void MarkOpenIncomplete();
    void Prune();
    void Process(const Item& item);
    void ResolveBoundary(const Item& item);
    void ResolveStartup(u64 first_present);
    void ResolveMarker(const Item& item);
    SpanRec* FindSpan(u32 owner, u64 seq);
    Decomp Decompose(u64 s, u64 e, std::span<const u32> roots) const;
    u64 TotalLost() const;
    AttributionSnapshot Snapshot();
    void Mirror();
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
        std::deque<Item> batch;
        bool stopping{};
        {
            std::unique_lock lock{queue_mutex};
            if (!stop && queue.empty()) {
                queue_cv.wait_for(lock, ConsumerPeriod, [this] { return stop || !queue.empty(); });
            }
            stopping = stop;
            batch.swap(queue);
        }
        if (abandon.load(std::memory_order_relaxed)) {
            return;
        }
        if (stopping && !finalized) {
            finalized = true;
            Quiesce();
            DrainAll();
            DropPendingEnds(); // their begins never came: counted, not hidden
            MarkOpenIncomplete();
        } else {
            DrainAll();
        }
        for (const Item& item : batch) {
            Quiesce();
            DrainAll();
            Process(item);
        }
        if (stopping && batch.empty()) {
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
    if (losses.size() < MaxLossEpisodes) {
        losses.emplace_back(begin, end);
    } else {
        losses.back().first = std::min(losses.back().first, begin);
        losses.back().second = std::max(losses.back().second, end);
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
    if (last_resolved_ts != 0 && rec.end_ns <= last_resolved_ts) {
        lost_consumer.fetch_add(1, std::memory_order_relaxed); // charged to its frame as open
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

void Session::DrainThread(u32 id, ThreadBuf& buf) {
    ThreadState& state = states[id - 1];
    const u64 head = buf.head.load(std::memory_order_relaxed);
    const u64 tail = buf.tail.load(std::memory_order_acquire);
    for (u64 i = head; i < tail; ++i) {
        const RingEvent& event = buf.events[i & buf.mask];
        switch (event.type) {
        case EventType::Begin: {
            if (history >= history_cap) {
                lost_consumer.fetch_add(1, std::memory_order_relaxed);
                AddLoss(event.begin_ns, event.begin_ns);
                break;
            }
            ++history;
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
            if (history >= history_cap) {
                lost_consumer.fetch_add(1, std::memory_order_relaxed);
                AddLoss(event.begin_ns, event.end_ns);
                break;
            }
            if (last_resolved_ts != 0 && event.end_ns <= last_resolved_ts) {
                // It ended before a frame that is already resolved: that frame missed it.
                lost_consumer.fetch_add(1, std::memory_order_relaxed);
                AddLoss(event.begin_ns, event.end_ns);
                break;
            }
            ++history;
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
    buf.head.store(tail, std::memory_order_release);
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

void Session::DrainAll() {
    const u32 count = thread_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        DrainThread(i + 1, *threads[i]);
    }
    ApplyEnds();
    ApplyLinks();
    TakeLosses();
}

void Session::MarkOpenIncomplete() {
    for (ThreadState& state : states) {
        for (SpanRec& rec : state.open) {
            rec.flags |= SpanFlags::Incomplete;
        }
    }
}

void Session::Prune() {
    const u64 now = NowNs();
    const u64 floor = std::min(last_resolved_ts, Sub(now, RetainNs));
    if (floor <= pruned_through) {
        return;
    }
    pruned_through = floor;
    const u32 count = thread_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        std::vector<SpanRec>& closed = states[i].closed;
        const size_t before = closed.size();
        std::erase_if(closed, [floor](const SpanRec& r) { return r.end_ns <= floor; });
        history -= before - closed.size();
    }
    std::erase_if(losses, [floor](const std::pair<u64, u64>& l) { return l.second <= floor; });
}

Decomp Session::Decompose(u64 s, u64 e, std::span<const u32> roots) const {
    const u32 count = thread_count.load(std::memory_order_acquire);
    Resolver resolver{std::span<const ThreadState>{states.data(), count}, losses, s, e};
    return resolver.Run(roots);
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
    if (created == 0 || created >= first_present) {
        return;
    }
    IntervalAttribution ia;
    ia.duration_ns = first_present - created;
    const u64 pre_end = std::min(start_ns, first_present);
    const u64 pre = created < pre_end ? pre_end - created : 0;
    const u64 from = std::max(created, start_ns);
    bool complete = !quiesce_timeout;
    Decomp d;
    if (from < first_present) {
        d = Decompose(from, first_present, roots);
        complete = complete && d.complete;
    }
    ia.cause_ns = d.cause;
    ia.cause_ns[CauseIdx(Cause::Unknown)] += pre;
    ia.status = complete ? IntervalStatus::Complete : IntervalStatus::Incomplete;
    std::scoped_lock lock{stats_mutex};
    snap.process_to_first_present = ia;
    sum_duration_ns += ia.duration_ns;
    sum_unknown_ns += ia.cause_ns[CauseIdx(Cause::Unknown)];
    snap.producer_unknown_wait_ns += d.producer_unknown_wait_ns;
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

    Decomp d;
    if (dur > 0 && (is_long || !interactive_done)) {
        const u32 root = item.root;
        d = Decompose(s, e, root != 0 ? std::span<const u32>{&root, 1} : std::span<const u32>{});
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
    // retained history and the resolver's copies of it. A ring never holds less than four events.
    const size_t ring_budget = config.buffer_bytes / 2;
    const size_t per_thread = ring_budget / MaxThreads / sizeof(RingEvent);
    s->ring_capacity = std::bit_floor(std::max<size_t>(per_thread, 4));
    s->history_cap = std::max<size_t>((config.buffer_bytes - ring_budget) / HistoryBytesPerSpan, 64);
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
}

u32 RegisterThread(std::string_view name) {
    Session* s = ActiveSession();
    if (s == nullptr) {
        return 0;
    }
    const ThreadBuf* buf = ThisThread(*s, name);
    return buf != nullptr ? buf->id : 0;
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
