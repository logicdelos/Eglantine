// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/driver_shader_cache.h"
#include "common/logging.h"
#include <ranges>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/core.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_spikes.h"
#include "video_core/gpu.h"
#include "video_core/present.h"
#include "video_core/replay/capture.h"
#include "video_core/replay/capture_composition.h"
#include "video_core/replay/memory_capture.h"
#include "video_core/replay/run_state.h"
#include "video_core/renderer_vulkan/present/util.h"
#ifdef HAS_RESHADE
#include "video_core/post_processing/fx_chain.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/vulkan_common/streamline.h"
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif
namespace Vulkan {
namespace {

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

// Replay composition (RendererVulkan::SetReplayReadback). One replay owns the process, so the
// state is process-wide; the picture is written by the GPU thread and read by the replay thread.
std::atomic<bool> replay_readback{false};
std::mutex replay_picture_mutex;
std::vector<u8> replay_picture;
u32 replay_picture_width = 0;
u32 replay_picture_height = 0;

bool ReadCapturePicture([[maybe_unused]] Core::System& system, u64 present_id,
                        VideoCore::Replay::EndPicture& out) {
    return VideoCore::Replay::CaptureComposition::Take(present_id, out);
}

// Registered by the RendererVulkan constructor, so the renderer of the system is one.
bool ReadDeviceIdentity(Core::System& system, VideoCore::Replay::DeviceIdentity& out) {
    return static_cast<const RendererVulkan&>(system.Renderer()).FillDeviceIdentity(out);
}

#ifdef HAS_LSFG
[[nodiscard]] VkExtent2D GuestExtent(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return VkExtent2D{};
    }

    const auto& framebuffer = framebuffers.front();
    if (framebuffer.crop_rect.IsEmpty()) {
        return VkExtent2D{.width = framebuffer.width, .height = framebuffer.height};
    }
    return VkExtent2D{
        .width = static_cast<u32>(framebuffer.crop_rect.GetWidth()),
        .height = static_cast<u32>(framebuffer.crop_rect.GetHeight()),
    };
}
#endif

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>>& available_extensions) {
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

/// Guest frames that reached the renderer (one per Composite that is shown); ids start at 1 and
/// only move while attribution records. Composite runs on one thread, the atomic only makes a
/// second renderer in the same process (a title change) safe.
std::atomic<u64> guest_frames_composited{};

/// What the hidden-window path of Composite reports. One thread (the GPU thread) uses it.
struct HiddenComposites {
    u64 run{};           ///< consecutive Composite calls skipped for a hidden window
    u64 logged_at_run{}; ///< `run` when the last line was written
    std::chrono::steady_clock::time_point logged_at{};
};
HiddenComposites hidden_composites;

constexpr std::chrono::seconds HIDDEN_COMPOSITE_LOG_INTERVAL{10};

/// One line when a run of skipped frames starts, one per interval while it lasts.
void NoteHiddenComposite() {
    HiddenComposites& state = hidden_composites;
    const auto now = std::chrono::steady_clock::now();
    if (++state.run == 1) {
        LOG_WARNING(Render_Vulkan, "Composite: window not shown, frames are not presented");
        state.logged_at = now;
        state.logged_at_run = state.run;
    } else if (now - state.logged_at >= HIDDEN_COMPOSITE_LOG_INTERVAL) {
        LOG_WARNING(Render_Vulkan, "Composite: window still not shown, {} frames not presented",
                    state.run - state.logged_at_run);
        state.logged_at = now;
        state.logged_at_run = state.run;
    }
}

/// One line when presenting resumes after a run of skipped frames.
void NoteShownComposite() {
    HiddenComposites& state = hidden_composites;
    if (state.run != 0) {
        LOG_INFO(Render_Vulkan, "Composite: window shown again after {} frames not presented",
                 state.run);
        state.run = 0;
        state.logged_at_run = 0;
    }
}

} // Anonymous namespace

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld, VkSurfaceKHR surface) {
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    const u32 device_index = Settings::values.vulkan_device.GetValue();
    if (device_index >= u32(devices.size())) {
        LOG_ERROR(Render_Vulkan, "Invalid device index {}!", device_index);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Tegra::GPU& gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try
    : RendererBase(emu_window, std::move(context_))
    , device_memory(device_memory_)
    , gpu(gpu_)
    , library(OpenLibrary(context.get()))
    , dld()
#ifdef EGLANTINE_STREAMLINE
    , streamline_started(Streamline::Start())
#endif
    // Create raw Vulkan instance first
    , instance(CreateInstance(*library,
                            dld,
                            VK_API_VERSION_1_1,
                            render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue()))
    // Create debug messenger if debug is enabled
    , debug_messenger(Settings::values.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{})
    // Create surface
    , surface(CreateSurface(instance, render_window.GetWindowInfo()))
    , device(CreateDevice(instance, dld, *surface))
    , memory_allocator(device)
    , state_tracker()
    , scheduler(device, state_tracker)
    , swapchain(*surface,
                device,
                scheduler,
               render_window.GetFramebufferLayout().width,
               render_window.GetFramebufferLayout().height)
    , present_manager(instance,
                      render_window,
                      device,
                      memory_allocator,
                      scheduler,
                      swapchain,
                      surface)
    , blit_swapchain(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_capture(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_applet(device_memory,
                  device,
                  memory_allocator,
                  present_manager,
                  scheduler,
                  PresentFiltersForAppletCapture)
    , rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker, scheduler)
#ifdef HAS_LSFG
    , frame_gen(memory_allocator, scheduler)
#endif
{

    if (Settings::values.renderer_force_max_clock.GetValue() && device.ShouldBoostClocks()) {
        turbo_mode.emplace(instance, dld);
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }

#ifdef HAS_RESHADE
    VideoCore::FxChain::Instance().LoadFromSettings();
#endif

    Report();
    VideoCore::Replay::SetCaptureHostHooks(VideoCore::Replay::CaptureHostHooks{
        .read_composed_picture = ReadCapturePicture,
        .device_identity = ReadDeviceIdentity,
    });
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan() {
    scheduler.RegisterOnSubmit([] {});
    void(device.GetLogical().WaitIdle());
}

void RendererVulkan::ComposeForCapture(std::span<const Tegra::FramebufferConfig> framebuffers,
                                       VideoCore::Replay::EndPicture& picture) {
    const auto layers = Tegra::FilterLayerStack(
        framebuffers, Service::Nvnflinger::LayerStackId::Default, screenshot_layer_scratch);
    u32 width = 0;
    u32 height = 0;
    if (!Tegra::PictureExtent(layers, width, height)) {
        return;
    }
    const vk::Buffer buffer =
        RenderToBuffer(layers, Layout::DefaultFrameLayout(width, height), VK_FORMAT_B8G8R8A8_UNORM,
                       VkDeviceSize{width} * height * 4);
    const auto mapped = buffer.Mapped();
    picture.pixels.assign(mapped.begin(), mapped.end());
    picture.width = width;
    picture.height = height;
    picture.stride_bytes = width * 4;
}

bool RendererVulkan::FillDeviceIdentity(VideoCore::Replay::DeviceIdentity& out) const {
    const VkPhysicalDeviceProperties properties = device.GetPhysical().GetProperties();
    out.vendor_id = properties.vendorID;
    out.device_id = properties.deviceID;
    out.driver_id = static_cast<u32>(device.GetDriverID());
    out.driver_version = properties.driverVersion;
    std::copy(std::begin(properties.pipelineCacheUUID), std::end(properties.pipelineCacheUUID),
              out.pipeline_cache_uuid.begin());
    return true;
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    namespace Profile = VideoCommon::Nx2Profile;
    SCOPE_EXIT {
        const Profile::Scope end_zone{Profile::Zone::CompositeEnd};
        render_window.OnFrameDisplayed();
    };

    // A replay never reaches the window, the swapchain, the applet capture or frame generation.
    if (VideoCore::Replay::ReplayGpuRunning()) {
        ComposeForReplay(framebuffers);
        return;
    }

    {
        const Profile::Scope capture_zone{Profile::Zone::CompositeCapture};
        RenderAppletCaptureLayer(framebuffers);
        // A live capture keeps the picture the way a replay composes it: at the guest size, layer
        // stack default, no overlay. Only while a capture runs.
        if (auto* picture = VideoCore::Replay::CaptureComposition::Target()) {
            ComposeForCapture(framebuffers, *picture);
        }
    }
    if (!render_window.IsShown()) {
        // Nothing is presented, but the frame still ends for the per-frame resources: without the
        // tick the descriptor payload of the frame never resets while the guest keeps drawing.
        // The presenting path is held back by GetRenderFrame (the present fence of an older
        // frame); with no present that backpressure is gone, and the rings TickFrame advances
        // destroy buffers, views and framebuffers a fixed number of ticks after their last use
        // without asking the GPU. So the frame's own submission is waited for on the device
        // (a blocking semaphore wait, no spin) before anything is released.
        NoteHiddenComposite();
        {
            const Profile::Scope hidden_zone{Profile::Zone::CompositeHidden};
            scheduler.Wait(scheduler.Flush());
        }
        const Profile::Scope end_zone{Profile::Zone::CompositeEnd};
        rasterizer.TickFrame();
        return;
    }
    NoteShownComposite();

    {
        const Profile::Scope capture_zone{Profile::Zone::CompositeCapture};
        RenderScreenshot(framebuffers);
    }
    Frame* frame = [this] {
        const Profile::Scope wait_zone{Profile::Zone::PresentFrameWait};
        return present_manager.GetRenderFrame();
    }();
    frame->is_generated = false;
    frame->generation_index = 0;
    frame->generation_count = 0;
    // The guest frame this host frame shows; the id travels in the Frame to the host present.
    // Off: one branch, no counter update.
    frame->guest_frame_id = VideoCommon::Nx2Spikes::AttributionEnabled()
                                ? guest_frames_composited.fetch_add(1, std::memory_order_relaxed) + 1
                                : 0;

    {
        const Profile::Scope blit_zone{Profile::Zone::PresentBlit};
        scheduler.RequestOutsideRenderPassOperationContext();
        blit_swapchain.DrawToFrame(device, rasterizer, frame, framebuffers,
                                   render_window.GetFramebufferLayout(),
                                   present_manager.SwapchainImageCount(),
                                   swapchain.GetImageViewFormat());

#ifdef HAS_LSFG
        void(frame_gen.WantedGenerations(present_manager.MaxExtraFrames()));

        frame_gen.Process(device, frame, present_manager.SwapchainImageFormat(),
                          GuestExtent(framebuffers));
#endif
    }

#ifdef HAS_LSFG

    const size_t generated_frames = frame_gen.GeneratedFrameCount();
    for (size_t generation = 0; generation < generated_frames; ++generation) {
        Frame* generated = present_manager.TryGetRenderFrame();
        if (!generated) {
            break;
        }
        generated->is_generated = true;
        generated->generation_index = static_cast<u32>(generation);
        generated->generation_count = static_cast<u32>(generated_frames);
        generated->guest_frame_id = frame->guest_frame_id;
        {
            const Profile::Scope blit_zone{Profile::Zone::PresentBlit};
            blit_swapchain.PrepareFrame(device, generated, render_window.GetFramebufferLayout());
            frame_gen.GenerateInto(device, generated, generation);
        }
        {
            const Profile::Scope flush_zone{Profile::Zone::PresentFlush};
            scheduler.Flush(*generated->render_ready);
        }
        const Profile::Scope call_zone{Profile::Zone::PresentCall};
        present_manager.Present(generated);
    }
#endif

    {
        const Profile::Scope flush_zone{Profile::Zone::PresentFlush};
        scheduler.Flush(*frame->render_ready);
    }

    {
        const Profile::Scope call_zone{Profile::Zone::PresentCall};
        present_manager.Present(frame);
    }
    const Profile::Scope end_zone{Profile::Zone::CompositeEnd};
#ifdef HAS_LSFG
    scheduler.DispatchWork();
#endif

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
}

void RendererVulkan::SetReplayReadback(bool readback) {
    replay_readback.store(readback, std::memory_order_release);
    std::scoped_lock lock{replay_picture_mutex};
    replay_picture.clear();
}

bool RendererVulkan::ReadReplayPicture(u32& width, u32& height, std::vector<u8>& bgra,
                                       std::string& error) {
    std::scoped_lock lock{replay_picture_mutex};
    if (replay_picture.empty()) {
        error = "no replay picture was composed for this frame";
        return false;
    }
    // Taken, not copied: a present that composes nothing must fail the next read, not repeat
    // the previous picture.
    width = replay_picture_width;
    height = replay_picture_height;
    bgra = std::move(replay_picture);
    replay_picture.clear();
    return true;
}

void RendererVulkan::ComposeForReplay(std::span<const Tegra::FramebufferConfig> framebuffers) {
    constexpr VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    // The same layer stack the screenshot path shows: what a viewer sees, without overlays.
    const auto layers = Tegra::FilterLayerStack(
        framebuffers, Service::Nvnflinger::LayerStackId::Default, screenshot_layer_scratch);
    // The extent of this present, from the same calculation the capture used: a frame that
    // composed nothing live composes nothing here, and the readback then fails by name.
    u32 width = 0;
    u32 height = 0;
    if (!Tegra::PictureExtent(layers, width, height)) {
        LOG_ERROR(Render_Vulkan, "Replay present has no picture extent");
    } else if (replay_readback.load(std::memory_order_acquire)) {
        const Layout::FramebufferLayout layout = Layout::DefaultFrameLayout(width, height);
        const vk::Buffer buffer =
            RenderToBuffer(layers, layout, format, VkDeviceSize{width} * height * 4);
        const auto mapped = buffer.Mapped();
        std::scoped_lock lock{replay_picture_mutex};
        replay_picture.assign(mapped.begin(), mapped.end());
        replay_picture_width = width;
        replay_picture_height = height;
    } else {
        // Timing only: draw into a cached target and wait for the GPU, with no download. The
        // target follows the present's extent: a size change makes a new one.
        const Layout::FramebufferLayout layout = Layout::DefaultFrameLayout(width, height);
        if (!replay_frame.image || replay_frame_extent.width != layout.width ||
            replay_frame_extent.height != layout.height) {
            scheduler.Finish();
            replay_frame_extent = VkExtent2D{layout.width, layout.height};
            replay_frame.image = CreateWrappedImage(memory_allocator, replay_frame_extent, format);
            replay_frame.image_view = CreateWrappedImageView(device, replay_frame.image, format);
            replay_frame.framebuffer =
                blit_capture.CreateFramebuffer(device, layout, *replay_frame.image_view, format);
        }
        blit_capture.DrawToFrame(device, rasterizer, &replay_frame, layers, layout, 1, format);
        scheduler.Finish();
    }

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
}

void RendererVulkan::Report() const {
    using namespace Common::Literals;
    const std::string vendor_name{device.GetVendorName()};
    const std::string model_name{device.GetModelName()};
    const std::string driver_version = GetDriverVersion(device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB", available_vram);
    LOG_INFO(Render_Vulkan, "{}", Common::DriverShaderCacheDescription());
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                                          const Layout::FramebufferLayout& layout, VkFormat format,
                                          VkDeviceSize buffer_size) {
    auto frame = [&]() {
        Frame f{};
        f.image =
            CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(device, layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(device, rasterizer, &frame, framebuffers, layout, 1, format);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer,
                           VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }

    const auto screenshot_layers = Tegra::FilterLayerStack(
        framebuffers, renderer_settings.screenshot_layer_stack, screenshot_layer_scratch);

    const auto& layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(screenshot_layers, layout, VK_FORMAT_B8G8R8A8_UNORM,
                                           layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(),
                dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image) {
        return out;
    }

    const auto dst_buffer =
        CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth,
                                   LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(
    std::span<const Tegra::FramebufferConfig> framebuffers) {
    const auto capture_layers = Tegra::FilterLayerStack(
        framebuffers, Service::Nvnflinger::LayerStackId::LastFrame, applet_capture_layers);

    if (capture_layers.empty())
        return;

    if (!applet_frame.image) {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(device,
            VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_applet.DrawToFrame(device, rasterizer, &applet_frame, capture_layers, VideoCore::Capture::Layout, 1,
                            CaptureFormat);
}

} // namespace Vulkan
