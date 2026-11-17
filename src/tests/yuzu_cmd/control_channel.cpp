// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "yuzu_cmd/control_channel.h"
#include "yuzu_cmd/control_transport.h"

namespace {

using Json = nlohmann::json;

/// One scripted client: the lines it sends, then it leaves. A client with a gate sends nothing
/// until the test opens the gate.
struct ScriptedClient {
    std::vector<std::string> lines;
    bool gated = false;
};

/// A transport that serves the scripted clients one after the other and then waits for Stop.
class ScriptedTransport final : public Control::Transport {
public:
    explicit ScriptedTransport(std::vector<ScriptedClient> clients_)
        : clients{std::move(clients_)} {}

    std::string Open(const std::string&) override {
        return {};
    }

    std::string Address() const override {
        return "scripted";
    }

    bool WaitForClient() override {
        std::unique_lock lock{mutex};
        if (stopped) {
            return false;
        }
        if (next >= clients.size()) {
            exhausted = true;
            changed.notify_all();
            changed.wait(lock, [this] { return stopped; });
            return false;
        }
        current = next++;
        sent = 0;
        return true;
    }

    std::size_t Read(char* buffer, std::size_t size) override {
        std::unique_lock lock{mutex};
        const ScriptedClient& client = clients[current];
        if (client.gated) {
            changed.wait(lock, [this] { return gate_open || stopped; });
        }
        if (stopped || sent >= client.lines.size()) {
            return 0;
        }
        const std::string text = client.lines[sent++] + '\n';
        const std::size_t count = std::min(size, text.size());
        std::copy_n(text.data(), count, buffer);
        return count;
    }

    bool Write(std::string_view data) override {
        std::lock_guard lock{mutex};
        replies.emplace_back(Json::parse(data));
        return true;
    }

    void DropClient() override {}

    void Stop() override {
        std::lock_guard lock{mutex};
        stopped = true;
        changed.notify_all();
    }

    bool Exhausted() {
        std::lock_guard lock{mutex};
        return exhausted;
    }

    void OpenGate() {
        std::lock_guard lock{mutex};
        gate_open = true;
        changed.notify_all();
    }

    std::vector<Json> Replies() {
        std::lock_guard lock{mutex};
        return replies;
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<ScriptedClient> clients;
    std::size_t next = 0;
    std::size_t current = 0;
    std::size_t sent = 0;
    bool exhausted = false;
    bool gate_open = false;
    bool stopped = false;
    std::vector<Json> replies;
};

/// Waits for a condition that another thread brings about; false when it did not come.
template <typename Predicate>
bool WaitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

Control::Channel::Direct ToMainThread() {
    return [](const Control::Request&) { return std::optional<Control::Response>{}; };
}

std::string HoldLine(int id, int key) {
    return Json{{"id", id}, {"cmd", "hold"}, {"args", {{"key", key}}}}.dump();
}

} // Anonymous namespace

TEST_CASE("Many connections while the main loop does not poll are kept in one record",
          "[yuzu_cmd][control_channel]") {
    constexpr std::uint64_t Connections = 1000;
    auto transport = std::make_unique<ScriptedTransport>(
        std::vector<ScriptedClient>(Connections, ScriptedClient{}));
    ScriptedTransport& script = *transport;
    Control::Channel channel{std::move(transport), ToMainThread()};
    REQUIRE(channel.Start("test").empty());

    REQUIRE(WaitFor([&] { return script.Exhausted(); }));
    // One record for a thousand clients, not a thousand records.
    REQUIRE(channel.QueuedEntries() == 1);

    std::vector<std::pair<std::uint64_t, std::uint64_t>> gone;
    channel.Poll([](const Control::Request&) { return Control::Response::Ok(); },
                 [&](std::uint64_t first, std::uint64_t last) { gone.emplace_back(first, last); });

    REQUIRE(gone.size() == 1);
    REQUIRE(gone[0] == std::pair<std::uint64_t, std::uint64_t>{1, Connections});
    REQUIRE(channel.QueuedEntries() == 0);
}

TEST_CASE("Records of clients that left keep their place between requests and own only their keys",
          "[yuzu_cmd][control_channel]") {
    // Client 1 holds key 7 and leaves. Clients 2 to 51 leave at once, once the test lets them.
    // Client 52 holds key 9 and leaves.
    std::vector<ScriptedClient> clients(52);
    clients[0].lines = {HoldLine(1, 7)};
    clients[1].gated = true;
    clients[51].lines = {HoldLine(2, 9)};
    auto transport = std::make_unique<ScriptedTransport>(std::move(clients));
    ScriptedTransport& script = *transport;
    Control::Channel channel{std::move(transport), ToMainThread()};
    REQUIRE(channel.Start("test").empty());

    // The model of the key owners that the main loop keeps.
    std::map<int, std::uint64_t> keys;
    std::vector<std::string> events;
    const auto handler = [&](const Control::Request& request) {
        const int key = request.args.at("key").get<int>();
        keys[key] = request.session;
        events.push_back("hold " + std::to_string(key) + " by " + std::to_string(request.session));
        return Control::Response::Ok();
    };
    const auto disconnected = [&](std::uint64_t first, std::uint64_t last) {
        events.push_back("gone " + std::to_string(first) + " to " + std::to_string(last));
        std::erase_if(keys, [&](const auto& entry) {
            return entry.second >= first && entry.second <= last;
        });
    };

    // Client 1's request, then its record. Client 2 waits at its gate with the record queued.
    REQUIRE(WaitFor([&] { return channel.QueuedEntries() == 1; }));
    channel.Poll(handler, disconnected);
    REQUIRE(keys == std::map<int, std::uint64_t>{{7, 1}});
    REQUIRE(WaitFor([&] { return channel.QueuedEntries() == 1; }));

    // Clients 2 to 51 leave behind client 1's record and are joined to it; client 52's request
    // comes after that record.
    script.OpenGate();
    REQUIRE(WaitFor([&] { return channel.QueuedEntries() == 2; }));
    channel.Poll(handler, disconnected);

    // Key 7 is released before client 52 takes key 9, and key 9 outlives the cleanup.
    REQUIRE(events == std::vector<std::string>{"hold 7 by 1", "gone 1 to 51", "hold 9 by 52"});
    REQUIRE(keys == std::map<int, std::uint64_t>{{9, 52}});

    REQUIRE(WaitFor([&] { return script.Exhausted() && channel.QueuedEntries() == 1; }));
    channel.Poll(handler, disconnected);
    REQUIRE(events.back() == "gone 52 to 52");
    REQUIRE(keys.empty());

    const std::vector<Json> replies = script.Replies();
    REQUIRE(replies.size() == 2);
    REQUIRE(replies[0]["ok"] == true);
    REQUIRE(replies[1]["ok"] == true);
}

TEST_CASE("Requests that wait for a main loop that does not poll are bounded",
          "[yuzu_cmd][control_channel]") {
    constexpr std::size_t Limit = 8;
    constexpr std::size_t Sent = 20;
    std::vector<ScriptedClient> clients(1);
    for (std::size_t i = 0; i < Sent; ++i) {
        clients[0].lines.push_back(HoldLine(static_cast<int>(i), 1));
    }
    auto transport = std::make_unique<ScriptedTransport>(std::move(clients));
    ScriptedTransport& script = *transport;
    // A timeout of 1 ms: every request that gets in is cancelled before the test polls.
    Control::Channel channel{std::move(transport), ToMainThread(), std::chrono::milliseconds{1},
                             Limit};
    REQUIRE(channel.Start("test").empty());

    REQUIRE(WaitFor([&] { return script.Exhausted(); }));
    // The cancelled requests are still counted, and so is the record of the client that left.
    REQUIRE(channel.QueuedEntries() == Limit + 1);

    const std::vector<Json> replies = script.Replies();
    REQUIRE(replies.size() == Sent);
    std::size_t timed_out = 0;
    std::size_t refused = 0;
    for (const Json& reply : replies) {
        REQUIRE(reply["ok"] == false);
        REQUIRE(reply["error"]["code"] == "busy");
        const std::string message = reply["error"]["message"].get<std::string>();
        if (message == "the main loop did not pick the request up in time") {
            ++timed_out;
        } else if (message == "too many requests are waiting for the main loop") {
            ++refused;
        }
    }
    REQUIRE(timed_out == Limit);
    REQUIRE(refused == Sent - Limit);

    // Nothing that was cancelled runs, and the record is delivered.
    int handled = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> gone;
    channel.Poll(
        [&](const Control::Request&) {
            ++handled;
            return Control::Response::Ok();
        },
        [&](std::uint64_t first, std::uint64_t last) { gone.emplace_back(first, last); });
    REQUIRE(handled == 0);
    REQUIRE(gone.size() == 1);
    REQUIRE(gone[0] == std::pair<std::uint64_t, std::uint64_t>{1, 1});
    REQUIRE(channel.QueuedEntries() == 0);
}
