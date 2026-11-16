// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <cstdint>
#include <map>
#include <string>

#include "yuzu_cmd/control_channel.h"

namespace Core {
class System;
}

namespace InputCommon {
class InputSubsystem;
}

namespace Control {

/// The commands of `--control`: status, press, release, screenshot, get, set and stop. Requests and
/// replies are one JSON object per line; the argument checks are in the command functions.
///
/// `status` is answered on the channel thread from atomics alone. The other commands touch the
/// emulator, so they wait in the channel's queue until the main loop calls Poll, once per
/// iteration. A screenshot only starts there; waiting for the frame and writing the file happen
/// on the channel thread.
class Commands {
public:
    /// `request_stop` runs on the main thread when a client sends `stop`. It must make the main
    /// loop end and ask for a stop of the run.
    Commands(Core::System& system_, InputCommon::InputSubsystem& input_,
             std::function<void()> request_stop_);

    /// Opens the endpoint and starts the channel thread. Returns an error text, empty on success.
    [[nodiscard]] std::string Start(const std::string& name);

    /// Main loop boundary: runs the queued commands (a client that left is cleaned up at its place
    /// among them) and ends the timed presses that are due.
    void Poll();

private:
    std::optional<Response> HandleDirect(const Request& request);
    Response HandleMain(const Request& request);
    Response Status() const;
    Response Key(const Request& request, bool press);
    Response Screenshot(const Request& request);
    Response Setting(const Request& request, bool change);
    Response Stop();
    void ReleaseSession(std::uint64_t session);

    Core::System& system;
    InputCommon::InputSubsystem& input;
    std::function<void()> request_stop;
    std::chrono::steady_clock::time_point started;

    // Written by the main thread in Poll, read by the channel thread for `status`.
    std::atomic<int> shaders_building{-1};
    std::atomic<bool> stop_requested{false};

    // Main thread only. One record per key that a client pressed: who holds it and, for a press
    // with hold_ms, when it ends. Nothing else releases a key on a client's behalf.
    struct KeyHold {
        std::uint64_t session = 0;
        std::optional<std::chrono::steady_clock::time_point> due;
    };
    std::map<int, KeyHold> keys;

    // Last: destroyed first, which joins the thread before anything above goes away.
    Channel channel;
};

} // namespace Control
