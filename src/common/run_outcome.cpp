// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/run_outcome.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

#include "common/thread.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Common::RunOutcome {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t DetailCapacity = 256;
constexpr auto PollInterval = std::chrono::milliseconds{100};
/// Longer than any sane limit; keeps the deadline arithmetic inside the clock's range.
constexpr double MaxSeconds = 3.0e8;

/// Held by the timer thread, and outlives every thread that can still call Request.
struct State {
    Clock::time_point start{};
    Limits limits;
    std::optional<Clock::time_point> deadline;
    StuckHandler stuck = nullptr;

    std::mutex mutex;
    std::condition_variable wake;
    bool done = false;
    std::thread timer;
};

State& GetState() {
    static auto* state = new State;
    return *state;
}

std::atomic<bool> active{};
std::atomic<bool> stop{};
std::atomic<int> signal_number{};
std::atomic<u64> frames_seen{};
/// 0 when there is no frame limit.
std::atomic<u64> frame_target{};
std::atomic<u32> reason_bits{static_cast<u32>(Reason::None)};
std::atomic<CrashReportWriter> crash_writer{};
std::atomic_flag crash_reported;
std::stop_source& GetStopSource() {
    static auto* source = new std::stop_source;
    return *source;
}

/// The reason and its detail change together, under this flag; the critical section is a copy of
/// a few bytes, and Request must work from any thread, so it is not a mutex.
std::atomic_flag record_lock;
char detail_text[DetailCapacity]{};
size_t detail_size = 0;

struct RecordGuard {
    RecordGuard() noexcept {
        while (record_lock.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~RecordGuard() {
        record_lock.clear(std::memory_order_release);
    }
    RecordGuard(const RecordGuard&) = delete;
    RecordGuard& operator=(const RecordGuard&) = delete;
};

constexpr u32 Rank(Reason reason) noexcept {
    switch (reason) {
    case Reason::None:
        return 0;
    case Reason::GuestExit:
    case Reason::WindowClosed:
    case Reason::Requested:
    case Reason::FrameLimit:
        return 1;
    case Reason::Restart:
        return 2;
    case Reason::Timeout:
        return 3;
    case Reason::Interrupted:
        return 4;
    case Reason::StartupError:
        return 5;
    case Reason::ShaderFailure:
        return 6;
    case Reason::GuestCrash:
        return 7;
    case Reason::DeviceLost:
        return 8;
    case Reason::HostCrash:
        return 9;
    case Reason::BadArguments:
        return 10;
    case Reason::ReportIoError:
        return 11;
    }
    return 0;
}

void OnSignal(int number) {
    signal_number.store(number, std::memory_order_relaxed);
}

std::string_view SignalDetail(int number) {
    if (number == SIGINT) {
        return "interrupted by SIGINT";
    }
    if (number == SIGTERM) {
        return "interrupted by SIGTERM";
    }
#ifdef SIGBREAK
    if (number == SIGBREAK) {
        return "interrupted by Ctrl+Break";
    }
#endif
    return "interrupted by a signal";
}

void TimerMain(State& state) {
    SetCurrentThreadName("RunOutcome");
    std::unique_lock lock{state.mutex};
    std::optional<Clock::time_point> stop_seen;
    while (!state.done) {
        const auto now = Clock::now();
        // A stop that is already requested keeps its reason: a game that left cleanly just before
        // the deadline did not time out.
        if (!stop.load(std::memory_order_acquire) && state.deadline && now >= *state.deadline) {
            Request(Reason::Timeout, "the --run-seconds limit was reached");
        }
        if (StopRequested()) {
            if (!stop_seen) {
                stop_seen = now;
            }
            if (state.stuck != nullptr &&
                std::chrono::duration<double>(now - *stop_seen).count() >= StopGraceSeconds) {
                lock.unlock();
                state.stuck();
                return;
            }
        }
        auto wake_at = now + PollInterval;
        if (state.deadline && now < *state.deadline) {
            wake_at = (std::min)(wake_at, *state.deadline);
        }
        state.wake.wait_until(lock, wake_at);
    }
}

} // Anonymous namespace

std::string_view ReasonName(Reason reason) noexcept {
    switch (reason) {
    case Reason::None:
        return "none";
    case Reason::GuestExit:
        return "guest_exit";
    case Reason::WindowClosed:
        return "window_closed";
    case Reason::Requested:
        return "requested";
    case Reason::FrameLimit:
        return "frame_limit";
    case Reason::Restart:
        return "restart";
    case Reason::Timeout:
        return "timeout";
    case Reason::Interrupted:
        return "interrupted";
    case Reason::StartupError:
        return "startup_error";
    case Reason::ShaderFailure:
        return "shader_failure";
    case Reason::GuestCrash:
        return "guest_crash";
    case Reason::DeviceLost:
        return "device_lost";
    case Reason::BadArguments:
        return "bad_arguments";
    case Reason::HostCrash:
        return "host_crash";
    case Reason::ReportIoError:
        return "report_io_error";
    }
    return "none";
}

int ExitCode(Reason reason) noexcept {
    switch (reason) {
    case Reason::None:
    case Reason::GuestExit:
    case Reason::WindowClosed:
    case Reason::Requested:
    case Reason::FrameLimit:
        return 0;
    case Reason::Restart:
        return 75;
    case Reason::Timeout:
        return 124;
    case Reason::Interrupted:
        return 130;
    case Reason::StartupError:
        return 1;
    case Reason::ShaderFailure:
        return 72;
    case Reason::GuestCrash:
        return 70;
    case Reason::DeviceLost:
        return 71;
    case Reason::HostCrash:
        return 73;
    case Reason::BadArguments:
        return 64;
    case Reason::ReportIoError:
        return 74;
    }
    return 1;
}

void Begin(const Limits& limits, StuckHandler stuck) {
    State& state = GetState();
    state.start = Clock::now();
    state.limits = limits;
    if (!state.limits.seconds && state.limits.frames) {
        state.limits.seconds = FrameOnlyTimeoutSeconds;
    }
    if (state.limits.seconds) {
        const double seconds = (std::min)(*state.limits.seconds, MaxSeconds);
        state.deadline =
            state.start + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double>(seconds));
    }
    state.stuck = stuck;
    frames_seen.store(0, std::memory_order_relaxed);
    frame_target.store(state.limits.frames.value_or(0), std::memory_order_relaxed);
    active.store(true, std::memory_order_release);
    state.timer = std::thread{TimerMain, std::ref(state)};
}

void Finish() {
    State& state = GetState();
    {
        std::scoped_lock lock{state.mutex};
        state.done = true;
    }
    state.wake.notify_all();
    if (state.timer.joinable()) {
        state.timer.join();
    }
}

bool Active() noexcept {
    return active.load(std::memory_order_acquire);
}

Limits EffectiveLimits() {
    return GetState().limits;
}

double ElapsedSeconds() noexcept {
    return std::chrono::duration<double>(Clock::now() - GetState().start).count();
}

void Request(Reason reason, std::string_view detail) noexcept {
    if (!active.load(std::memory_order_acquire) || reason == Reason::None) {
        return;
    }
    {
        const RecordGuard guard;
        if (Rank(reason) > Rank(static_cast<Reason>(reason_bits.load(std::memory_order_relaxed)))) {
            reason_bits.store(static_cast<u32>(reason), std::memory_order_relaxed);
            detail_size = (std::min)(detail.size(), DetailCapacity - 1);
            if (detail_size != 0) {
                std::memcpy(detail_text, detail.data(), detail_size);
            }
        }
    }
    stop.store(true, std::memory_order_release);
    GetStopSource().request_stop();
    GetState().wake.notify_all();
}

bool StopRequested() noexcept {
    if (const int number = signal_number.exchange(0, std::memory_order_relaxed); number != 0) {
        Request(Reason::Interrupted, SignalDetail(number));
    }
    return stop.load(std::memory_order_acquire);
}

std::stop_token StopToken() noexcept {
    return GetStopSource().get_token();
}

void NoteFrame() noexcept {
    if (!active.load(std::memory_order_relaxed)) {
        return;
    }
    const u64 seen = frames_seen.fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 target = frame_target.load(std::memory_order_relaxed);
    if (target != 0 && seen >= target && !stop.load(std::memory_order_relaxed)) {
        Request(Reason::FrameLimit, "the --run-frames limit was reached");
    }
}

u64 FramesPresented() noexcept {
    return frames_seen.load(std::memory_order_relaxed);
}

Reason CurrentReason() noexcept {
    return static_cast<Reason>(reason_bits.load(std::memory_order_acquire));
}

std::string Detail() {
    const RecordGuard guard;
    return std::string{detail_text, detail_size};
}

void InstallSignalHandlers() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, OnSignal);
#endif
}

void SetCrashReportWriter(CrashReportWriter writer) noexcept {
    crash_writer.store(writer, std::memory_order_release);
}

void ReportHostCrash(std::string_view detail) noexcept {
    if (crash_reported.test_and_set(std::memory_order_acq_rel)) {
        return;
    }
    if (const CrashReportWriter writer = crash_writer.load(std::memory_order_acquire)) {
        writer(detail);
    }
}

void ExitAfterHostCrash() noexcept {
    if (!active.load(std::memory_order_acquire) &&
        crash_writer.load(std::memory_order_acquire) == nullptr) {
        return;
    }
    const int code = ExitCode(Reason::HostCrash);
#ifdef _WIN32
    // Not exit(): the crashed process may hold the loader lock or a CRT lock.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#else
    _exit(code);
#endif
}

} // namespace Common::RunOutcome
