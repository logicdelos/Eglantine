// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "yuzu_cmd/control_transport.h"

namespace Control {

using Json = nlohmann::json;

/// One request line: {"id": <any>, "cmd": "<name>", "args": {...}}.
struct Request {
    Json id;
    std::string cmd;
    Json args = Json::object();
    /// Which connection sent it. Starts at 1 and grows by one for every client that connects.
    std::uint64_t session = 0;
};

/// The answer to one request. An empty `error_code` is success and `result` is sent.
struct Response {
    Json result = Json::object();
    std::string error_code;
    std::string error_message;
    /// Runs on the channel thread after the handler returned, for work that must not hold up the
    /// main loop (waiting for a frame, writing a file). May change the response.
    std::function<void(Response&)> finish;
    /// The main thread waits, for a short time, until the reply has been written before it goes
    /// on. For a request whose handling ends the program.
    bool flush_reply = false;

    static Response Ok(Json result = Json::object());
    static Response Error(std::string code, std::string message);
};

/// The line protocol over a Transport, on a thread of its own. The thread parses and frames, and
/// answers what it can by itself; everything else is queued for Poll, which the main loop calls
/// once per iteration. The thread never calls into the emulation or the renderer except through
/// what a handler passes it.
class Channel {
public:
    /// Called on the channel thread. Returns the response, or nothing to queue the request for the
    /// main thread.
    using Direct = std::function<std::optional<Response>(const Request&)>;
    /// Called on the main thread from Poll.
    using Handler = std::function<Response(const Request&)>;
    /// Called on the main thread from Poll, in order with the requests, when a client has left.
    /// Everything that client's requests did is done by then; those of its successor are not.
    using Disconnected = std::function<void(std::uint64_t session)>;

    /// The longest request line. A longer one is answered with an error; a stream that has no end
    /// of line within this many bytes is dropped.
    static constexpr std::size_t MaxRequestBytes = 64 * 1024;
    /// How long a request may wait for the main thread to pick it up before it is answered with
    /// the "busy" error and cancelled.
    static constexpr std::chrono::seconds MainThreadTimeout{10};

    Channel(std::unique_ptr<Transport> transport_, Direct direct_);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    /// Opens the endpoint and starts the thread. Returns an error text, empty on success.
    [[nodiscard]] std::string Start(const std::string& name);

    /// Main thread: runs the queued requests through `handler`, and tells `disconnected` when a
    /// client has left, at its place in the order.
    void Poll(const Handler& handler, const Disconnected& disconnected);

    /// True once Stop was called.
    [[nodiscard]] bool Stopping() const {
        return stopping.load();
    }

    /// Wakes the thread so it ends; the destructor joins it. Any thread.
    void Stop();

private:
    struct Pending;

    void ThreadMain();
    void ServeClient(std::uint64_t session);
    bool HandleLine(const std::string& line, std::uint64_t session);
    Response RunOnMain(const std::shared_ptr<Pending>& pending);
    bool Send(const Json& reply);

    std::unique_ptr<Transport> transport;
    Direct direct;
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::uint64_t last_session = 0; // channel thread only

    std::mutex queue_mutex;
    std::deque<std::shared_ptr<Pending>> queue;
};

} // namespace Control
