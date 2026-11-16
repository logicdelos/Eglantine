// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

#include "common/common_types.h"

namespace Vulkan::DriverBuildReport {

enum class Kind { Graphics, Compute };

/// Records one vkCreate*Pipelines call. Every 10 s with builds, logs how many there were and how
/// many took over 10 ms: a driver that finds the pipeline in its cache builds it in about 1 ms, one
/// that compiles it takes tens of ms (NVIDIA: 0.6-1.5 ms against ~48 ms), so tester logs show
/// whether the driver's own cache works on their system. Thread safe.
void Record(Kind kind, std::chrono::steady_clock::duration time);

/// Builds recorded on this thread while it lives are the shader precache's: they are counted
/// apart, so the game's own builds show whether the driver's cache served them. The shader build
/// report below leaves them out.
class PrecacheScope {
public:
    PrecacheScope();
    ~PrecacheScope();
    PrecacheScope(const PrecacheScope&) = delete;
    PrecacheScope& operator=(const PrecacheScope&) = delete;
};

/// Logs the builds since the last summary (the pipelines built at boot) and starts over.
void LogSummary(const char* when);

// Shader build report: where the time of every pipeline build goes (translation, SPIR-V, driver,
// waiting in a worker queue), how many builders ran at once, and what the GPU thread waited for.
// One "Shader builds:" line every 10 s of build activity (written from Tick, so after the builds
// that completed), at boot and when the queues first drain.

enum class Phase { Translate, Spirv, Other };

/// Time one thread spent in a phase of a build (summed over threads, so it can exceed the wall
/// time). Translate is guest code to IR, Spirv is IR to SPIR-V and shader module creation, Other
/// is environments and pipeline objects.
void AddPhase(Phase phase, std::chrono::steady_clock::duration time);

/// While alive on a thread, that thread counts as a busy builder. Nested scopes count once.
class BuildScope {
public:
    BuildScope();
    ~BuildScope();
    BuildScope(const BuildScope&) = delete;
    BuildScope& operator=(const BuildScope&) = delete;

private:
    bool counted;
};

/// Time a build waited in a queue for a thread to pick it up.
void AddQueued(std::chrono::steady_clock::duration time);

/// Time since `queued` (when the build was queued), for a task that has just started.
void AddQueuedSince(std::chrono::steady_clock::time_point queued);

/// A build finished its driver call: front end (translation to pipeline object) plus driver time
/// is the build's own time, without the queue.
void NoteBuilt(std::chrono::steady_clock::duration build_time);

/// Pipelines created on this thread while it lives come from the disk cache's records (boot
/// replay, deferred disk records), not from a first sight in game.
class DiskScope {
public:
    DiskScope();
    ~DiskScope();
    DiskScope(const DiskScope&) = delete;
    DiskScope& operator=(const DiskScope&) = delete;
};

/// A pipeline is about to be created. Returns true when the same pipeline (`hash`) was built
/// before in this session.
bool NoteStart(u64 hash);

/// The GPU thread waited for a translation or build that was not its own.
void NoteGpuWait(std::chrono::steady_clock::duration time);

/// The GPU thread built a pipeline itself (translation on its own time).
void NoteGpuBuild(std::chrono::steady_clock::duration time);

/// The submission thread waited for a pipeline's driver build before the draw or dispatch.
void NoteSubmissionWait(std::chrono::steady_clock::duration time);

/// A draw was dropped because its pipeline was not ready.
void NoteDrawSkipped();

/// Start of a pipeline's front end (environments to pipeline object), for the thread's next
/// pipeline constructor to pick up. TakeFrontEnd returns the time since then, right before the
/// driver build is queued, which often runs on another thread.
void SetFrontEnd(std::chrono::steady_clock::time_point begin);
std::chrono::steady_clock::duration TakeFrontEnd();

/// A new pipeline cache starts a session: forgets the keys built, the pending period and the
/// boot state. `count` is the number of threads that can build pipelines at once.
void BeginSession(size_t count);

/// True from the first build after boot until the builders first go idle.
[[nodiscard]] bool DrainArmed();

/// Called every frame: logs the 10 s window after the last build, and once the builders went idle
/// after boot (`queues_idle`) logs what they did.
void Tick(bool queues_idle);

} // namespace Vulkan::DriverBuildReport
