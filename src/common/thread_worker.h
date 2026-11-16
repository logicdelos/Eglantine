// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <queue>

#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "common/unique_function.h"

namespace Common {

template <class StateType = void>
class StatefulThreadWorker {
    static constexpr bool with_state = !std::is_same_v<StateType, void>;

    struct DummyCallable {
        int operator()() const noexcept {
            return 0;
        }
    };

    using Task =
        std::conditional_t<with_state, UniqueFunction<void, StateType*>, UniqueFunction<void>>;
    using StateMaker = std::conditional_t<with_state, std::function<StateType()>, DummyCallable>;

public:
    explicit StatefulThreadWorker(size_t num_workers, std::string name, StateMaker func = {},
                                  ThreadPlacement placement = ThreadPlacement::Default)
        : worker_limit{num_workers}, workers_queued{num_workers}, thread_name{std::move(name)} {
        const auto lambda = [this, func, placement](std::stop_token stop_token, size_t index) {
            Common::SetCurrentThreadName(thread_name.c_str());
            if (placement != ThreadPlacement::Default) {
                Common::SetCurrentThreadPriority(ThreadPriority::Low);
            }
            switch (placement) {
            case ThreadPlacement::Efficiency:
                Common::SetCurrentThreadToEfficiencyCores();
                break;
            case ThreadPlacement::Background:
                Common::SetCurrentThreadToBackgroundWork();
                break;
            default:
                Common::SetCurrentThreadToAllCores();
                break;
            }
            {
                [[maybe_unused]] std::conditional_t<with_state, StateType, int> state{func()};
                while (!stop_token.stop_requested()) {
                    Task task;
                    {
                        std::unique_lock lock{queue_mutex};
                        if (requests.empty()) {
                            wait_condition.notify_all();
                        }
                        // A worker above the limit stays parked: it takes no task, and counts
                        // as idle.
                        condition.wait(lock, stop_token, [this, index] {
                            return !requests.empty() &&
                                   index < worker_limit.load(std::memory_order_relaxed);
                        });
                        if (stop_token.stop_requested()) {
                            break;
                        }
                        task = std::move(requests.front());
                        requests.pop();
                    }
                    if constexpr (with_state) {
                        task(&state);
                    } else {
                        task();
                    }
                    ++work_done;
                }
            }
            {
                std::scoped_lock lock{queue_mutex};
                ++workers_stopped;
            }
            wait_condition.notify_all();
        };
        threads.reserve(num_workers);
        for (size_t i = 0; i < num_workers; ++i) {
            threads.emplace_back(lambda, i);
        }
    }

    StatefulThreadWorker& operator=(const StatefulThreadWorker&) = delete;
    StatefulThreadWorker(const StatefulThreadWorker&) = delete;

    StatefulThreadWorker& operator=(StatefulThreadWorker&&) = delete;
    StatefulThreadWorker(StatefulThreadWorker&&) = delete;

    void QueueWork(Task work) {
        {
            std::unique_lock lock{queue_mutex};
            requests.emplace(std::move(work));
            ++work_scheduled;
        }
        // With parked workers a single wakeup could land on one that may not take the task.
        if (worker_limit.load(std::memory_order_relaxed) < threads.size()) {
            condition.notify_all();
        } else {
            condition.notify_one();
        }
    }

    /// Lets only the first `count` workers (clamped to 1..all) take tasks. The others stay parked
    /// until the limit rises again; a running task always finishes, queued tasks are kept.
    void SetWorkerLimit(size_t count) {
        {
            std::scoped_lock lock{queue_mutex};
            worker_limit.store((std::min)((std::max)(count, size_t{1}), threads.size()),
                               std::memory_order_relaxed);
        }
        condition.notify_all();
    }

    void WaitForRequests(std::stop_token stop_token = {}) {
        std::stop_callback callback(stop_token, [this] {
            for (auto& thread : threads) {
                thread.request_stop();
            }
        });
        std::unique_lock lock{queue_mutex};
        wait_condition.wait(lock, [this] {
            return workers_stopped >= workers_queued || work_done >= work_scheduled;
        });
    }

    /// Tasks queued or running.
    [[nodiscard]] size_t PendingWork() const noexcept {
        return work_scheduled.load(std::memory_order_relaxed) -
               work_done.load(std::memory_order_relaxed);
    }

    // Drops tasks no worker has started. Running tasks still finish.
    void ClearRequests() {
        std::scoped_lock lock{queue_mutex};
        work_scheduled -= requests.size();
        requests = std::queue<Task>{};
    }

private:
    std::queue<Task> requests;
    std::mutex queue_mutex;
    std::condition_variable_any condition;
    std::condition_variable wait_condition;
    std::atomic<size_t> work_scheduled{};
    std::atomic<size_t> work_done{};
    std::atomic<size_t> workers_stopped{};
    std::atomic<size_t> worker_limit{};
    std::atomic<size_t> workers_queued{};
    std::string thread_name;
    std::vector<std::jthread> threads;
};

using ThreadWorker = StatefulThreadWorker<>;

} // namespace Common
