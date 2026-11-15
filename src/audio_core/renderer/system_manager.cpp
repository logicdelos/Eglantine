// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include "audio_core/adsp/adsp.h"
#include "audio_core/audio_core.h"
#include "audio_core/renderer/system_manager.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"

namespace AudioCore::Renderer {

SystemManager::SystemManager(Core::System& core_)
    : core{core_}, audio_renderer{core.AudioCore().ADSP().AudioRenderer()} {}

SystemManager::~SystemManager() {
    Stop();
}

void SystemManager::InitializeUnsafe() {
    if (!active) {
        active = true;
        audio_renderer.Start();
        thread = std::jthread([this](std::stop_token stop_token) {
            Common::SetCurrentThreadName("AudioRenderSystemManager");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
            // One audio frame (TargetSampleCount at TargetSampleRate).
            constexpr auto frame_period{std::chrono::microseconds{5000}};
            auto deadline{std::chrono::steady_clock::now()};
            while (active && !stop_token.stop_requested()) {
                {
                    std::scoped_lock l{mutex1};
                    for (auto system : systems) {
                        system->SendCommandToDsp();
                    }
                }
                audio_renderer.Signal();
                audio_renderer.Wait();

                // A running sink stream blocks the ADSP until its device consumes a buffer, which
                // is the pacing. When nothing did (null sink, no renderer with commands, a stream
                // that never started) hold the loop to one frame per period against a steady
                // clock, so the rate is real time and does not drift with the work done per pass.
                // mutex1 is not held here, so Add/Remove are never blocked by this wait.
                const auto now{std::chrono::steady_clock::now()};
                if (audio_renderer.LastRenderWasPaced()) {
                    deadline = now;
                    continue;
                }
                deadline += frame_period;
                if (deadline > now) {
                    Common::StoppableTimedWait(stop_token, deadline - now);
                }
            }
        });
    }
}

void SystemManager::Stop() {
    if (active) {
        active = false;
        thread.request_stop();
        thread.join();
        audio_renderer.Stop();
    }
}

bool SystemManager::Add(System& system_) {
    std::scoped_lock l2{mutex2};
    if (systems.size() + 1 > MaxRendererSessions) {
        LOG_ERROR(Service_Audio, "Maximum AudioRenderer Systems active, cannot add more!");
        return false;
    }
    {
        std::scoped_lock l{mutex1};
        if (systems.empty())
            InitializeUnsafe();
    }
    systems.push_back(&system_);
    return true;
}

bool SystemManager::Remove(System& system_) {
    std::scoped_lock l2{mutex2};
    {
        std::scoped_lock l{mutex1};
        if (systems.remove(&system_) == 0) {
            LOG_ERROR(Service_Audio, "Failed to remove a render system, it was not found in the list!");
            return false;
        }
    }

    if (systems.empty())
        Stop();
    return true;
}

} // namespace AudioCore::Renderer
