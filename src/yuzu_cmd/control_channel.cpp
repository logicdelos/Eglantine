// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <exception>

#include "common/logging.h"
#include "yuzu_cmd/control_channel.h"

namespace Control {

using namespace std::chrono_literals;

Response Response::Ok(Json result) {
    Response response;
    response.result = std::move(result);
    return response;
}

Response Response::Error(std::string code, std::string message) {
    Response response;
    response.error_code = std::move(code);
    response.error_message = std::move(message);
    return response;
}

/// A request that went to the main thread. `state` is guarded by `mutex`.
struct Channel::Pending {
    enum class State { Queued, Running, Done, Replied, Cancelled };

    Request request;
    /// A marker for the clients that left, sessions `request.session` to `last_session`, not a
    /// request. Guarded by `queue_mutex` while it is in the queue.
    bool disconnect = false;
    std::uint64_t last_session = 0;
    std::mutex mutex;
    std::condition_variable cv;
    State state = State::Queued;
    Response response;
};

namespace {

Json Render(const Json& id, const Response& response) {
    Json reply = Json::object();
    reply["id"] = id;
    if (response.error_code.empty()) {
        reply["ok"] = true;
        reply["result"] = response.result;
    } else {
        Json error = Json::object();
        error["code"] = response.error_code;
        error["message"] = response.error_message;
        reply["ok"] = false;
        reply["error"] = std::move(error);
    }
    return reply;
}

} // namespace

Channel::Channel(std::unique_ptr<Transport> transport_, Direct direct_,
                 std::chrono::milliseconds main_thread_timeout_, std::size_t max_queued_)
    : transport{std::move(transport_)}, direct{std::move(direct_)},
      main_thread_timeout{main_thread_timeout_}, max_queued{max_queued_} {}

Channel::~Channel() {
    Stop();
    if (thread.joinable()) {
        thread.join();
    }
}

std::string Channel::Start(const std::string& name) {
    std::string error = transport->Open(name);
    if (!error.empty()) {
        return error;
    }
    LOG_INFO(Frontend, "Control channel listening on {}", transport->Address());
    thread = std::thread{[this] { ThreadMain(); }};
    return {};
}

void Channel::Stop() {
    if (!stopping.exchange(true)) {
        transport->Stop();
    }
}

void Channel::ThreadMain() {
    while (transport->WaitForClient()) {
        const std::uint64_t session = ++last_session;
        LOG_INFO(Frontend, "Control channel: client {} connected", session);
        ServeClient(session);
        transport->DropClient();
        {
            // In the queue like a request: the main thread cleans up after this client before it
            // runs anything the next one sends. When the record at the end of the queue is the
            // previous client's, nothing the channel did lies between them: extend that record.
            std::scoped_lock lock{queue_mutex};
            if (!queue.empty() && queue.back()->disconnect &&
                queue.back()->last_session + 1 == session) {
                queue.back()->last_session = session;
            } else {
                auto marker = std::make_shared<Pending>();
                marker->disconnect = true;
                marker->request.session = session;
                marker->last_session = session;
                queue.push_back(std::move(marker));
            }
        }
        LOG_INFO(Frontend, "Control channel: client {} disconnected", session);
    }
    if (!stopping.load()) {
        LOG_ERROR(Frontend, "Control channel: the endpoint failed; the channel is closed");
    }
}

void Channel::ServeClient(std::uint64_t session) {
    std::string buffered;
    char chunk[4096];
    for (;;) {
        const std::size_t count = transport->Read(chunk, sizeof(chunk));
        if (count == 0) {
            return;
        }
        buffered.append(chunk, count);
        std::size_t end;
        while ((end = buffered.find('\n')) != std::string::npos) {
            std::string line = buffered.substr(0, end);
            buffered.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            if (!HandleLine(line, session)) {
                return;
            }
        }
        if (buffered.size() > MaxRequestBytes) {
            // No end of line in sight: the peer is not speaking the protocol. Say so and leave.
            (void)Send(Render(nullptr, Response::Error("request_too_large",
                                                       "a request line is longer than 65536 bytes")));
            return;
        }
    }
}

bool Channel::HandleLine(const std::string& line, std::uint64_t session) {
    if (line.size() > MaxRequestBytes) {
        return Send(Render(nullptr, Response::Error("request_too_large",
                                                    "a request line is longer than 65536 bytes")));
    }
    const Json parsed = Json::parse(line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return Send(Render(nullptr, Response::Error("bad_request",
                                                    "the request is not a JSON object")));
    }
    Request request;
    request.session = session;
    if (const auto id = parsed.find("id"); id != parsed.end()) {
        request.id = *id;
    }
    const auto cmd = parsed.find("cmd");
    if (cmd == parsed.end() || !cmd->is_string()) {
        return Send(Render(request.id,
                           Response::Error("bad_request", "\"cmd\" must be a string")));
    }
    request.cmd = cmd->get<std::string>();
    if (const auto args = parsed.find("args"); args != parsed.end() && !args->is_null()) {
        if (!args->is_object()) {
            return Send(Render(request.id,
                               Response::Error("bad_request", "\"args\" must be an object")));
        }
        request.args = *args;
    }

    std::shared_ptr<Pending> pending;
    Response response;
    try {
        if (std::optional<Response> now = direct(request)) {
            response = std::move(*now);
        } else {
            pending = std::make_shared<Pending>();
            pending->request = request;
            response = RunOnMain(pending);
        }
        if (response.finish && response.error_code.empty()) {
            response.finish(response);
        }
    } catch (const std::exception& e) {
        response = Response::Error("internal", e.what());
    }

    const bool sent = Send(Render(request.id, response));
    if (pending) {
        {
            std::scoped_lock lock{pending->mutex};
            if (pending->state == Pending::State::Done) {
                pending->state = Pending::State::Replied;
            }
        }
        pending->cv.notify_all();
    }
    return sent;
}

Response Channel::RunOnMain(const std::shared_ptr<Pending>& pending) {
    {
        std::scoped_lock lock{queue_mutex};
        if (queued_requests >= max_queued) {
            // Cancelled requests wait in the queue too, so a main loop that does not poll cannot
            // make it grow without limit.
            return Response::Error("busy", "too many requests are waiting for the main loop");
        }
        ++queued_requests;
        queue.push_back(pending);
    }
    const auto deadline = std::chrono::steady_clock::now() + main_thread_timeout;
    const auto slice = std::min<std::chrono::milliseconds>(100ms, main_thread_timeout);
    std::unique_lock lock{pending->mutex};
    for (;;) {
        if (pending->cv.wait_for(lock, slice,
                                 [&] { return pending->state == Pending::State::Done; })) {
            return std::move(pending->response);
        }
        // A request the main thread has not started can still be withdrawn. One it has started
        // is waited for: its handler is short and it must not run twice.
        if (pending->state == Pending::State::Queued &&
            (stopping.load() || std::chrono::steady_clock::now() >= deadline)) {
            pending->state = Pending::State::Cancelled;
            return stopping.load()
                       ? Response::Error("shutting_down", "the emulator is shutting down")
                       : Response::Error("busy",
                                         "the main loop did not pick the request up in time");
        }
    }
}

std::size_t Channel::QueuedEntries() {
    std::scoped_lock lock{queue_mutex};
    return queue.size();
}

void Channel::Poll(const Handler& handler, const Disconnected& disconnected) {
    std::deque<std::shared_ptr<Pending>> batch;
    {
        std::scoped_lock lock{queue_mutex};
        batch.swap(queue);
        queued_requests = 0;
    }
    for (const std::shared_ptr<Pending>& pending : batch) {
        if (pending->disconnect) {
            disconnected(pending->request.session, pending->last_session);
            continue;
        }
        {
            std::scoped_lock lock{pending->mutex};
            if (pending->state != Pending::State::Queued) {
                continue; // cancelled while it waited
            }
            pending->state = Pending::State::Running;
        }
        Response response;
        try {
            response = handler(pending->request);
        } catch (const std::exception& e) {
            response = Response::Error("internal", e.what());
        }
        const bool flush = response.flush_reply;
        std::unique_lock lock{pending->mutex};
        pending->response = std::move(response);
        pending->state = Pending::State::Done;
        pending->cv.notify_all();
        if (flush) {
            pending->cv.wait_for(lock, 2s, [&] {
                return pending->state == Pending::State::Replied ||
                       pending->state == Pending::State::Cancelled;
            });
        }
    }
}

bool Channel::Send(const Json& reply) {
    return transport->Write(reply.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n');
}

} // namespace Control
