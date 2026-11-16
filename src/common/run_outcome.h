// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include "common/common_types.h"

// How an unattended run ends, for the command line's --run-seconds, --run-frames and --report.
// Everything here is inert until Begin: a normal interactive run never reads or changes it.
//
// Threads that notice something (the guest, the renderer, a timer, a signal) only call Request
// or NoteFrame, which record the reason and ask for a stop. The main thread of the frontend polls
// StopRequested, tears everything down in order and then reads the reason.
namespace Common::RunOutcome {

/// Why a run ended. The code of each is in ExitCode; the name is the one the JSON report uses.
enum class Reason : u32 {
    None,
    GuestExit,     // the game left (System::Exit)
    WindowClosed,  // the user closed the window
    Requested,     // a quit from the live keys file or the MCP server
    FrameLimit,    // --run-frames
    Restart,       // the game asked to be started again
    Timeout,       // --run-seconds, or the safety timeout of a run limited by frames only
    Interrupted,   // SIGINT, SIGTERM
    StartupError,  // the game did not load
    ShaderFailure, // a pipeline the game cannot go without failed to build
    GuestCrash,    // the game broke, or hit a fatal error
    DeviceLost,    // the Vulkan device was lost
    HostCrash,     // the emulator itself crashed (written by the crash handler, never requested)
    BadArguments,  // set by the frontend, never requested
    ReportIoError, // set by the frontend, never requested
};

/// The bounds of a run; a missing member is unbounded.
struct Limits {
    std::optional<double> seconds;
    std::optional<u64> frames;
};

/// A run bounded by frames only still gets this many seconds, so a stuck game cannot hang a lab.
inline constexpr double FrameOnlyTimeoutSeconds = 900.0;
/// A stop that takes longer than this calls the handler given to Begin.
inline constexpr double StopGraceSeconds = 30.0;

using StuckHandler = void (*)() noexcept;

[[nodiscard]] std::string_view ReasonName(Reason reason) noexcept;
[[nodiscard]] int ExitCode(Reason reason) noexcept;

/// Starts the run clock, arms the limits and the timer thread. `stuck` runs on the timer thread
/// when StopGraceSeconds pass after a stop was requested without the run having finished; it is
/// meant to publish what is known and end the process. Call once, before the game starts.
void Begin(const Limits& limits, StuckHandler stuck);

/// Stops the timer thread and ends the grace period. Not from the timer thread.
void Finish();

/// True between Begin and the end of the process.
[[nodiscard]] bool Active() noexcept;

/// The limits Begin armed, with the safety timeout filled in.
[[nodiscard]] Limits EffectiveLimits();

/// Wall-clock seconds since Begin.
[[nodiscard]] double ElapsedSeconds() noexcept;

/// Records `reason` if no reason of higher precedence is recorded yet, and asks for a stop. The
/// precedence is DeviceLost, GuestCrash, ShaderFailure, StartupError, Interrupted, Timeout,
/// Restart, then the normal reasons (the first of those wins). Does nothing before Begin.
void Request(Reason reason, std::string_view detail = {}) noexcept;

/// True once a stop was requested. A caught signal counts, and is recorded as Interrupted.
[[nodiscard]] bool StopRequested() noexcept;

/// Stopped by the same request, for loops that take a token (loading shader caches).
[[nodiscard]] std::stop_token StopToken() noexcept;

/// One finished game frame. Asks for a stop when the frame limit is reached.
void NoteFrame() noexcept;

/// The reason on record, None when nothing was requested.
[[nodiscard]] Reason CurrentReason() noexcept;
/// The detail given with the reason on record.
[[nodiscard]] std::string Detail();

/// Makes SIGINT and SIGTERM (and Ctrl+Break on Windows) request a stop. The handlers only set a
/// flag, which StopRequested converts. Call after anything that installs handlers of its own.
void InstallSignalHandlers();

/// Frames presented since Begin, counted whether or not there is a frame limit.
[[nodiscard]] u64 FramesPresented() noexcept;

// A crash of the emulator itself. The crash handlers (common/windows/crash_handler.cpp) call these
// two from an exception filter or a signal handler: no allocation, no lock, nothing that waits.

/// Writes the report of a crash. The frontend gives the writer; it was prepared before the run
/// (open file, text already formatted) and gets the one-line detail of the crash.
using CrashReportWriter = void (*)(std::string_view detail) noexcept;
void SetCrashReportWriter(CrashReportWriter writer) noexcept;

/// The first call hands `detail` to the writer; later calls (a second crashing thread) do nothing.
void ReportHostCrash(std::string_view detail) noexcept;

/// Ends the process with ExitCode(Reason::HostCrash), without unwinding or running the exit
/// handlers, when this is an unattended run or a report is prepared. Returns otherwise, and the
/// crash takes its usual course.
void ExitAfterHostCrash() noexcept;

} // namespace Common::RunOutcome
