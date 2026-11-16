// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <qdebug.h>
#include "core/core.h"
#include "core/cpu_manager.h"
#include "emu_thread.h"
#include "yuzu_cmd/run_control.h"
#include "qt_common/qt_common.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_warm/shader_warm_builder.h"

EmuThread::EmuThread() {}

EmuThread::~EmuThread() = default;

void EmuThread::run() {
    Common::SetCurrentThreadName("EmuControlThread");

    auto& gpu = QtCommon::system->GPU();
    auto stop_token = m_stop_source.get_token();

    // Attribution (EGLANTINE_ATTRIBUTION=0 turns it off) runs for the life of this thread.
    const bool attribution =
        RunControl::AttributionRequested() && VideoCommon::Nx2Spikes::BeginSession({});
    if (attribution) {
        VideoCommon::Nx2Spikes::RegisterThread("EmuControlThread");
    }

    QtCommon::system->RegisterHostThread();

    // Main process has been loaded. Make the context current to this thread and begin GPU and CPU
    // execution.
    gpu.ObtainContext();

    emit LoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
    // The caches are tied to this build, title, content and settings: said before any of them
    // loads.
    const u64 application_program_id = QtCommon::system->GetApplicationProcessProgramID();
    RunControl::PublishHostIdentity(*QtCommon::system, application_program_id);
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        {
            RunControl::BootPhaseScope warm_refresh{VideoCommon::Nx2Spikes::BootPhase::WarmRefresh};
            VideoCommon::ShaderWarm::RefreshWarmFiles(*QtCommon::system, application_program_id,
                                                      stop_token);
            warm_refresh.End(stop_token.stop_requested()
                                 ? VideoCommon::Nx2Spikes::PhaseOutcome::Cancelled
                                 : VideoCommon::Nx2Spikes::PhaseOutcome::Ok);
        }
        RunControl::DiskLoadPhases load_phases;
        QtCommon::system->Renderer().ReadRasterizer()->LoadDiskResources(
            application_program_id, stop_token,
            [this, &load_phases](VideoCore::LoadCallbackStage stage, std::size_t value,
                                 std::size_t total) {
                load_phases.OnStage(stage);
                emit LoadProgress(stage, value, total);
            });
        load_phases.Finish(stop_token.stop_requested()
                               ? VideoCommon::Nx2Spikes::PhaseOutcome::Cancelled
                               : VideoCommon::Nx2Spikes::PhaseOutcome::Ok);
    }
    emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0);
    std::optional<RunControl::BootPhaseScope> game_run;
    if (attribution) {
        game_run.emplace(VideoCommon::Nx2Spikes::BootPhase::GameRun);
    }

    gpu.ReleaseContext();
    gpu.Start();

    QtCommon::system->GetCpuManager().OnGpuReady();

    if (QtCommon::system->DebuggerEnabled()) {
        QtCommon::system->InitializeDebugger();
    }

    while (!stop_token.stop_requested()) {
        std::unique_lock lk{m_should_run_mutex};
        if (m_should_run) {
            QtCommon::system->Run();
            if (game_run) {
                game_run->End(VideoCommon::Nx2Spikes::PhaseOutcome::Ok);
                game_run.reset();
            }
            m_stopped.Reset();

            m_should_run_cv.wait(lk, stop_token, [&] {
                return !m_should_run || m_pending_shader_cache_title.has_value();
            });

            if (m_should_run && m_pending_shader_cache_title.has_value()) {
                const u64 program_id = *m_pending_shader_cache_title;
                m_pending_shader_cache_title.reset();

                lk.unlock();
                this->ReloadDiskShaderCache(program_id);
                lk.lock();
            }
        } else {
            QtCommon::system->Pause();
            m_stopped.Set();

            EmulationPaused(lk);
            m_should_run_cv.wait(lk, stop_token, [&] { return m_should_run; });
            EmulationResumed(lk);
        }
    }

    // Shutdown the main emulated process
    QtCommon::system->DetachDebugger();
    QtCommon::system->ShutdownMainProcess();
    if (game_run) {
        game_run->End(VideoCommon::Nx2Spikes::PhaseOutcome::Cancelled); // never ran
    }
    if (attribution) {
        // The recorder finishes; the Qt frontend has no report, its long frames are in the log.
        VideoCommon::Nx2Spikes::EndSession();
    }
}

void EmuThread::ReloadDiskShaderCache(u64 program_id) {
    if (!Settings::values.use_disk_shader_cache.GetValue()) {
        return;
    }

    LOG_INFO(Frontend, "Reloading disk shader cache for {:016X}", program_id);

    auto& system = *QtCommon::system;
    auto& gpu = system.GPU();

    system.Pause();
    gpu.WaitForIdle();
    gpu.ObtainContext();

    RunControl::PublishHostIdentity(system, program_id);
    RunControl::DiskLoadPhases load_phases;
    emit ShaderCacheReloadStarted();
    emit LoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
    const auto stop_token = m_stop_source.get_token();
    system.Renderer().ReadRasterizer()->LoadDiskResources(program_id, stop_token,
        [this, &load_phases](VideoCore::LoadCallbackStage stage, std::size_t value,
                             std::size_t total) {
            load_phases.OnStage(stage);
            emit LoadProgress(stage, value, total);
        });
    load_phases.Finish(stop_token.stop_requested() ? VideoCommon::Nx2Spikes::PhaseOutcome::Cancelled
                                                   : VideoCommon::Nx2Spikes::PhaseOutcome::Ok);
    emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0);
    emit ShaderCacheReloadFinished();

    gpu.ReleaseContext();
}

// Unlock while emitting signals so that the main thread can
// continue pumping events.

void EmuThread::EmulationPaused(std::unique_lock<std::mutex>& lk) {
    lk.unlock();
    emit DebugModeEntered();
    lk.lock();
}

void EmuThread::EmulationResumed(std::unique_lock<std::mutex>& lk) {
    lk.unlock();
    emit DebugModeLeft();
    lk.lock();
}
