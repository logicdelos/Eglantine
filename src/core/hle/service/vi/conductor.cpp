// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/adpf.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/vi/conductor.h"
#include "core/hle/service/vi/conductor_pace.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/display_list.h"
#include "core/hle/service/vi/vsync_manager.h"

constexpr auto FrameNs = std::chrono::nanoseconds{1000000000 / 60};

namespace Service::VI {

Conductor::Conductor(Core::System& system, Container& container, DisplayList& displays)
    : m_system(system), m_container(container) {
    displays.ForEachDisplay([&](Display& display) {
        m_vsync_managers.insert({display.GetId(), VsyncManager{}});
    });

    if (system.IsMulticore()) {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64 time,
                   std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
                m_signal.Set();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });

        system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
        m_thread = std::jthread([this](std::stop_token token) { this->VsyncThread(token); });
    } else {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64 time,
                   std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
                this->ProcessVsync();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });

        system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
    }
}

Conductor::~Conductor() {
    m_system.CoreTiming().UnscheduleEvent(m_event);

    if (m_system.IsMulticore()) {
        m_thread.request_stop();
        m_signal.Set();
    }
}

void Conductor::LinkVsyncEvent(u64 display_id, Event* event) {
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end()) {
        it->second.LinkVsyncEvent(event);
    }
}

void Conductor::UnlinkVsyncEvent(u64 display_id, Event* event) {
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end()) {
        it->second.UnlinkVsyncEvent(event);
    }
}

void Conductor::ProcessVsync() {
    Common::PollThreadPolicies();
    Common::ADPF::SetTargetWorkDuration(std::chrono::nanoseconds{this->GetFramePeriodNs()});

    for (auto& [display_id, manager] : m_vsync_managers) {
        m_container.ComposeOnDisplay(&m_swap_interval, &m_compose_speed_scale,
                                     &m_queued_swap_interval, display_id);
        if (m_compose_speed_scale != m_logged_compose_scale) {
            LOG_INFO(Service_VI, "{}",
                     Pace::ComposeMultiplierChangeText(m_queued_swap_interval,
                                                       m_logged_compose_scale,
                                                       m_compose_speed_scale));
            m_logged_compose_scale = m_compose_speed_scale;
        }
        manager.SignalVsync(m_system.Kernel());
    }
}

void Conductor::VsyncThread(std::stop_token token) {
    Common::SetCurrentThreadName("VSyncThread");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
    Common::SetCurrentThreadToPerformanceCores();

    while (!token.stop_requested()) {
        m_signal.Wait();

        if (m_system.IsShuttingDown()) {
            return;
        }

        this->ProcessVsync();
    }
}

s64 Conductor::GetNextTicks() const {
    return Pace::NextTickNs(Pace::FromSettings(m_system.GetNVDECActive(), m_compose_speed_scale,
                                               m_swap_interval));
}

s64 Conductor::GetFramePeriodNs() const {
    return Pace::FramePeriodNs(Pace::FromSettings(false, m_compose_speed_scale, m_swap_interval));
}

} // namespace Service::VI
