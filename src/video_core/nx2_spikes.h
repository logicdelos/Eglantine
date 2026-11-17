// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"

// Frame spike recorder (EDEN_NX2_SPIKES=1, or =<ms> for the threshold; the MCP frame_spikes tool
// turns it on and off live). A frame is the time between two guest presents (nx2:vi2 229). A
// frame over the threshold and over `ratio` times the recent median is kept with what happened
// during it: the present's own split (the game's time, the GPU fence wait, composite, pacing),
// each GPU thread's profiler zones, the CPU time and run-queue wait of every host thread (Linux),
// the process's page faults and file reads, pipeline builds, skipped draws and the log lines.
// Each spike is also logged as one line.
namespace VideoCommon::Nx2Spikes {

using Clock = std::chrono::steady_clock;

[[nodiscard]] bool Enabled();

/// Turns recording on or off. On, it also turns on the GPU profiler (and turns it off again with
/// the recorder if it was off) and the in-memory log ring.
void SetEnabled(bool enabled);

/// A spike is a frame over `min_ms` and over `ratio` times the median of the last 120 frames.
void Configure(double min_ms, double ratio);

/// Called at every guest present: when the present began, when the GPU fence wait, composite and
/// vblank pacing ended, and the guest thread that presented (its own SVC waits are kept apart).
void OnPresent(Clock::time_point entered, Clock::time_point waited, Clock::time_point composited,
               Clock::time_point paced, u64 guest_thread);

enum class Event : u32 {
    DriverBuild,     ///< A vkCreate*Pipelines call (any thread)
    DriverCompile,   ///< Of those, over 10 ms: compiled, not from the driver's cache
    RuntimePipeline, ///< A pipeline translated and built on the GPU thread
    DrawSkipped,     ///< A draw skipped while its pipeline translates
    CpuWriteSlowPath,   ///< Guest CPU writes to GPU-cached pages (amount: bytes)
    CpuWriteInvalidate, ///< Of those, ones that reached the GPU caches (OnCPUWrite)
    CpuWriteLockWait,   ///< OnCPUWrite waiting for the buffer or texture cache lock
    ProtectChange,      ///< Host page protection changes for the GPU caches (amount: pages)
    ReadTrapHit,        ///< Guest CPU reads that hit a read trap
    JitCompile,         ///< Guest code blocks compiled (polled from the JIT)
    JitFastmemRecompile, ///< Blocks recompiled after a fastmem fault (polled from the JIT)
    JitInvalidate,      ///< Guest code cache range invalidations (amount: bytes)
    JitClear,           ///< Whole guest code cache clears
    Svc,                ///< Every SVC (time: inside the call, blocked time included)
    SvcSleep,           ///< SleepThread, yields included
    SvcWaitSync,        ///< WaitSynchronization
    SvcMutexCondvar,    ///< ArbitrateLock/Unlock, WaitProcessWideKeyAtomic, SignalProcessWideKey
    SvcAddressArbiter,  ///< WaitForAddress, SignalToAddress
    SvcIpc,             ///< SendSyncRequest(WithUserBuffer), ReplyAndReceive(WithUserBuffer)
    FileRead,           ///< A host file read for the guest (amount: bytes asked for)
    NupDecompress,      ///< A NUP member decompressed to serve a read (amount: bytes out)
    CpuReadSync,        ///< A guest CPU read of GPU-written memory waiting for the GPU thread
    Count,
};

/// True while the recorder or a Window counts events: callers can skip timing work otherwise.
[[nodiscard]] bool Counting();

/// A cheap host time stamp in nanoseconds (unfenced TSC) for timing short scopes such as SVCs,
/// and the time since one. Not related to Clock.
[[nodiscard]] u64 Stamp();
[[nodiscard]] Clock::duration Since(u64 stamp);

/// Counts an event (any thread), with the time it took and an amount when there is one.
void Note(Event event, Clock::duration time = {}, u64 amount = 0);

/// Counts an SVC wait of one guest thread (svc.cpp, with the SvcSleep, SvcWaitSync, SvcMutexCondvar,
/// SvcAddressArbiter and SvcIpc events only): OnPresent reports the presenting thread's own.
void NoteGuestThread(u64 guest_thread, Event event, Clock::duration time);

/// Times a scope into Note(event, time, amount); free when nothing counts.
class Scope {
public:
    explicit Scope(Event event_, u64 amount_ = 0)
        : event{event_}, amount{amount_}, start{Counting() ? Clock::now() : Clock::time_point{}} {}
    ~Scope() {
        if (start != Clock::time_point{}) {
            Note(event, Clock::now() - start, amount);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Event event;
    u64 amount;
    Clock::time_point start;
};

/// An event counted elsewhere (the JIT's own totals): read at each present and window instead of
/// counted with Note. The functions return running totals.
void SetPolled(Event event, u64 (*count)(), u64 (*ns)());

/// A guest thread's CPU ticks so far.
struct GuestThreadTicks {
    u64 id{};
    u64 ticks{};
    s32 core{};
};

/// Where guest thread CPU times come from (set by the core while a program runs).
struct GuestThreadSource {
    /// Every guest thread's CPU ticks and the guest clock's ticks now; false when there is no
    /// program.
    bool (*read)(void* user, std::vector<GuestThreadTicks>& threads, u64& clock){};
    /// A thread's name ("" when gone).
    std::string (*name)(void* user, u64 id){};
    void* user{};
};

/// Sets or (with an empty source) clears the guest thread source; clearing waits for readers.
void SetGuestThreadSource(GuestThreadSource source);

struct ZoneTime {
    std::string zone;
    double ms{};
    u64 calls{};
};

struct GpuThreadTimes {
    std::string thread;
    /// Costliest first. "other (no zone)" is time in no zone, or in one still running at the
    /// present (it counts when it ends, in the next frame).
    std::vector<ZoneTime> zones;
    std::vector<std::pair<std::string, u64>> counters;
};

struct HostThreadTimes {
    std::string name;
    int tid{};
    double cpu_ms{};       ///< Time on a CPU
    double run_queue_ms{}; ///< Time runnable but waiting for a CPU
};

struct EventTimes {
    std::string event;
    u64 count{};
    double ms{};
    u64 amount{};
    const char* unit{}; ///< What amount counts, or null when the event has none
};

struct GuestThreadTimes {
    u64 id{};
    std::string name;
    s32 core{};
    double cpu_ms{}; ///< Guest CPU time (the kernel's accounting, scaled to host ms)
};

struct Spike {
    u64 number{};
    u64 present{};
    double at_s{}; ///< Seconds since the recorder started, at the end of the frame
    double frame_ms{};
    double median_ms{};
    // The frame runs from the previous present's start to this one's: the previous present's
    // GPU fence wait, composite and pacing, then the game until it presents again. They add up
    // to frame_ms.
    double game_ms{};      ///< From the previous present's return to this present
    double fence_ms{};     ///< The previous present waiting for the GPU thread
    double composite_ms{};
    double pace_ms{};
    std::vector<GpuThreadTimes> gpu;
    bool host_sampled{};       ///< Host thread times available (Linux)
    double host_span_ms{};     ///< What the host samples covered (they bracket the frame)
    std::vector<HostThreadTimes> host; ///< Busiest first, and any that waited for a CPU
    u64 minor_faults{};
    u64 major_faults{};
    u64 read_kib{};
    std::vector<EventTimes> events;
    std::vector<GuestThreadTimes> guest; ///< Busiest guest threads first
    // The guest thread that presented this frame: what it did itself. Its CPU time plus its SVC
    // waits against game_ms says whether the game ran, waited on other guest threads, or sat in
    // service calls; what is left over is time it was not scheduled or ran off the host.
    u64 presenter{};                 ///< 0 when there is no per-thread data
    std::string presenter_name;
    double presenter_cpu_ms{-1.0};   ///< Guest CPU time over the frame; negative when unknown
    std::vector<EventTimes> presenter_waits; ///< Its own SVC waits over the frame
    std::vector<std::string> log; ///< Log lines written during the frame (the last ones)
};

struct Report {
    bool enabled{};
    double min_ms{};
    double ratio{};
    u64 presents{};
    double median_ms{};
    u64 spikes{};
    std::vector<Spike> recent; ///< Newest first
    std::vector<Spike> worst;  ///< Longest first
};

[[nodiscard]] Report Read(size_t limit);

/// What happened over a measured window (perf_sample).
struct WindowReport {
    double seconds{};
    std::vector<EventTimes> events;
    std::vector<GuestThreadTimes> guest;
    bool host_sampled{};
    std::vector<HostThreadTimes> host;
    u64 minor_faults{};
    u64 major_faults{};
    u64 read_kib{};
};

/// Counts events from construction to Finish, independently of the recorder.
class Window {
public:
    Window();
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// The events, busiest guest and host threads and page faults since construction.
    [[nodiscard]] WindowReport Finish(size_t max_threads = 12);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

void Clear();

/// The spike as the one line the recorder logs.
[[nodiscard]] std::string Summary(const Spike& spike);

// ---------------------------------------------------------------------------------------------
// Attribution (milestone 0.5): which cause every long frame and the cold start are charged to.
//
// This block is a frozen contract (plan_0.5.md section 2, contracts_0.5.md). It extends this
// recorder instead of adding another one; the Event counters above keep their meaning and the
// frame spike recorder above is not changed by it. Nothing here is implemented by the header:
// recorder code lives in nx2_attribution.cpp (and nx2_spikes.cpp), callers only use these
// declarations. Common code (Nx2Bottleneck) does not include this file, and Dynarmic reaches it
// only through the hook of dynarmic/interface/jit_stats.h.
//
// Clock: every timestamp is nanoseconds of std::chrono::steady_clock (NowNs), the clock of the
// run report; durations are integer nanoseconds. A frame is the interval between two real,
// successful host presents (OnHostPresent): F_i = (P(i-1), P(i)], duration = P(i) - P(i-1). It is
// a long frame when the duration is strictly over the threshold (100 ms by default).
//
// Off cost: AttributionEnabled() is one relaxed load of a plain atomic bool. Every site tests it
// once (SpanScope does) and does nothing else when it is false: no clock read, no allocation, no
// queue operation.
// ---------------------------------------------------------------------------------------------

/// What a stretch of time is charged to (the roadmap's list). The value is the index into
/// LongFrame::cause_ns and activity_ns.
enum class Cause : u8 {
    Disk,            ///< A synchronous file read for the guest or the shader caches
    Shader,          ///< Shader environment capture, translation, SPIR-V emission, module creation
    Pipeline,        ///< Driver pipeline object creation and pipeline preparation
    Texture,         ///< CPU ASTC/BC decode, GPU decode submission
    Decompression,   ///< NUP / zstd decompression (not the file reads nested inside it)
    Waiting,         ///< A wait whose producer is known or unknown (see SpanFlags)
    CodeTranslation, ///< The guest JIT translating, optimizing and emitting code
    Unknown,         ///< Time no span explains
    Count,
};
inline constexpr size_t CauseCount = static_cast<size_t>(Cause::Count);

/// The names reports and log lines use, indexed by Cause.
inline constexpr std::array<std::string_view, CauseCount> CauseNames{
    "disk",          "shader",  "pipeline",        "texture",
    "decompression", "waiting", "code_translation", "unknown",
};
[[nodiscard]] constexpr std::string_view CauseName(Cause cause) noexcept {
    return CauseNames[static_cast<size_t>(cause)];
}

/// The stable names of the Event counters in reports (the counter_deltas of a long frame), indexed
/// by Event. The recorder's own human readable names are not used for machine output.
inline constexpr std::array<std::string_view, static_cast<size_t>(Event::Count)> EventTokens{
    "driver_build",   "driver_compile",   "runtime_pipeline",   "draw_skipped",
    "cpu_write_slow_path", "cpu_write_invalidate", "cpu_write_lock_wait", "protect_change",
    "read_trap_hit",  "jit_compile",      "jit_fastmem_recompile", "jit_invalidate",
    "jit_clear",      "svc",              "svc_sleep",          "svc_wait_sync",
    "svc_mutex_condvar", "svc_address_arbiter", "svc_ipc",      "file_read",
    "nup_decompress", "cpu_read_sync",
};
[[nodiscard]] constexpr std::string_view EventToken(Event event) noexcept {
    return EventTokens[static_cast<size_t>(event)];
}

/// A stable 64-bit session-local identity for a unit of work (a pipeline build, a read request);
/// 0 means unlinked. Allocated with NewWorkId and carried from enqueue to the worker that runs it.
using WorkId = u64;
[[nodiscard]] WorkId NewWorkId();

/// Kinds of derived work ids, for waits and the spans they wait for that share an identity both
/// sides already know instead of a stored NewWorkId.
enum class WorkKind : u64 { GraphicsPipelineBuild = 1, ComputePipelineBuild = 2, Submission = 3 };

/// A WorkId computed from an identity both sides know (pipeline key hash, submission tick). The
/// kind sits in the top 3 bits, so it never equals 0 and never meets a NewWorkId value (NewWorkId
/// counts up from 1 and stays below 2^61).
[[nodiscard]] constexpr WorkId DerivedWorkId(WorkKind kind, u64 identity) noexcept {
    return (static_cast<u64>(kind) << 61) | (identity & ((u64{1} << 61) - 1));
}

/// Names one span for EndSpan and LinkWait. Valid until the session ends; a token of an earlier
/// session generation is ignored.
struct SpanToken {
    u32 session_generation{};
    u32 thread_registration_id{};
    u64 sequence{}; ///< 0: no span (recording was off)

    [[nodiscard]] constexpr bool Valid() const noexcept {
        return sequence != 0;
    }
    friend constexpr bool operator==(const SpanToken&, const SpanToken&) = default;
};

/// Span::flags bits.
namespace SpanFlags {
inline constexpr u32 Incomplete = 1u << 0;      ///< Still open when the session ended
inline constexpr u32 Failed = 1u << 1;          ///< The operation failed
inline constexpr u32 Cancelled = 1u << 2;       ///< The operation was cancelled
inline constexpr u32 ProducerUnknown = 1u << 3; ///< A wait that no LinkWait connected to work
inline constexpr u32 GuestWait = 1u << 4; ///< A wait for the guest: see MarkGuestThread
} // namespace SpanFlags

/// One finished or open stretch of work on one thread. `parent` is the span open on the same
/// thread when this one began (recorder-assigned; nested time is subtracted from the parent).
struct Span {
    SpanToken token;
    SpanToken parent;
    WorkId work{};
    Cause cause{Cause::Unknown};
    u32 flags{};
    u64 begin_ns{};
    u64 end_ns{}; ///< 0 while open
    u64 amount{}; ///< Bytes, blocks or entries, whatever the cause counts
};

enum class DependencyKind : u8 {
    Job,   ///< A queued job (pipeline build, translation) released the wait
    Fence, ///< A GPU submission fence or timeline value
    Join,  ///< A thread or worker join
    Io,    ///< A read or decompression request
};

/// A wait span and the work whose completion released it.
struct Dependency {
    SpanToken wait_span;
    WorkId producer_work{};
    u64 release_ns{};
    DependencyKind kind{DependencyKind::Job};
};

/// The result of one host present attempt, from the Vulkan call's VkResult.
enum class PresentResult : u8 {
    Success,
    Suboptimal,
    OutOfDate,
    Error,
};

/// VK_SUCCESS, VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR by value, so no Vulkan header is
/// needed here.
[[nodiscard]] constexpr PresentResult ClassifyPresent(s32 vk_result) noexcept {
    if (vk_result == 0) {
        return PresentResult::Success;
    }
    if (vk_result == 1000001003) {
        return PresentResult::Suboptimal;
    }
    if (vk_result == -1000001004) {
        return PresentResult::OutOfDate;
    }
    return PresentResult::Error;
}

/// Only a successful or suboptimal submission of a real frame ends a frame interval.
[[nodiscard]] constexpr bool QualifiesAsHostPresent(PresentResult result, bool real_frame) noexcept {
    return real_frame && (result == PresentResult::Success || result == PresentResult::Suboptimal);
}

/// One host present attempt. The renderer fills everything but host_present_id.
struct FrameBoundary {
    u64 host_present_id{}; ///< Assigned by the recorder, monotonic from 1, for qualifying presents
    u64 guest_frame_id{};  ///< The guest frame that was presented, 0 when unknown
    u64 timestamp_ns{};    ///< NowNs() taken right after the present call returned
    bool real_frame{};     ///< False for a generated (frame generation) frame
    PresentResult result{PresentResult::Success};
};

/// LongFrame::unknown_reasons bits: why part of a frame is Unknown.
namespace UnknownReason {
inline constexpr u32 PreInstrumentation = 1u << 0;
inline constexpr u32 UnlinkedGuest = 1u << 1;
inline constexpr u32 UninstrumentedDriver = 1u << 2;
inline constexpr u32 SchedulerOrPaging = 1u << 3;
inline constexpr u32 MissingGpuTimestamp = 1u << 4;
inline constexpr u32 AmbiguousDependency = 1u << 5;
inline constexpr u32 LostSpans = 1u << 6;
inline constexpr u32 ProducerUnknownWait = 1u << 7;
/// Part of the frame is time the presenting thread has no span for, charged by what the other
/// threads were doing (or Waiting): an inference, not a measurement.
inline constexpr u32 PresenterGap = 1u << 8;
} // namespace UnknownReason

/// The names reports use, indexed by the bit number of an UnknownReason.
inline constexpr std::array<std::string_view, 9> UnknownReasonNames{
    "pre_instrumentation", "unlinked_guest",     "uninstrumented_driver", "scheduler_or_paging",
    "missing_gpu_timestamp", "ambiguous_dependency", "lost_spans",          "producer_unknown_wait",
    "presenter_gap",
};

inline constexpr size_t MaxCriticalWork = 8;

/// Where the Waiting time of a frame went, by what the guest was doing while the waiting thread
/// waited (the order of WaitClassNames, the index into a WaitSplit). Each stretch of Waiting is
/// in exactly one class, so a WaitSplit sums to the Waiting it splits.
///  * GuestRunning: a guest CPU core thread was executing translated guest code.
///  * GuestIdle: every guest core thread that reported was idle (sleep, timer, vsync wait).
///  * Service: no core was running, and a host-side service request handler was executing (the
///    guest thread that sent it is blocked until it returns).
///  * HostOther: host scheduling, locks and system call handling on a core thread that none of
///    the above covers, and time before any state was reported.
/// Where several apply at an instant: GuestRunning first, then Service, then HostOther; GuestIdle
/// only when every thread that reported is idle.
enum class WaitClass : u8 { GuestRunning, GuestIdle, Service, HostOther, Count };
inline constexpr size_t WaitClassCount = static_cast<size_t>(WaitClass::Count);
inline constexpr std::array<std::string_view, WaitClassCount> WaitClassNames{
    "guest_running", "guest_idle", "service", "host_other",
};
using WaitSplit = std::array<u64, WaitClassCount>;

/// Time of the Service class spent in one service command, named by the service and the command
/// number (the high bit of `command` is set for a TIPC command): no game names, only the
/// interface the guest called.
struct WaitHandlerTime {
    u32 service{}; ///< An id of InternServiceName, 0 when unknown
    u32 command{};
    u64 ns{};
};
/// The handlers a long frame names (the longest first), and the session totals.
inline constexpr size_t MaxWaitHandlers = 4;
inline constexpr size_t MaxTotalWaitHandlers = 8;

/// One frame over the threshold. cause_ns is the exclusive wall-time decomposition of the frame
/// and sums exactly to duration_ns (unresolved time is Unknown); activity_ns is the clipped sum
/// over all threads and may exceed the duration. Fixed size: streamed to the report writer.
struct LongFrame {
    u64 id{};          ///< host_present_id of the present that ended the frame
    u64 start_ns{};    ///< The previous qualifying present
    u64 end_ns{};      ///< This present
    u64 duration_ns{}; ///< end_ns - start_ns
    std::array<u64, CauseCount> cause_ns{};
    std::array<u64, CauseCount> activity_ns{};
    /// The part of cause_ns that is inferred, not observed: time the presenting thread has no span
    /// for, charged by what the other threads did (UnknownReason::PresenterGap). Per cause, so a
    /// table can mark it; cause_ns - inferred_ns is what spans cover.
    std::array<u64, CauseCount> inferred_ns{};
    /// How the Waiting part of cause_ns splits by guest activity; sums to cause_ns[Waiting].
    /// waiting_observed is false when no core or service thread reported a state in the session
    /// (then everything would read HostOther, which says nothing).
    WaitSplit waiting_ns{};
    std::array<WaitHandlerTime, MaxWaitHandlers> waiting_handlers{}; ///< Service time, longest first
    u32 waiting_handler_count{};
    bool waiting_observed{};
    Cause primary_cause{Cause::Unknown}; ///< The largest cause_ns, Unknown on ties
    u32 confidence_permille{};           ///< 1000 - 1000 * Unknown / duration
    u32 unknown_reasons{};               ///< UnknownReason bits
    /// Event counts during the frame (the existing Note counters, not their times).
    std::array<u64, static_cast<size_t>(Event::Count)> counter_deltas{};
    std::array<WorkId, MaxCriticalWork> critical_work_ids{}; ///< Longest first
    u32 critical_work_count{};
    PresentResult present_result{PresentResult::Success};
    bool instrumentation_complete{}; ///< No lost or still-open spans touched the frame
};

/// The monotonic clock of every attribution timestamp.
[[nodiscard]] inline u64 NowNs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

namespace Detail {
inline std::atomic<bool> attribution_enabled{false}; ///< Set by BeginSession, cleared by EndSession
} // namespace Detail

/// True between BeginSession and EndSession. The one branch every site pays when recording is off.
[[nodiscard]] inline bool AttributionEnabled() noexcept {
    return Detail::attribution_enabled.load(std::memory_order_relaxed);
}

struct SessionConfig {
    u64 threshold_ns{100'000'000};
    /// What the recorder may hold in total, shared by all threads; spans past it are counted as
    /// lost. Half goes to the per-thread rings. The other half goes to the retained spans and the
    /// storage used to resolve them: a retained span slot is charged its record and the most
    /// working storage resolving it can need, about 584 bytes on a 64-bit host (an estimate, not an
    /// allocator measurement), after a fixed 16 KiB for what does not grow with the spans. The
    /// history never has fewer than 64 slots and a ring never fewer than four events, so budgets
    /// below about 100 KiB are exceeded.
    size_t buffer_bytes{size_t{16} << 20};
    /// Charge the time the presenting thread has no span for by what the other threads were doing
    /// (UnknownReason::PresenterGap) instead of leaving it Unknown. Off: the strict decomposition.
    bool charge_presenter_gaps{true};
};

/// Starts a session: a new generation, empty buffers, recording on. False when one is running.
/// Called by the frontend after option validation (CLI) or before the emulation thread runs (Qt).
bool BeginSession(const SessionConfig& config);

/// Ends the session: still-open spans become incomplete spans, the consumer is drained, producer
/// completion is awaited and the final snapshot becomes readable (until the next BeginSession).
/// Called after every producer thread has been joined.
void EndSession();

/// Gives the calling thread a collision-free registration (once per thread; later calls return
/// the first id). A thread that records without registering is registered as "unnamed".
u32 RegisterThread(std::string_view name);

/// Marks the calling thread as one that runs guest code (a CPU core thread). A wait recorded with
/// SpanFlags::GuestWait is charged to the causes the guest threads were in while it lasted.
/// Cheap and idempotent; call it where the thread runs guest code. Call only when
/// AttributionEnabled().
void MarkGuestThread();

/// Opens a span on the calling thread and returns its token (invalid when recording is off).
/// The span open on this thread becomes its parent. Call only when AttributionEnabled().
SpanToken BeginSpan(Cause cause, WorkId work = 0, u64 amount = 0);

/// Closes the span, adding `amount` to the one given at BeginSpan, and ORing in SpanFlags.
void EndSpan(SpanToken token, u64 amount = 0, u32 flags = 0);

/// Records a span that is already over (the JIT hook, a timestamped GPU result).
void RecordSpan(Cause cause, u64 begin_ns, u64 end_ns, WorkId work = 0, u64 amount = 0,
                u32 flags = 0);

/// States that `wait` ended because `producer` finished. Time of the wait that overlaps the
/// producer's spans is charged to the producer's cause; an unlinked wait stays Waiting.
void LinkWait(SpanToken wait, WorkId producer, DependencyKind kind);

/// Times a scope into BeginSpan/EndSpan; costs one atomic load when recording is off.
class SpanScope {
public:
    explicit SpanScope(Cause cause, WorkId work = 0, u64 amount = 0)
        : token{AttributionEnabled() ? BeginSpan(cause, work, amount) : SpanToken{}} {}
    ~SpanScope() {
        if (token.Valid()) {
            EndSpan(token, end_amount, flags);
        }
    }
    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;

    [[nodiscard]] SpanToken Token() const noexcept {
        return token;
    }
    /// Adds to the amount reported at the end (bytes read, blocks decoded).
    void AddAmount(u64 value) noexcept {
        end_amount += value;
    }
    void Fail() noexcept {
        flags |= SpanFlags::Failed;
    }

private:
    SpanToken token;
    u64 end_amount{};
    u32 flags{};
};

/// Waits shorter than this are not recorded by LongWaitScope: a thread that polls for work can
/// idle thousands of times a second, and a span per poll would cost more than the attribution
/// gains. The time of a dropped wait is not charged to Waiting (the thread was about to work), so
/// a thread that polls faster than this shows the polls as time without a span.
inline constexpr u64 LongWaitMinNs = 50'000;

/// Records one Waiting span for an idle wait of the calling thread, only when it lasted at least
/// LongWaitMinNs, as a single event when the wait ends (no event at the start). With
/// SpanFlags::GuestWait the wait is a wait for the guest. Costs one relaxed load when recording is
/// off, and two clock reads when on.
class LongWaitScope {
public:
    explicit LongWaitScope(u32 wait_flags = 0)
        : begin_ns{AttributionEnabled() ? NowNs() : 0}, flags{wait_flags} {}
    ~LongWaitScope() {
        End();
    }
    LongWaitScope(const LongWaitScope&) = delete;
    LongWaitScope& operator=(const LongWaitScope&) = delete;

    /// Ends the wait now (the destructor does nothing afterwards).
    void End() {
        if (begin_ns == 0) {
            return;
        }
        const u64 start = std::exchange(begin_ns, 0);
        if (!AttributionEnabled()) {
            return;
        }
        const u64 end = NowNs();
        if (end - start >= LongWaitMinNs) {
            RecordSpan(Cause::Waiting, start, end, 0, 0, flags);
        }
    }

private:
    u64 begin_ns;
    u32 flags;
};

/// What the calling thread is doing, for the split of Waiting (WaitClass). A guest CPU core thread
/// reports Running while it executes translated code, Idle while its core has nothing to run, and
/// Other on its host side (system calls, scheduling, locks); a thread that serves a request of a
/// guest service reports Service for the handler's duration and Off between requests. A thread
/// that never reports is not part of the split.
enum class GuestState : u8 { Off, Running, Idle, Service, Other };

/// A state with the handler it carries (a Service state only).
struct GuestMark {
    GuestState state{GuestState::Off};
    u32 service{};
    u32 command{};
};

/// Records that the calling thread is in `state` from `ns` (0: now, read inside the publication, as
/// BeginSpan does) and returns the state it was in. A change to the state it is in already, with
/// the same handler, records nothing. Call only when AttributionEnabled(). Cost: one clock read
/// and one 64-byte ring store, no lock and no allocation.
GuestMark RecordGuestState(GuestState state, u64 ns = 0, u32 service = 0, u32 command = 0);

/// Interns a service name for the handler of a Service state and returns its id (stable for the
/// process, never 0), or 0 when the table (4096 names) is full. Takes a lock: callers cache the id.
[[nodiscard]] u32 InternServiceName(std::string_view name);

/// The name of an id of InternServiceName ("unknown" for 0 or an id never given).
[[nodiscard]] std::string ServiceNameOf(u32 id);

/// Reports the calling thread's state for a scope and puts back the one it had: a service handler
/// on a thread that has a state of its own (a core thread) must not erase it. One relaxed load
/// when recording is off.
class GuestStateScope {
public:
    explicit GuestStateScope(GuestState state, u32 service = 0, u32 command = 0) {
        if (AttributionEnabled()) {
            previous = RecordGuestState(state, 0, service, command);
            active = true;
        }
    }
    ~GuestStateScope() {
        if (active && AttributionEnabled()) {
            RecordGuestState(previous.state, 0, previous.service, previous.command);
        }
    }
    GuestStateScope(const GuestStateScope&) = delete;
    GuestStateScope& operator=(const GuestStateScope&) = delete;

private:
    GuestMark previous;
    bool active{};
};

/// Reports the calling thread's state until it reports another. One relaxed load when recording is
/// off.
inline void NoteGuestState(GuestState state) {
    if (AttributionEnabled()) {
        RecordGuestState(state);
    }
}

/// Publishes a host present attempt. Returns its host_present_id when it qualifies
/// (QualifiesAsHostPresent), 0 otherwise; a non-qualifying attempt is only counted (and keeps
/// the interval open, it never ends or starts one).
u64 OnHostPresent(const FrameBoundary& boundary);

/// ORs UnknownReason bits into the frame that is open now (counted against its unknown time and
/// reported in unknown_reasons); a no-op without a session or an open frame. Used where a cause
/// cannot be measured, for example MissingGpuTimestamp for GPU work without timestamp queries.
void NoteUnknownReason(u32 unknown_reason_bits);

/// A cold-start phase of the frontend (the startup.phases list of the report).
enum class BootPhase : u8 {
    FrontendLoad,   ///< Frontend loads the program, up to the first thing below
    WarmRefresh,    ///< Guessed warm-compute archive scan and conversion
    CacheValidate,  ///< Cache header read, identity check and indexing
    PipelineReplay, ///< Starting and waiting for disk replay at boot
    GameRun,        ///< Until the guest program runs
    Count,
};
inline constexpr std::array<std::string_view, static_cast<size_t>(BootPhase::Count)>
    BootPhaseNames{"frontend_load", "warm_refresh", "cache_validate", "pipeline_replay",
                   "game_run"};

enum class PhaseOutcome : u8 { NotRun, Ok, Cancelled, Error };

void BeginBootPhase(BootPhase phase);
void EndBootPhase(BootPhase phase, PhaseOutcome outcome);

/// When the OS created the process, on the NowNs clock (the frontend maps it at initialization)
/// and how uncertain that mapping is. Time before the session began is Unknown/pre_instrumentation.
void SetProcessCreated(u64 created_ns, u64 uncertainty_ns);

/// The verified interactive-scene marker (the lab supplies it, F validates the session nonce).
struct InteractiveMarker {
    std::string scene_id;
    std::string origin; ///< Who supplied it ("lab", "manual")
    u64 marker_ns{};    ///< On the NowNs clock
    bool scene_check_passed{};
};

enum class MarkerResult : u8 {
    Accepted,
    Duplicate,    ///< A marker was already accepted this session
    Backdated,    ///< marker_ns is before the first host present or after now
    NoSession,
};
[[nodiscard]] MarkerResult MarkInteractive(const InteractiveMarker& marker);

/// Whether an interval of the report was observed completely.
enum class IntervalStatus : u8 { NotObserved, Complete, Incomplete };

struct IntervalAttribution {
    u64 duration_ns{};
    std::array<u64, CauseCount> cause_ns{}; ///< Sums to duration_ns
    IntervalStatus status{IntervalStatus::NotObserved};
    WaitSplit waiting_ns{}; ///< Sums to cause_ns[Waiting]; meaningful when the snapshot says so
};

struct PhaseRecord {
    BootPhase phase{BootPhase::FrontendLoad};
    PhaseOutcome outcome{PhaseOutcome::NotRun};
    u64 begin_ns{};
    u64 end_ns{}; ///< 0 while running
};

/// The first two minutes after the first real host present (strictly over the threshold counts,
/// the lower bound is exclusive and the upper inclusive).
inline constexpr u64 FirstWindowNs = 120'000'000'000ull;
struct FirstWindow {
    u64 anchor_ns{}; ///< The first qualifying present, 0 before it
    bool complete{}; ///< A present crossed anchor + FirstWindowNs
    u64 elapsed_ns{};
    u64 frames{};
    u64 frames_over_threshold{};
    u64 max_ns{};
    u64 crossing_tail_interval_ns{}; ///< The interval that crossed the window's end
};

/// Everything the report needs from the recorder, immutable.
struct AttributionSnapshot {
    bool enabled{};
    bool complete{}; ///< No lost spans, no write failure, session ended with producers joined
    u64 threshold_ns{};
    u64 lost_events{};
    u64 dropped_boundaries{}; ///< Present boundaries lost; each leaves an unattributed gap
    u64 sink_write_errors{};
    u64 rejected_presents{}; ///< Present attempts that did not qualify
    u64 frames{};            ///< Qualifying intervals (from the second present on)
    u64 long_frames{};
    double unknown_wall_fraction{};          ///< Over all long frames and both startup intervals
    double unknown_primary_frame_fraction{}; ///< Long frames whose largest bucket is Unknown
    u64 producer_unknown_wait_ns{};
    /// The inferred part of the causes over all long frames and the startup intervals (the sum of
    /// LongFrame::inferred_ns plus the start-up's), per cause.
    std::array<u64, CauseCount> inferred_ns{};
    /// How the Waiting part of the causes splits (WaitClass), over the same intervals as
    /// inferred_ns, and the service commands that took most of its Service class (the longest
    /// first, at most MaxTotalWaitHandlers). waiting_observed: a core or service thread reported a
    /// state, so the split says something; otherwise it is all HostOther and the report omits it.
    WaitSplit waiting_ns{};
    std::vector<WaitHandlerTime> waiting_handlers;
    bool waiting_observed{};
    u64 process_created_ns{}; ///< 0: not set
    u64 process_created_uncertainty_ns{};
    u64 first_host_present_ns{}; ///< 0: no present yet
    IntervalAttribution process_to_first_present;
    IntervalAttribution first_present_to_interactive;
    InteractiveMarker interactive;
    IntervalStatus interactive_status{IntervalStatus::NotObserved};
    std::vector<PhaseRecord> phases; ///< In the order begun, at most 64
    FirstWindow first_120s;
};

/// Valid while a session runs (then incomplete) and after EndSession (final).
[[nodiscard]] AttributionSnapshot ReadAttributionSnapshot();

/// Where long frames go as they are resolved: called on the recorder's one consumer thread, once
/// per long frame, in order. `write` returns false when the record could not be stored (counted
/// in sink_write_errors, the snapshot becomes incomplete). Set once, before BeginSession;
/// clearing (an empty sink) waits for a call in progress.
struct LongFrameSink {
    bool (*write)(void* user, const LongFrame& frame){};
    void* user{};
};
void SetLongFrameSink(LongFrameSink sink);

/// The line the recorder logs for every long frame, for example
/// "Nx2LongFrame frame=123 dt_ms=154.2 primary=shader shader_ms=92.1 waiting_ms=40.0
/// unknown_ms=22.1 complete=1" (milliseconds with one decimal; every cause is printed, in
/// CauseNames order, as <name>_ms).
[[nodiscard]] std::string LongFrameLine(const LongFrame& frame);

/// Crash-path status: relaxed atomic loads only, no allocation, no lock. Valid at any time (zeros
/// before the first session).
struct CrashStatus {
    bool session_active{};
    bool complete{}; ///< Same meaning as AttributionSnapshot::complete, computed so far
    u64 threshold_ns{};
    u64 lost_events{};
    u64 frames{};
    u64 long_frames{};
    u64 first_host_present_ns{};
    u64 last_host_present_ns{};
    u64 last_long_frame_id{}; ///< 0: none yet
    u64 last_long_frame_ns{}; ///< Duration of that frame
};
[[nodiscard]] CrashStatus ReadCrashStatus() noexcept;

} // namespace VideoCommon::Nx2Spikes
