// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "common/baseline_settings.h"
#include "common/settings.h"

namespace {

// Platforms whose reference baseline is pinned in full: Windows and Linux on x86-64. Elsewhere a
// few defaults differ on purpose (CPU backend, host MMU, fullscreen mode, extended dynamic state,
// renderer back end), and only the settings shared by every desktop platform are pinned.
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__) && !defined(HAS_NCE))
constexpr bool kPinsPlatformSettings = true;
#else
constexpr bool kPinsPlatformSettings = false;
#endif

constexpr u32 kPinnedVersion = 1;

// Baseline version 1: the default of every persisted emulation setting, sorted by key. This is the
// list in docs/baseline.md. Do not edit it to make the test pass: a default that changes, or a
// setting that is added or removed, makes a new baseline. Raise Baseline::kBaselineVersion, add a
// line to the baseline history in docs/baseline.md and write the new list as the pinned one.
constexpr std::string_view kPinnedCommon[] = {
    "accelerate_astc=1",
    "anti_aliasing=2",
    "aspect_ratio=0",
    "astc_recompression=2",
    "async_presentation=false",
    "barrier_feedback_loops=true",
    "bg_blue=0",
    "bg_green=0",
    "bg_red=0",
    "buf_strength=100",
    "cold_hint_mode=0",
    "cpu_accuracy=0",
    "cpu_ticks=16000",
    "cpuopt_block_linking=true",
    "cpuopt_const_prop=true",
    "cpuopt_context_elimination=true",
    "cpuopt_fast_dispatcher=true",
    "cpuopt_fastmem=true",
    "cpuopt_fastmem_exclusives=true",
    "cpuopt_ignore_memory_aborts=true",
    "cpuopt_misc_ir=true",
    "cpuopt_page_tables=true",
    "cpuopt_recompile_exclusives=true",
    "cpuopt_reduce_misalign_checks=true",
    "cpuopt_return_stack_buffer=true",
    "cpuopt_unsafe_fastmem_check=true",
    "cpuopt_unsafe_ignore_global_monitor=true",
    "cpuopt_unsafe_ignore_standard_fpcr=true",
    "cpuopt_unsafe_inaccurate_nan=true",
    "cpuopt_unsafe_reduce_fp_error=true",
    "cpuopt_unsafe_unfuse_fma=true",
    "custom_rtc_enabled=false",
    "custom_rtc_offset=0",
    "debug=false",
    "disable_buffer_reorder=false",
    "disable_shader_loop_safety_checks=false",
    "dma_accuracy=0",
    "driver_shader_cache_size=10",
    "emulate_bgr565=false",
    "enable_buffer_history=false",
    "enable_compute_pipelines=false",
    "fast_cpu_time=0",
    "fast_gpu_time=1",
    "fix_bloom_effects=false",
    "force_max_clock=false",
    "frame_gen=false",
    "frame_gen_dump_flow=false",
    "frame_gen_flow_scale=75",
    "frame_gen_flow_scale_auto=true",
    "frame_gen_multiplier=2",
    "frame_gen_queue_target=0",
    "frame_gen_target_rate=0",
    "frame_gen_type=1",
    "frame_pacing_mode=0",
    "fsr_sharpening_slider=25",
    "gpu_accuracy=1",
    "gpu_fence_behavior=0",
    "gpu_unswizzle_chunk_size=3",
    "gpu_unswizzle_enabled=false",
    "gpu_unswizzle_stream_size=3",
    "gpu_unswizzle_texture_size=3",
    "hyrule_30fps=false",
    "hyrule_dynamic_fps=false",
    "input_device=auto",
    "language_index=1",
    "max_anisotropy=0",
    "memory_layout_mode=0",
    "mkw_30fps=false",
    "mkw_dynamic_fps=false",
    "nsight_aftermath=false",
    "nvdec_emulation=1",
    "nvidia_dlss=false",
    "nvidia_dlss_quality=2",
    "output_device=auto",
    "output_engine=0",
    "post_shader_chain=",
    "post_shader_enabled=true",
    "post_shader_preset=",
    "prefer_3d_vcache=true",
    "private_driver_shader_cache=true",
    "region_index=1",
    "renderdoc_hotkey=false",
    "resolution_setup=3",
    "rng_seed=0",
    "rng_seed_enabled=false",
    "sample_shading_fraction=0",
    "scaling_filter=16",
    "shader_feedback=false",
    "skip_cpu_inner_invalidation=false",
    "slow_speed_limit=50",
    "sound_index=1",
    "speed_limit=100",
    "sync_core_speed=false",
    "sync_memory_operations=false",
    "time_zone_index=0",
    "turbo_speed_limit=200",
    "use_asynchronous_gpu_emulation=true",
    "use_asynchronous_shaders=true",
    "use_custom_cpu_ticks=false",
    "use_disk_shader_cache=true",
    "use_docked_mode=1",
    "use_mesh_shaders=true",
    "use_multi_core=true",
    "use_reactive_flushing=true",
    "use_speed_limit=true",
    "use_video_framerate=false",
    "use_vsync=2",
    "use_vulkan_driver_pipeline_cache=true",
    "vertex_input_dynamic_state=true",
    "vram_usage_mode=0",
    "vulkan_device=0",
};

// The settings whose default depends on the platform, as pinned for the reference platforms.
constexpr std::string_view kPinnedPlatform[] = {
    "backend=1",
    "cpu_backend=0",
    "cpuopt_unsafe_host_mmu=true",
#ifdef _WIN32
    "fullscreen_mode=0",
#else
    "fullscreen_mode=1",
#endif
    "dyna_state=2",
};

#ifdef _WIN32
constexpr std::string_view kPinnedHash =
    "23977f27b2859cb0013f008fedaa07c2590d4a52706bcd3085cbcb2750463557";
#else
constexpr std::string_view kPinnedHash =
    "8e074e3912f682100c5a9eb64dd71c29755f054a154f16b2debc8520dbb8d3eb";
#endif

bool IsPlatformKey(std::string_view key) {
    for (const std::string_view line : kPinnedPlatform) {
        if (line.substr(0, line.find('=')) == key) {
            return true;
        }
    }
    return false;
}

std::map<std::string, std::string> Pinned() {
    std::map<std::string, std::string> pinned;
    const auto add = [&pinned](std::string_view line) {
        const auto equals = line.find('=');
        pinned.emplace(std::string{line.substr(0, equals)}, std::string{line.substr(equals + 1)});
    };
    for (const std::string_view line : kPinnedCommon) {
        add(line);
    }
    if (kPinsPlatformSettings) {
        for (const std::string_view line : kPinnedPlatform) {
            add(line);
        }
    }
    return pinned;
}

/// What differs between the pinned list and the baseline of the code, one line per difference.
std::string Differences(const Settings::Values& values) {
    std::map<std::string, std::string> actual;
    for (const auto& entry : Settings::Baseline::Entries(values)) {
        if (kPinsPlatformSettings || !IsPlatformKey(entry.key)) {
            actual.emplace(entry.key, entry.value);
        }
    }
    const auto pinned = Pinned();
    std::string text;
    for (const auto& [key, value] : pinned) {
        const auto it = actual.find(key);
        if (it == actual.end()) {
            text += "removed: " + key + "=" + value + "\n";
        } else if (it->second != value) {
            text += "changed: " + key + " pinned " + value + ", now " + it->second + "\n";
        }
    }
    for (const auto& [key, value] : actual) {
        if (!pinned.contains(key)) {
            text += "added: " + key + "=" + value + "\n";
        }
    }
    return text;
}

const Settings::Baseline::Deviation* Find(const std::vector<Settings::Baseline::Deviation>& list,
                                          std::string_view key) {
    for (const auto& deviation : list) {
        if (deviation.key == key) {
            return &deviation;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Baseline version 1 is pinned", "[baseline_settings]") {
    // The pinned list below belongs to one version. When the version is raised the list has to be
    // rewritten for it.
    REQUIRE(Settings::Baseline::kBaselineVersion == kPinnedVersion);

    const auto values = std::make_unique<Settings::Values>();
    const std::string differences = Differences(*values);
    INFO("The baseline settings changed without a new baseline version. Raise "
         "Baseline::kBaselineVersion, add a line to the baseline history in docs/baseline.md and "
         "write the new list as the pinned one:\n"
         << differences);
    CHECK(differences.empty());
}

TEST_CASE("Baseline hash is pinned on the reference platforms", "[baseline_settings]") {
    if (!kPinsPlatformSettings) {
        SUCCEED("The hash is pinned for Windows and Linux on x86-64 only");
        return;
    }
    const auto values = std::make_unique<Settings::Values>();
    const auto entries = Settings::Baseline::Entries(*values);
    INFO("Raise Baseline::kBaselineVersion and update docs/baseline.md when the baseline changes");
    CHECK(Settings::Baseline::Hash(entries) == kPinnedHash);
}

TEST_CASE("Baseline keeps the owner defaults", "[baseline_settings]") {
    const auto values = std::make_unique<Settings::Values>();
    CHECK(values->use_docked_mode.GetDefault() == Settings::ConsoleMode::Docked);
    CHECK(values->anti_aliasing.GetDefault() == Settings::AntiAliasing::Smaa);
    CHECK(values->scaling_filter.GetDefault() == Settings::ScalingFilter::Buf);
    CHECK(values->resolution_setup.GetDefault() == Settings::ResolutionSetup::Res1X);
    CHECK(values->gpu_accuracy.GetDefault() == Settings::GpuAccuracy::High);
    CHECK(values->vsync_mode.GetDefault() == Settings::VSyncMode::Fifo);
    CHECK(values->renderer_backend.GetDefault() == Settings::RendererBackend::Vulkan);
}

TEST_CASE("Baseline leaves out user data, controls and paths", "[baseline_settings]") {
    const auto values = std::make_unique<Settings::Values>();
    std::map<std::string, std::string> entries;
    for (const auto& entry : Settings::Baseline::Entries(*values)) {
        entries.emplace(entry.key, entry.value);
    }
    CHECK_FALSE(entries.contains("current_user"));
    CHECK_FALSE(entries.contains("device_name"));
    CHECK_FALSE(entries.contains("volume"));
    CHECK_FALSE(entries.contains("audio_muted"));
    CHECK_FALSE(entries.contains("mouse_enabled"));
    CHECK_FALSE(entries.contains("nand_directory"));
    CHECK_FALSE(entries.contains("eden_token"));
    CHECK(entries.contains("use_docked_mode"));
}

TEST_CASE("SHA-256 matches the published vectors", "[baseline_settings]") {
    using Settings::Baseline::Sha256Hex;
    CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("Default settings have no deviations", "[baseline_settings]") {
    Settings::Baseline::ClearNotedSources();
    const auto values = std::make_unique<Settings::Values>();
    const auto deviations = Settings::Baseline::Deviations(*values, {});
    CHECK(deviations.empty());
    CHECK(Settings::Baseline::OnBaseline(deviations, {}));
}

TEST_CASE("Deviations report exactly the changed settings with their source",
          "[baseline_settings]") {
    Settings::Baseline::ClearNotedSources();
    const auto values = std::make_unique<Settings::Values>();

    values->use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
    values->scaling_filter.SetGlobal(false);
    values->scaling_filter.SetValue(Settings::ScalingFilter::Bilinear);
    values->anti_aliasing.SetValue(Settings::AntiAliasing::Fxaa);
    Settings::Baseline::NoteSource("anti_aliasing", "command line");
    // A per-game value that equals the default is not a deviation.
    values->gpu_accuracy.SetGlobal(false);
    values->gpu_accuracy.SetValue(Settings::GpuAccuracy::High);
    // A setting outside the baseline does not count.
    values->current_user.SetValue(3);

    const std::vector<Settings::Baseline::EnvironmentSwitch> switches{
        {"EDEN_NX2_EXAMPLE", "1"},
    };
    const auto deviations = Settings::Baseline::Deviations(*values, switches);
    Settings::Baseline::ClearNotedSources();

    REQUIRE(deviations.size() == 4);
    const auto* docked = Find(deviations, "use_docked_mode");
    REQUIRE(docked != nullptr);
    CHECK(docked->baseline == "1");
    CHECK(docked->effective == "0");
    CHECK(docked->source == "global config");

    const auto* filter = Find(deviations, "scaling_filter");
    REQUIRE(filter != nullptr);
    CHECK(filter->baseline == "16");
    CHECK(filter->effective == "1");
    CHECK(filter->source == "per-game config");

    const auto* aa = Find(deviations, "anti_aliasing");
    REQUIRE(aa != nullptr);
    CHECK(aa->baseline == "2");
    CHECK(aa->effective == "1");
    CHECK(aa->source == "command line");

    const auto* env = Find(deviations, "EDEN_NX2_EXAMPLE");
    REQUIRE(env != nullptr);
    CHECK(env->baseline == "unset");
    CHECK(env->effective == "1");
    CHECK(env->source == "environment switch");

    CHECK_FALSE(Settings::Baseline::OnBaseline(deviations, {}));
    CHECK_FALSE(Settings::Baseline::OnBaseline(deviations, {"use_docked_mode"}));
    CHECK(Settings::Baseline::OnBaseline(
        deviations, {"use_docked_mode", "scaling_filter", "anti_aliasing", "EDEN_NX2_EXAMPLE"}));
}

TEST_CASE("Reset to baseline restores the defaults", "[baseline_settings]") {
    Settings::Baseline::ClearNotedSources();
    const auto values = std::make_unique<Settings::Values>();
    values->use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
    values->resolution_setup.SetGlobal(false);
    values->resolution_setup.SetValue(Settings::ResolutionSetup::Res2X);
    values->current_user.SetValue(3);
    values->volume.SetValue(40);

    Settings::Baseline::ResetToBaseline(*values);

    CHECK(Settings::Baseline::Deviations(*values, {}).empty());
    CHECK(values->resolution_setup.UsingGlobal());
    CHECK(values->resolution_setup.GetValue() == Settings::ResolutionSetup::Res1X);
    // Everything that is not an emulation setting is left alone.
    CHECK(values->current_user.GetValue() == 3);
    CHECK(values->volume.GetValue() == 40);
}

TEST_CASE("The report object lists the deviations and the declared keys", "[baseline_settings]") {
    const std::vector<Settings::Baseline::Deviation> none;
    const auto clean = Settings::Baseline::ReportJson(true, {}, none);
    CHECK(clean["version"] == Settings::Baseline::kBaselineVersion);
    CHECK(clean["hash"] == Settings::Baseline::Hash());
    CHECK(clean["requested"] == true);
    CHECK(clean["on_baseline"] == true);
    CHECK(clean["deviations"].is_array());
    CHECK(clean["deviations"].empty());
    CHECK(clean["declared"].is_array());
    CHECK(clean["declared"].empty());

    const std::vector<Settings::Baseline::Deviation> one{
        {"resolution_setup", "3", "6", "command line"},
    };
    const auto changed = Settings::Baseline::ReportJson(false, {}, one);
    CHECK(changed["requested"] == false);
    CHECK(changed["on_baseline"] == false);
    REQUIRE(changed["deviations"].size() == 1);
    CHECK(changed["deviations"][0]["key"] == "resolution_setup");
    CHECK(changed["deviations"][0]["baseline"] == "3");
    CHECK(changed["deviations"][0]["effective"] == "6");
    CHECK(changed["deviations"][0]["source"] == "command line");

    const auto declared = Settings::Baseline::ReportJson(true, {"resolution_setup"}, one);
    CHECK(declared["on_baseline"] == true);
    REQUIRE(declared["declared"].size() == 1);
    CHECK(declared["declared"][0] == "resolution_setup");
    CHECK(declared["deviations"].size() == 1);
}

TEST_CASE("The printed baseline starts with the version and the hash", "[baseline_settings]") {
    const std::string text = Settings::Baseline::PrintableText();
    const std::string head = "baseline_version=" +
                             std::to_string(Settings::Baseline::kBaselineVersion) +
                             "\nbaseline_hash=" + Settings::Baseline::Hash() + "\n";
    CHECK(text.starts_with(head));
    CHECK(text.find("\nuse_docked_mode=1\n") != std::string::npos);
    CHECK(text.find("\nanti_aliasing=2\n") != std::string::npos);
    CHECK(text.find("\nscaling_filter=16\n") != std::string::npos);
}

TEST_CASE("A program requirement is listed but does not leave the baseline", "[baseline_settings]") {
    Settings::Baseline::ClearNotedSources();
    const auto values = std::make_unique<Settings::Values>();
    values->memory_layout_mode.SetGlobal(false);
    values->memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_12Gb);
    Settings::Baseline::NoteSource("memory_layout_mode", Settings::Baseline::kProgramRequirement);

    auto deviations = Settings::Baseline::Deviations(*values, {});
    REQUIRE(deviations.size() == 1);
    CHECK(deviations[0].key == "memory_layout_mode");
    CHECK(deviations[0].baseline == "0");
    CHECK(deviations[0].effective == "4");
    CHECK(deviations[0].source == "program requirement");
    CHECK(Settings::Baseline::OnBaseline(deviations, {}));

    // A noted source is forgotten when it is noted as empty; the value then reads as the config's.
    Settings::Baseline::NoteSource("memory_layout_mode", {});
    deviations = Settings::Baseline::Deviations(*values, {});
    REQUIRE(deviations.size() == 1);
    CHECK(deviations[0].source == "per-game config");
    CHECK_FALSE(Settings::Baseline::OnBaseline(deviations, {}));
    Settings::Baseline::ClearNotedSources();
}

TEST_CASE("--baseline-allow items are split, applied and checked", "[baseline_settings]") {
    Settings::Baseline::ClearNotedSources();
    const auto items = Settings::Baseline::SplitAllowList("use_docked_mode=0,EDEN_NX2_SPLIT");
    REQUIRE(items.size() == 2);
    CHECK(items[0] == "use_docked_mode=0");
    CHECK(items[1] == "EDEN_NX2_SPLIT");
    CHECK(Settings::Baseline::SplitAllowList("a,,b").size() == 3);
    CHECK(Settings::Baseline::SplitAllowList("").size() == 1);

    const auto keys = Settings::Baseline::AllowedKeys(items);
    REQUIRE(keys.size() == 2);
    CHECK(keys[0] == "use_docked_mode");
    CHECK(keys[1] == "EDEN_NX2_SPLIT");

    const auto values = std::make_unique<Settings::Values>();
    Settings::Baseline::ResetToBaseline(*values);
    CHECK(Settings::Baseline::ApplyAllowedValues(*values, items).empty());
    CHECK(values->use_docked_mode.GetValue() == Settings::ConsoleMode::Handheld);
    const auto deviations = Settings::Baseline::Deviations(*values, {});
    REQUIRE(deviations.size() == 1);
    CHECK(deviations[0].key == "use_docked_mode");
    CHECK(deviations[0].source == "command line");
    CHECK(Settings::Baseline::OnBaseline(deviations, keys));
    CHECK_FALSE(Settings::Baseline::OnBaseline(deviations, {}));

    // Not a baseline setting, not a setting at all, and a value the setting does not take.
    CHECK_FALSE(Settings::Baseline::ApplyAllowedValues(*values, {"current_user=1"}).empty());
    CHECK_FALSE(Settings::Baseline::ApplyAllowedValues(*values, {"no_such_setting=1"}).empty());
    CHECK_FALSE(Settings::Baseline::ApplyAllowedValues(*values, {"anti_aliasing=nonsense"}).empty());
    Settings::Baseline::ClearNotedSources();
}

TEST_CASE("An environment switch is refused unless it is named", "[baseline_settings]") {
    const std::vector<Settings::Baseline::EnvironmentSwitch> switches{
        {"EDEN_NX2_FENCE_BATCH", "0"}, {"EDEN_NX2_SPLIT", "1"}};
    CHECK(Settings::Baseline::UndeclaredSwitches(switches, {}).size() == 2);
    const auto undeclared = Settings::Baseline::UndeclaredSwitches(switches, {"EDEN_NX2_SPLIT"});
    REQUIRE(undeclared.size() == 1);
    CHECK(undeclared[0] == "EDEN_NX2_FENCE_BATCH");
    CHECK(Settings::Baseline::UndeclaredSwitches(switches, {"EDEN_NX2_SPLIT,x"}).size() == 2);
}
