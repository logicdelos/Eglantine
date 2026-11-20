// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Registers the system-side functions a replay calls (video_core/replay/replay_host.h): the GPU and
// renderer setup without a guest process, its teardown, the device allocations that back the
// recorded memory, and the renderer's off-screen picture. Call once before ReplayRunner::Run.

#pragma once

namespace ReplayRunner {

void RegisterHost();

} // namespace ReplayRunner
