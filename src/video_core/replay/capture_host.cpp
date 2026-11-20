// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "common/settings.h"
#include "core/core.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/replay/capture_host.h"

namespace VideoCore::Replay {
namespace {
constexpr u64 PageBytes = 4096;
std::mutex g_hooks_mutex;
CaptureHostHooks g_host_hooks;

[[nodiscard]] CaptureHostHooks HostHooks() {
    std::lock_guard lock{g_hooks_mutex};
    return g_host_hooks;
}

// Reads guest memory through the Host1x device memory manager, page by page. A page that is not
// mapped makes the read fail (the tracker counts that as an unresolved resource).
class HostSource final : public MemorySource {
public:
    explicit HostSource(Core::System& system_) : system{system_} {}

    bool ReadDevice(u64 address, std::span<u8> out) override {
        auto& memory = system.Host1x().MemoryManager();
        size_t done = 0;
        while (done < out.size()) {
            const u64 cursor = address + done;
            const size_t count = (std::min)(out.size() - done,
                                            static_cast<size_t>(PageBytes - (cursor % PageBytes)));
            const u8* const pointer = memory.template GetPointer<u8>(cursor);
            if (pointer == nullptr) {
                return false;
            }
            std::memcpy(out.data() + done, pointer, count);
            done += count;
        }
        return true;
    }

private:
    Core::System& system;
};

// The settings the replayer must apply before it creates its renderer, read from the running
// configuration. `checkpoint` supplies the per-channel flags.
std::vector<SessionPin> CollectSessionPins(const GpuCheckpoint& checkpoint) {
    std::vector<SessionPin> pins;
    const auto pin = [&pins](SettingKey key, u64 value) {
        pins.push_back(SessionPin{static_cast<u32>(key), value});
    };
    const bool docked = Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Docked;
    bool native_tiling = false;
    bool turing = false;
    for (const ChannelEntry& channel : checkpoint.channels) {
        native_tiling = native_tiling || (channel.flags & ChannelNativeTiling) != 0;
        turing = turing || (channel.flags & ChannelTuringMacros) != 0;
    }
    pin(SettingKey::Backend, static_cast<u64>(Settings::values.renderer_backend.GetValue()));
    pin(SettingKey::FrameWidth, docked ? 1920 : 1280);
    pin(SettingKey::FrameHeight, docked ? 1080 : 720);
    pin(SettingKey::Docked, docked ? 1 : 0);
    pin(SettingKey::GpuAccuracy, static_cast<u64>(Settings::values.gpu_accuracy.GetValue()));
    pin(SettingKey::ResolutionSetup, static_cast<u64>(Settings::values.resolution_setup.GetValue()));
    pin(SettingKey::AntiAliasing, static_cast<u64>(Settings::values.anti_aliasing.GetValue()));
    pin(SettingKey::ScalingFilter, static_cast<u64>(Settings::values.scaling_filter.GetValue()));
    pin(SettingKey::AsyncGpu,
        static_cast<u64>(Settings::values.use_asynchronous_gpu_emulation.GetValue()));
    pin(SettingKey::NativeTiling, native_tiling ? 1 : 0);
    // This fork has one shader backend (SPIR-V through Vulkan) and composes the default layer
    // stack with no output mask: both are the replayer's only honoured value, 0.
    pin(SettingKey::ShaderBackend, 0);
    pin(SettingKey::OutputLayerMask, 0);
    pin(SettingKey::TuringMacros, turing ? 1 : 0);
    pin(SettingKey::VSync, static_cast<u64>(Settings::values.vsync_mode.GetValue()));
    pin(SettingKey::AstcRecompression,
        static_cast<u64>(Settings::values.astc_recompression.GetValue()));
    return pins;
}

// The recorder over the running system.
class SystemHost final : public CaptureHost {
public:
    explicit SystemHost(Core::System& system_) : system{system_}, memory{system_} {}

    MemorySource& Memory() override {
        return memory;
    }

    // Runs inside RunQuiescent's body, so on the GPU thread between two command lists.
    bool Cut(u64, CutState& out, std::string& error) override {
        Tegra::GPU& gpu = system.GPU();
        SettleEngineSinks(gpu);
        if (!CaptureGpuCheckpoint(gpu, out.checkpoint, error, &out.retry_later)) {
            return false;
        }
        std::map<u32, Tegra::MemoryManager*> spaces;
        gpu.ForEachChannel([&spaces](Tegra::Control::ChannelState& channel) {
            if (channel.memory_manager) {
                spaces.emplace(static_cast<u32>(channel.memory_manager->GetID()),
                               channel.memory_manager);
            }
        });
        for (const auto& [space_id, manager] : spaces) {
            manager->ForEachMapping(
                [&out, space_id](const Tegra::MemoryManager::MappingRun& run) {
                    CutMapping mapping;
                    mapping.space = space_id;
                    mapping.gpu_addr = run.gpu_addr;
                    mapping.size = run.size;
                    mapping.device = run.device;
                    mapping.kind = static_cast<u32>(run.kind);
                    mapping.big_pages = run.big_pages;
                    mapping.sparse = run.sparse;
                    out.mappings.push_back(mapping);
                });
        }
        const CaptureHostHooks hooks = HostHooks();
        if (hooks.device_identity != nullptr && !hooks.device_identity(system, out.device)) {
            out.device = DeviceIdentity{};
        }
        out.pins = CollectSessionPins(out.checkpoint);
        if (hooks.title_version != nullptr) {
            u32 version = UnavailableVersion;
            out.title_version_known =
                hooks.title_version(system, version) && version != UnavailableVersion;
            out.title_version = out.title_version_known ? version : 0;
        }
        return true;
    }

    bool RunQuiescent(u64 present_id, const std::function<CutOutcome(std::string&)>& body,
                      std::string& error) override {
        if (present_id == 0) {
            return body(error) == CutOutcome::Done;
        }
        // What the GPU thread and this thread share. The action may outlive this call (the system
        // stops before the action runs), so it owns this by shared pointer and touches `body` only
        // after it has marked itself started, which this call then waits out.
        struct Shared {
            std::mutex mutex;
            std::condition_variable done_cv;
            bool started = false;
            bool abandoned = false;
            bool finished = false;
            CutOutcome outcome = CutOutcome::Failed;
            std::string message;
        };
        const auto begin = std::chrono::steady_clock::now();
        const auto deadline = begin + CutQuiesceWindow;
        u32 attempts = 0;
        std::string last_retry;
        for (;;) {
            ++attempts;
            const auto shared = std::make_shared<Shared>();
            system.GPU().RunQuiesced([shared, &body] {
                {
                    const std::scoped_lock lock{shared->mutex};
                    if (shared->abandoned) {
                        return;
                    }
                    shared->started = true;
                }
                std::string message;
                const CutOutcome outcome = body(message);
                {
                    const std::scoped_lock lock{shared->mutex};
                    shared->outcome = outcome;
                    shared->message = std::move(message);
                    shared->finished = true;
                }
                shared->done_cv.notify_all();
            });
            {
                std::unique_lock lock{shared->mutex};
                while (!shared->finished) {
                    shared->done_cv.wait_for(lock, CutPollInterval);
                    // A body that has started has to be waited for, it holds the recorder; one that
                    // has not started can still be called off.
                    if (!shared->finished && !shared->started &&
                        (!system.IsPoweredOn() || std::chrono::steady_clock::now() >= deadline)) {
                        shared->abandoned = true;
                        error = "the GPU did not reach a quiescent point before stop or deadline";
                        return false;
                    }
                }
            }
            if (shared->outcome == CutOutcome::Done) {
                return true;
            }
            if (shared->outcome == CutOutcome::Failed) {
                error = std::move(shared->message);
                return false;
            }
            last_retry = std::move(shared->message);
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                const auto waited =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - begin).count();
                error = last_retry + " (still so at " + std::to_string(attempts) +
                        " quiescent points over " + std::to_string(waited) + " ms)";
                return false;
            }
            std::this_thread::sleep_for(CutRetryPause);
        }
    }

    void PinPages(std::span<const std::pair<u64, size_t>> runs, s32 delta) override {
        system.Host1x().MemoryManager().UpdatePagesCachedBatch(runs, delta);
    }

    bool ReadComposedPicture(u64 present_id, EndPicture& out) override {
        const CaptureHostHooks hooks = HostHooks();
        return hooks.read_composed_picture != nullptr &&
               hooks.read_composed_picture(system, present_id, out);
    }

private:
    // How long a refused cut is tried again at later quiescent points before the cut gives up.
    static constexpr std::chrono::milliseconds CutQuiesceWindow{2000};
    static constexpr std::chrono::milliseconds CutRetryPause{2};
    static constexpr std::chrono::milliseconds CutPollInterval{100};

    Core::System& system;
    HostSource memory;
};

} // namespace

void SetCaptureHostHooks(const CaptureHostHooks& hooks) noexcept {
    std::lock_guard lock{g_hooks_mutex};
    g_host_hooks = hooks;
}

bool BeginCapture(Core::System& system, const CaptureConfig& config, std::string& error) {
    return BeginCaptureWithHost(std::make_unique<SystemHost>(system), config, error);
}

} // namespace VideoCore::Replay
