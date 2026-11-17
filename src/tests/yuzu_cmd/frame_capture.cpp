// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "yuzu_cmd/frame_capture.h"

using namespace std::chrono_literals;

namespace {

/// A renderer stand-in. Like the real one, a request counts as pending only once Request has
/// finished, and Request can be held at a gate so that a second caller has time to arrive.
class FakeTarget final : public FrameCapture::Target {
public:
    bool IsPending() const override {
        std::lock_guard lock{mutex};
        ++checks;
        changed.notify_all();
        return registered;
    }

    void Request(u8* data_, u32 width_, u32 height_, std::function<void(bool)> done_) override {
        std::unique_lock lock{mutex};
        ++entered;
        changed.notify_all();
        changed.wait(lock, [this] { return gate_open; });
        registered = true;
        ++registrations;
        data = data_;
        width = width_;
        height = height_;
        done = std::move(done_);
    }

    void WaitUntilEntered(int count) {
        std::unique_lock lock{mutex};
        changed.wait(lock, [&] { return entered >= count; });
    }

    /// True when a second check arrived within `limit`. With a correct lock the second caller is
    /// still waiting for the first and never gets as far as the check.
    bool SecondCheckArrived(std::chrono::milliseconds limit) {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, limit, [this] { return checks >= 2; });
    }

    void OpenGate() {
        std::lock_guard lock{mutex};
        gate_open = true;
        changed.notify_all();
    }

    /// The frame is rendered: row `y` is filled with the byte y + 1, then the request is answered.
    void Complete(bool invert_y) {
        std::function<void(bool)> callback;
        {
            std::lock_guard lock{mutex};
            for (u32 y = 0; y < height; ++y) {
                std::fill_n(data + size_t{y} * width * 4, size_t{width} * 4, static_cast<u8>(y + 1));
            }
            registered = false;
            callback = std::move(done);
        }
        callback(invert_y);
    }

    int Registrations() {
        std::lock_guard lock{mutex};
        return registrations;
    }

private:
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    mutable int checks = 0;
    int entered = 0;
    int registrations = 0;
    bool gate_open = false;
    bool registered = false;
    u8* data = nullptr;
    u32 width = 0;
    u32 height = 0;
    std::function<void(bool)> done;
};

} // Anonymous namespace

TEST_CASE("Two overlapping screenshot requests register exactly once", "[yuzu_cmd][frame_capture]") {
    FakeTarget target;
    std::optional<FrameCapture::Pending> first;
    std::optional<FrameCapture::Pending> second;
    std::string first_error;
    std::string second_error;

    // The first caller is inside the registration, held at the gate of the fake renderer.
    std::thread control{[&] { first = FrameCapture::Begin(target, 320, first_error); }};
    target.WaitUntilEntered(1);
    // The second caller arrives while the first one has not finished registering.
    std::thread mcp{[&] { second = FrameCapture::Begin(target, 640, second_error); }};
    const bool second_got_in = target.SecondCheckArrived(200ms);
    target.OpenGate();
    control.join();
    mcp.join();

    REQUIRE_FALSE(second_got_in);
    REQUIRE(target.Registrations() == 1);
    REQUIRE(first.has_value());
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second_error == "another screenshot is in progress; retry");

    // The accepted capture is the first one, complete and unaltered by the refused request.
    target.Complete(false);
    std::string wait_error;
    const std::optional<FrameCapture::Frame> frame = first->Wait(1s, wait_error);
    REQUIRE(frame.has_value());
    REQUIRE(frame->width == 320);
    REQUIRE(frame->height == 180);
    REQUIRE(frame->bgra.size() == size_t{320} * 180 * 4);
    REQUIRE(frame->bgra[0] == 1);
    REQUIRE(frame->bgra[size_t{179} * 320 * 4 + 7] == 180);
}

TEST_CASE("A new screenshot can start once the previous one is answered", "[yuzu_cmd][frame_capture]") {
    FakeTarget target;
    target.OpenGate();
    std::string error;

    auto first = FrameCapture::Begin(target, 160, error);
    REQUIRE(first.has_value());
    REQUIRE_FALSE(FrameCapture::Begin(target, 160, error).has_value());
    REQUIRE(error == "another screenshot is in progress; retry");

    target.Complete(false);
    auto again = FrameCapture::Begin(target, 160, error);
    REQUIRE(again.has_value());
    REQUIRE(target.Registrations() == 2);
}

TEST_CASE("A frame rendered upside down is turned back", "[yuzu_cmd][frame_capture]") {
    FakeTarget target;
    target.OpenGate();
    std::string error;

    auto pending = FrameCapture::Begin(target, 160, error);
    REQUIRE(pending.has_value());
    target.Complete(true);
    const std::optional<FrameCapture::Frame> frame = pending->Wait(1s, error);

    REQUIRE(frame.has_value());
    REQUIRE(frame->height == 90);
    REQUIRE(frame->bgra[0] == 90); // the last row is now first
    REQUIRE(frame->bgra[size_t{89} * 160 * 4] == 1);
}
