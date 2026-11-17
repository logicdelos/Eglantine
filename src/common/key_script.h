// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// The scripted input of EDEN_NX2_KEY_SCRIPT: a list of steps, each of which presses or releases a
// key once its trigger is met. The list is "<when>:<code>,<when>:<code>,..."; <code> is an SDL
// scancode, negative to release. <when> is one of
//
//   <seconds>               seconds on the script clock (it starts at the first poll)
//   f<N>                    N presents of the NX2 present counter (kept as it always was)
//   frame>=<N>              N frames presented, as the run report counts them
//   after first_frame+<S>   S seconds after the first presented frame
//   after shaders_idle+<S>  the shader and pipeline build queue has been empty for S seconds
//                           (and a frame has been presented)
//   after prev+<S>          S seconds after the previous step fired (after the first poll for
//                           the first step)
//
// The three event forms and f<N> may end in "@<T>": give up when T seconds pass after the step
// became the next one to wait for. An event step without it waits at most
// DefaultEventTimeoutSeconds. Steps fire in the order written: a step waits for the one before it.
//
// The parser and the runner are pure (no clock, no emulator), so they can be tested; the frontend
// feeds Runner::Poll with what it observes.
namespace Common::KeyScript {

inline constexpr double DefaultEventTimeoutSeconds = 300.0;

enum class Trigger {
    Seconds,          // <seconds>
    PresentCount,     // f<N>
    Frames,           // frame>=<N>
    AfterFirstFrame,  // after first_frame+<S>
    AfterShadersIdle, // after shaders_idle+<S>
    AfterPrevious,    // after prev+<S>
};

struct Step {
    Trigger trigger{};
    double seconds{}; // Seconds: the time; the After forms: the delay
    u64 count{};      // PresentCount and Frames
    int code{};
    /// Seconds to wait before giving up; empty for a step that waits for ever.
    std::optional<double> timeout;
    /// What was written before the colon, trimmed; for logs and the report.
    std::string when;
};

struct ParseResult {
    std::vector<Step> steps;
    /// Empty when the whole text parsed. A text with an error yields no steps at all: a script
    /// with a step missing would press the rest at the wrong moments.
    std::string error;
};

[[nodiscard]] ParseResult Parse(std::string_view text);

/// What the frontend sees at one poll.
struct Observation {
    double seconds{};                   // the script clock
    std::optional<double> run_seconds;  // seconds since the start of the run; empty if not timed
    u64 present_count{};                // the NX2 present counter
    u64 frames{};                       // frames presented
    std::optional<int> shaders_building; // empty while the GPU is not up
};

enum class Status { Pending, Fired, TimedOut, Skipped };

[[nodiscard]] std::string_view StatusName(Status status) noexcept;

struct Record {
    size_t index{};
    std::string when;
    int code{};
    Status status{Status::Pending};
    double fired_seconds{};               // script clock, Fired only
    std::optional<double> fired_run_seconds;
    u64 fired_frames{};
};

struct PollResult {
    /// Indices of the steps that fired at this poll, in order.
    std::vector<size_t> fired;
    /// The step that gave up waiting; the steps after it will not run.
    std::optional<size_t> timed_out;
};

struct Progress {
    size_t total{};
    size_t fired{};
    bool finished{}; // every step fired, or one timed out
};

/// The state of one script. Poll runs on one thread; Records and GetProgress may be read from
/// others (the report writer, the control channel).
class Runner {
public:
    explicit Runner(std::vector<Step> steps);

    [[nodiscard]] PollResult Poll(const Observation& now);

    [[nodiscard]] const Step& GetStep(size_t index) const {
        return steps[index];
    }
    [[nodiscard]] Progress GetProgress() const;
    [[nodiscard]] std::vector<Record> Records() const;

private:
    [[nodiscard]] bool Due(const Step& step, const Observation& now) const;

    const std::vector<Step> steps;

    mutable std::mutex mutex;
    std::vector<Record> records;
    size_t next = 0;
    bool started = false;
    bool gave_up = false;
    double armed_at = 0.0;   // when `next` became the step being waited for
    double previous_at = 0.0; // when the previous step fired (the start for the first)
    std::optional<double> first_frame_at;
    std::optional<double> idle_since;
};

/// The runner of this process, if there is one; the run report reads it. Set once by the
/// frontend, to an object that lives as long as the process.
void SetCurrent(Runner* runner) noexcept;
[[nodiscard]] Runner* Current() noexcept;

} // namespace Common::KeyScript
