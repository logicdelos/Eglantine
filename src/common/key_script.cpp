// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/key_script.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>

#include <fmt/format.h>

namespace Common::KeyScript {

namespace {

constexpr std::string_view FrameForm = "frame>=";

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

/// A whole, finite, non-negative number of seconds.
std::optional<double> ParseSeconds(std::string_view text) {
    text = Trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    const std::string copy{text};
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || errno == ERANGE || !std::isfinite(value) ||
        value < 0.0) {
        return std::nullopt;
    }
    return value;
}

std::optional<u64> ParseCount(std::string_view text) {
    text = Trim(text);
    u64 value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<int> ParseCode(std::string_view text) {
    text = Trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    const std::string copy{text};
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (end != copy.c_str() + copy.size() || errno == ERANGE || value < INT_MIN ||
        value > INT_MAX) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

/// Fills `step` from the text before the colon; returns what is wrong with it, or "".
std::string ParseWhen(std::string_view text, Step& step) {
    step.when = std::string{text};
    std::string_view when = text;
    bool has_timeout = false;
    if (const size_t at = when.find('@'); at != std::string_view::npos) {
        const auto timeout = ParseSeconds(when.substr(at + 1));
        if (!timeout || *timeout <= 0.0) {
            return "the time after @ must be a positive number of seconds";
        }
        step.timeout = *timeout;
        has_timeout = true;
        when = Trim(when.substr(0, at));
    }

    if (when.starts_with(FrameForm)) {
        const auto count = ParseCount(when.substr(FrameForm.size()));
        if (!count) {
            return "frame>= needs a whole number of frames";
        }
        step.trigger = Trigger::Frames;
        step.count = *count;
    } else if (when.starts_with("after") && (when.size() == 5 || std::isspace(static_cast<unsigned char>(when[5])) != 0)) {
        const std::string_view rest = Trim(when.substr(5));
        const size_t plus = rest.find('+');
        const std::string_view event = Trim(rest.substr(0, plus));
        if (event == "first_frame") {
            step.trigger = Trigger::AfterFirstFrame;
        } else if (event == "shaders_idle") {
            step.trigger = Trigger::AfterShadersIdle;
        } else if (event == "prev") {
            step.trigger = Trigger::AfterPrevious;
        } else {
            return fmt::format("unknown event '{}' (first_frame, shaders_idle or prev)", event);
        }
        if (plus != std::string_view::npos) {
            const auto delay = ParseSeconds(rest.substr(plus + 1));
            if (!delay) {
                return "the delay after + must be a number of seconds";
            }
            step.seconds = *delay;
        }
    } else if (!when.empty() && when.front() == 'f') {
        // The old parser read this number with stoull, which accepts one leading plus.
        std::string_view digits = Trim(when.substr(1));
        if (digits.starts_with('+')) {
            digits.remove_prefix(1);
        }
        const auto count = ParseCount(digits);
        if (!count) {
            return "f<N> needs a whole number of presents";
        }
        step.trigger = Trigger::PresentCount;
        step.count = *count;
    } else {
        const auto seconds = ParseSeconds(when);
        if (!seconds) {
            return "expected seconds, f<N>, frame>=<N> or after <event>+<seconds>";
        }
        if (has_timeout) {
            return "a step at a fixed time cannot have a timeout";
        }
        step.trigger = Trigger::Seconds;
        step.seconds = *seconds;
    }

    if (!has_timeout && step.trigger != Trigger::Seconds && step.trigger != Trigger::PresentCount) {
        step.timeout = DefaultEventTimeoutSeconds;
    }
    return {};
}

std::atomic<Runner*> current{};

} // Anonymous namespace

ParseResult Parse(std::string_view text) {
    ParseResult result;
    size_t start = 0;
    size_t number = 0;
    while (start < text.size()) {
        const size_t end = (std::min)(text.find(',', start), text.size());
        const std::string_view item = Trim(text.substr(start, end - start));
        start = end + 1;
        if (item.empty()) {
            continue;
        }
        ++number;
        const size_t colon = item.find(':');
        if (colon == std::string_view::npos) {
            result.error = fmt::format("step {} '{}': no ':' between the trigger and the key",
                                       number, item);
            break;
        }
        Step step;
        if (std::string problem = ParseWhen(Trim(item.substr(0, colon)), step); !problem.empty()) {
            result.error = fmt::format("step {} '{}': {}", number, item, problem);
            break;
        }
        const auto code = ParseCode(item.substr(colon + 1));
        if (!code) {
            result.error = fmt::format("step {} '{}': the key code is not a whole number", number,
                                       item);
            break;
        }
        step.code = *code;
        result.steps.push_back(std::move(step));
    }
    if (!result.error.empty()) {
        result.steps.clear();
    }
    return result;
}

std::string_view StatusName(Status status) noexcept {
    switch (status) {
    case Status::Pending:
        return "pending";
    case Status::Fired:
        return "fired";
    case Status::TimedOut:
        return "timed_out";
    case Status::Skipped:
        return "skipped";
    }
    return "pending";
}

Runner::Runner(std::vector<Step> steps_) : steps{std::move(steps_)} {
    records.reserve(steps.size());
    for (size_t i = 0; i < steps.size(); ++i) {
        Record& record = records.emplace_back();
        record.index = i;
        record.when = steps[i].when;
        record.code = steps[i].code;
    }
}

bool Runner::Due(const Step& step, const Observation& now) const {
    switch (step.trigger) {
    case Trigger::Seconds:
        return step.seconds <= now.seconds;
    case Trigger::PresentCount:
        return step.count <= now.present_count;
    case Trigger::Frames:
        return step.count <= now.frames;
    case Trigger::AfterFirstFrame:
        return first_frame_at && now.seconds - *first_frame_at >= step.seconds;
    case Trigger::AfterShadersIdle:
        return first_frame_at && idle_since &&
               now.seconds - (std::max)(*first_frame_at, *idle_since) >= step.seconds;
    case Trigger::AfterPrevious:
        return now.seconds - previous_at >= step.seconds;
    }
    return false;
}

PollResult Runner::Poll(const Observation& now) {
    std::scoped_lock lock{mutex};
    PollResult result;
    if (!started) {
        started = true;
        armed_at = now.seconds;
        previous_at = now.seconds;
    }
    if (!first_frame_at && now.frames >= 1) {
        first_frame_at = now.seconds;
    }
    if (now.shaders_building && *now.shaders_building <= 0) {
        if (!idle_since) {
            idle_since = now.seconds;
        }
    } else {
        idle_since.reset();
    }

    while (!gave_up && next < steps.size()) {
        const Step& step = steps[next];
        if (Due(step, now)) {
            Record& record = records[next];
            record.status = Status::Fired;
            record.fired_seconds = now.seconds;
            record.fired_run_seconds = now.run_seconds;
            record.fired_frames = now.frames;
            result.fired.push_back(next);
            ++next;
            armed_at = now.seconds;
            previous_at = now.seconds;
        } else if (step.timeout && now.seconds - armed_at >= *step.timeout) {
            records[next].status = Status::TimedOut;
            for (size_t i = next + 1; i < records.size(); ++i) {
                records[i].status = Status::Skipped;
            }
            result.timed_out = next;
            gave_up = true;
        } else {
            break;
        }
    }
    return result;
}

Progress Runner::GetProgress() const {
    std::scoped_lock lock{mutex};
    return {.total = steps.size(),
            .fired = next,
            .finished = gave_up || next >= steps.size()};
}

std::vector<Record> Runner::Records() const {
    std::scoped_lock lock{mutex};
    return records;
}

void SetCurrent(Runner* runner) noexcept {
    current.store(runner, std::memory_order_release);
}

Runner* Current() noexcept {
    return current.load(std::memory_order_acquire);
}

} // namespace Common::KeyScript
