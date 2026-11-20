// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "video_core/replay/events.h"

namespace VideoCore::Replay {

// The renderer writes into its calling composition's picture, never into a renderer-wide slot.
class CaptureComposition {
public:
    explicit CaptureComposition(u64 id) : previous{std::exchange(current, this)} {
        picture.frame_id = id;
    }
    CaptureComposition(const CaptureComposition&) = delete;
    ~CaptureComposition() { current = previous; }

    static EndPicture* Target() { return current ? &current->picture : nullptr; }
    static bool Take(u64 id, EndPicture& out) {
        auto* target = Target();
        if (!target || target->frame_id != id || target->pixels.empty()) return false;
        out = std::exchange(*target, {});
        return true;
    }

private:
    static inline thread_local CaptureComposition* current = nullptr;
    CaptureComposition* previous;
    EndPicture picture;
};

// Requests enter on the GPU queue, but their fences can signal in reverse order or inline.
// Never wait for an earlier fence on the signaling thread: a single drainer runs the ready prefix.
class CaptureCompositeQueue {
    struct Entry {
        std::function<void()> compose;
        bool ready = false;
    };
public:
    using Ticket = std::shared_ptr<Entry>;
    Ticket Enqueue(std::function<void()> compose) {
        auto ticket = std::make_shared<Entry>(std::move(compose));
        std::lock_guard lock{mutex};
        pending.push_back(ticket);
        return ticket;
    }
    void Ready(const Ticket& ticket) {
        std::unique_lock lock{mutex};
        ticket->ready = true;
        if (draining) return;
        draining = true;
        while (!pending.empty() && pending.front()->ready) {
            auto compose = std::move(pending.front()->compose);
            pending.pop_front();
            lock.unlock();
            compose();
            lock.lock();
        }
        draining = false;
    }
private:
    std::mutex mutex;
    std::deque<Ticket> pending;
    bool draining = false;
};

} // namespace VideoCore::Replay
