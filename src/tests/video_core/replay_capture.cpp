// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Drive the production recorder, writer, reader and GPU hook boundary with synthetic memory.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <latch>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/guest_call/nvn_census.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/replay/capture.h"
#include "video_core/replay/capture_composition.h"
#include "video_core/replay/capture_host.h"
#include "video_core/replay/checkpoint.h"
#include "video_core/replay/events.h"
#include "video_core/replay/format.h"
#include "video_core/replay/format_io.h"
#include "video_core/replay/memory_capture.h"
#include "common/run_outcome.h"
#include <nlohmann/json.hpp>
#include "video_core/replay/run_state.h"

namespace R = VideoCore::Replay;

namespace {

constexpr s32 Channel = 1;
constexpr u32 Space = 1;
constexpr u64 MapGpu = 0x10000;
constexpr u64 MapBytes = 0x2000;
// The start of the tracker's second 64 MiB window: an allocation prefix, as a readback needs.
constexpr u64 DeviceBase = 0x4000000;
constexpr u64 SemaphoreVa = 0x10100;
constexpr u32 TitleVersion = 0x00010002;

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<u32> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eglantine_replay_capture_" + std::to_string(stamp) + "_" +
                std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    std::filesystem::path path;
};

// Device memory that is a pure function of the address: every byte is readable and repeatable.
u8 PatternByte(u64 address) {
    return static_cast<u8>((address * 7 + (address >> 12) * 29 + 1) & 0xFF);
}

class PatternMemory final : public R::MemorySource {
public:
    bool ReadDevice(u64 device_address, std::span<u8> out) override {
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = PatternByte(device_address + i);
        }
        return true;
    }
};

struct HostScript {
    std::function<void()> during_cut;
    std::function<void()> before_quiescent;
    std::function<void(s32)> on_pin;
    u64 vsync = 0;
    bool wrong_picture = false;
};

class FakeHost final : public R::CaptureHost {
public:
    explicit FakeHost(HostScript& script_) : script{script_} {}

    R::MemorySource& Memory() override {
        return memory;
    }

    bool Cut(u64, R::CutState& out, std::string& error) override {
        if (script.during_cut) {
            script.during_cut();
        }
        R::ChannelEntry channel;
        channel.channel_id = static_cast<u32>(Channel);
        channel.address_space_id = Space;
        out.checkpoint.channels.push_back(channel);
        R::AddressSpaceEntry space;
        space.address_space_id = Space;
        space.address_space_bits = 40;
        space.big_page_bits = 16;
        out.checkpoint.address_spaces.push_back(space);

        R::CutMapping mapping;
        mapping.space = Space;
        mapping.gpu_addr = MapGpu;
        mapping.size = MapBytes;
        mapping.device = DeviceBase;
        out.mappings.push_back(mapping);
        R::SessionPin vsync;
        vsync.key = static_cast<u32>(R::SettingKey::VSync);
        vsync.value = script.vsync;
        out.pins.push_back(vsync);
        R::SessionPin backend;
        backend.key = static_cast<u32>(R::SettingKey::ShaderBackend);
        out.pins.push_back(backend);
        R::SessionPin layers;
        layers.key = static_cast<u32>(R::SettingKey::OutputLayerMask);
        out.pins.push_back(layers);
        out.device.vendor_id = 0x10DE;
        out.device.device_id = 0x2204;
        out.title_version_known = true;
        out.title_version = TitleVersion;
        return true;
    }

    // The fake has no GPU thread: the body runs here, again after each Retry, up to a bound.
    bool RunQuiescent(u64, const std::function<R::CutOutcome(std::string&)>& body,
                      std::string& error) override {
        if (script.before_quiescent) script.before_quiescent();
        constexpr u32 MaxAttempts = 8;
        for (u32 attempt = 0; attempt < MaxAttempts; ++attempt) {
            const R::CutOutcome outcome = body(error);
            if (outcome != R::CutOutcome::Retry) {
                return outcome == R::CutOutcome::Done;
            }
        }
        return false;
    }

    void PinPages(std::span<const std::pair<u64, size_t>> runs, s32 delta) override {
        if (script.on_pin) script.on_pin(static_cast<s32>(runs.size()) * delta);
    }

    bool ReadComposedPicture(u64 present_id, R::EndPicture& out) override {
        out.frame_id = present_id + script.wrong_picture;
        out.width = 4;
        out.height = 4;
        out.stride_bytes = 16;
        out.pixels.assign(64, static_cast<u8>(present_id));
        return true;
    }

private:
    HostScript& script;
    PatternMemory memory;
};

// Ends a capture the test left running when a check fails, so the next test can begin one.
struct CaptureGuard {
    CaptureGuard() = default;
    CaptureGuard(const CaptureGuard&) = delete;
    CaptureGuard& operator=(const CaptureGuard&) = delete;
    ~CaptureGuard() {
        static_cast<void>(R::EndCapture(false));
    }
};

R::CaptureConfig MakeConfig(const std::filesystem::path& path, u64 first_frame, u32 frame_count,
                            u32 preroll) {
    R::CaptureConfig config;
    config.path = path.string();
    config.first_frame = first_frame;
    config.frame_count = frame_count;
    config.preroll_frames = preroll;
    config.title_id = 0x0100000000001234ULL;
    config.limits.max_file_bytes = 16ULL << 20;
    config.limits.max_chunk_bytes = 1ULL << 16;
    config.limits.max_staging_bytes = 1ULL << 20;
    return config;
}

void Start(HostScript& script, const R::CaptureConfig& config) {
    R::SetAcceptedPresentBase(0);
    std::string error;
    const bool begun = R::BeginCaptureWithHost(std::make_unique<FakeHost>(script), config, error);
    REQUIRE(error == "");
    REQUIRE(begun);
}

Tegra::CommandList MakeList(u64 gpu_va, u32 words) {
    Tegra::CommandList list{std::size_t{1}};
    list.command_lists[0].raw = 0;
    list.command_lists[0].addr.Assign(gpu_va);
    list.command_lists[0].size.Assign(static_cast<u64>(words));
    return list;
}

void Submit(u64 gpu_va = 0x1000) {
    const Tegra::CommandList list = MakeList(gpu_va, 4);
    R::NoteSubmitSlow(Channel, list);
}

void Consume(u64 gpu_va = 0x1000) {
    const std::array<u32, 4> words{1, 2, 3, 4};
    R::NoteConsumeSlow(Channel, 0, gpu_va, 0, R::ConsumeEntryEnd, 0, words);
}

void Present(u64 id) {
    R::NoteAcceptedPresentSlow(id, R::PresentPath::Nx2, {}, {});
}

void PresentAndCompose(u64 id) {
    Present(id);
    R::NoteFrameComposedSlow(id);
}

} // namespace

TEST_CASE("recorder: two live frames preserve repeated effects and independent counts", "[video_core][replay][recorder]") {
    TempDirectory dir;
    HostScript script;
    CaptureGuard guard;
    const auto path = dir.path / "two.egcap";
    Start(script, MakeConfig(path, 1, 2, 0));
    R::NoteGraphicsCall();
    R::NoteGraphicsCall();
    Submit();
    for (const u32 value : {9U, 3U, 9U}) {
        R::NoteFenceProduced(R::FenceKind::SemaphoreWrite, Channel, Space, SemaphoreVa, value, 4, false);
    }
    Present(1);
    R::NoteGraphicsCall(); // guest has begun frame 2, GPU work is still for frame 1
    Consume();
    R::NoteFrameComposed(1);
    Submit();
    Present(2);
    // The normal close drains the actual queued GPU work, including its last words. A duplicate
    // composition notification and repeated frontend teardown must not create another frame.
    script.before_quiescent = [] {
        Consume();
        R::NoteFrameComposed(2);
        R::NoteFrameComposedSlow(2);
    };
    const auto report = R::EndCapture(true);
    INFO(report.error);
    REQUIRE(report.run_complete);
    REQUIRE(report.finalized_frames == 2);
    REQUIRE(R::EndCapture(true).status == R::SectionStatus::Off);
    R::CaptureReader reader;
    std::string why;
    REQUIRE(reader.Open(path, why) == R::ReadError::None);
    const auto status = reader.Validate(why);
    INFO(why);
    REQUIRE(status == R::ReadError::None);
    std::vector<R::FrameEndRecord> ends;
    for (;;) {
        R::ChunkView chunk;
        bool eof = false;
        REQUIRE(reader.Next(chunk, eof, why) == R::ReadError::None);
        if (eof) break;
        if (chunk.type == R::ChunkType::FrameEnd) {
            REQUIRE(R::Decode(chunk.payload, ends.emplace_back()) == R::ReadError::None);
        }
    }
    REQUIRE(ends.size() == 2);
    REQUIRE(ends[0].frame_id == 1);
    REQUIRE(ends[0].call_count == 2);
    REQUIRE(ends[0].effect_count == 6);
    REQUIRE(ends[0].outputs.size() == 1);
    REQUIRE(ends[0].gpu_words == 4);
    REQUIRE(ends[1].frame_id == 2);
    REQUIRE(ends[1].call_count == 1);
    REQUIRE(ends[1].effect_count == 3);
    REQUIRE(ends[1].gpu_words == 4);
    std::ifstream sidecar{dir.path / "expected_effects.jsonl"};
    std::vector<nlohmann::json> rows;
    for (std::string line; std::getline(sidecar, line);) rows.push_back(nlohmann::json::parse(line));
    REQUIRE(rows.size() == 9);
    constexpr std::array kinds{"submit", "fence_signal", "fence_signal", "fence_signal", "consume",
                               "present_compose", "submit", "consume", "present_compose"};
    for (size_t i = 0; i < rows.size(); ++i) {
        REQUIRE(rows[i]["frame_id"] == (i < 6 ? 1 : 2));
        REQUIRE(rows[i]["ordinal"] == (i < 6 ? i : i - 6));
        REQUIRE(rows[i]["kind"] == kinds[i]);
        REQUIRE(rows[i]["outcome"] == "observed");
    }
    REQUIRE(rows[1]["payload"]["value"] == 9);
    REQUIRE(rows[2]["payload"]["value"] == 3);
    REQUIRE(rows[3]["payload"]["value"] == 9);
}

TEST_CASE("readback origins: verification probes do not become emulated effects",
          "[video_core][replay][readback-origin]") {
    static std::atomic<u32> rows;
    rows.store(0);
    REQUIRE_FALSE(R::CaptureActive());
    R::SetReplayEffectObserver([](const R::LiveEffect& effect) noexcept {
        if (effect.kind == R::EffectKind::ReadbackVisibility) ++rows;
    });
    struct ResetObserver {
        ~ResetObserver() { R::SetReplayEffectObserver(nullptr); }
    } reset;
    {
        R::ScopedReplayVerificationRead fence_probe;
        {
            R::ScopedReplayVerificationRead nested_probe;
            R::NoteHostReadback(DeviceBase, 4);
        }
        R::NoteHostReadback(DeviceBase, 4);
        CHECK(rows.load() == 0);
        // Same address, same size, same observer/frame; only the caller's origin differs.
        std::thread gpu_worker{[] { R::NoteHostReadback(DeviceBase, 4); }};
        gpu_worker.join();
        CHECK(rows.load() == 1);
    }
    R::NoteHostReadback(DeviceBase, 4);
    CHECK(rows.load() == 2);
}

TEST_CASE("readback origins: live recording still records a guest readback",
          "[video_core][replay][recorder][readback-origin]") {
    TempDirectory dir;
    HostScript script;
    CaptureGuard guard;
    Start(script, MakeConfig(dir.path / "guest-read.egcap", 1, 1, 0));
    R::NoteHostReadback(DeviceBase, 4);
    PresentAndCompose(1);
    const auto report = R::EndCapture(true);
    INFO(report.error);
    REQUIRE(report.run_complete);
    std::ifstream ledger{dir.path / "expected_effects.jsonl"};
    std::vector<std::string> kinds;
    for (std::string line; std::getline(ledger, line);) {
        const auto row = nlohmann::json::parse(line);
        REQUIRE(row["frame_id"] == 1);
        kinds.push_back(row.at("kind").get<std::string>());
    }
    CHECK(kinds == std::vector<std::string>{"readback_visibility", "present_compose"});
}

TEST_CASE("recorder: an effect racing the GPU checkpoint refuses the cut", "[video_core][replay][recorder]") {
    TempDirectory dir;
    HostScript script;
    CaptureGuard guard;
    script.during_cut = [] { R::NoteEngineEffect(R::EffectKind::ComputeDispatch); };
    const auto path = dir.path / "raced.egcap";
    R::SetAcceptedPresentBase(0);
    std::string error;
    REQUIRE_FALSE(R::BeginCaptureWithHost(std::make_unique<FakeHost>(script), MakeConfig(path, 1, 1, 0), error));
    REQUIRE(error.find("checkpoint") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("recorder: incomplete or abnormal close requests a reasoned nonzero exit", "[video_core][replay][recorder]") {
    TempDirectory dir;
    HostScript script;
    CaptureGuard guard;
    const auto path = dir.path / "incomplete.egcap";
    Common::RunOutcome::Begin({}, nullptr);
    struct FinishRun { ~FinishRun() { Common::RunOutcome::Finish(); } } finish;
    Start(script, MakeConfig(path, 1, 1, 0));
    bool normal = true;
    SECTION("stop before final present") {}
    SECTION("interrupted after final composite") { PresentAndCompose(1); normal = false; }
    const auto report = R::EndCapture(normal);
    REQUIRE_FALSE(report.run_complete);
    REQUIRE((report.incomplete_reasons & R::IncompleteInterrupted) != 0);
    REQUIRE_FALSE(report.error.empty());
    REQUIRE(Common::RunOutcome::CurrentReason() == Common::RunOutcome::Reason::CaptureFailed);
    REQUIRE(Common::RunOutcome::ExitCode(Common::RunOutcome::CurrentReason()) != 0);
    REQUIRE_FALSE(std::filesystem::exists(path));
    REQUIRE_FALSE(std::filesystem::exists(dir.path / "expected_effects.jsonl"));
    REQUIRE(std::filesystem::exists(path.string() + ".partial"));
}

TEST_CASE("census and recorder number frames by the same closing accepted present",
          "[video_core][replay][census_frame_ids]") {
    namespace C = Core::NvnCensus;
    TempDirectory dir;
    HostScript script;
    CaptureGuard guard;
    const auto path = dir.path / "ids.egcap";
    Start(script, MakeConfig(path, 1, 3, 0));
    C::testing::Begin(4);
    // One synthetic sequence feeds both: every graphics call reaches both counters, and only a
    // present that passes the admission seam closes a frame in either.
    u64 stream_id = 0;
    const auto call = [](u64 index) {
        C::testing::Call(index);
        R::NoteGraphicsCall();
    };
    const auto not_admitted = [&] { // refresh-only or unmapped: the stream saw it, nobody admitted it
        C::NotePresent(C::PresentPath::BufferQueue, 1, 0, ++stream_id);
    };
    const auto admitted = [&](u64 present_id) {
        C::NotePresent(C::PresentPath::BufferQueue, 1, 0, ++stream_id);
        C::NoteAcceptedPresent(present_id);
        Present(present_id);
    };
    call(0);
    call(1);
    Submit();
    admitted(1);
    Consume();
    R::NoteFrameComposed(1);
    call(2);
    not_admitted(); // refresh-only
    call(2);
    not_admitted(); // unmapped layer
    call(3);
    Submit();
    admitted(2);
    Consume();
    R::NoteFrameComposed(2);
    call(1);
    Submit();
    admitted(3);
    script.before_quiescent = [] {
        Consume();
        R::NoteFrameComposed(3);
    };
    const auto report = R::EndCapture(true);
    const C::testing::FrameRows census = C::testing::End();
    INFO(report.error);
    REQUIRE(report.run_complete);
    REQUIRE(report.finalized_frames == 3);
    R::CaptureReader reader;
    std::string why;
    REQUIRE(reader.Open(path, why) == R::ReadError::None);
    REQUIRE(reader.Validate(why) == R::ReadError::None);
    std::vector<std::pair<u64, u64>> capture_rows;
    for (;;) {
        R::ChunkView chunk;
        bool eof = false;
        REQUIRE(reader.Next(chunk, eof, why) == R::ReadError::None);
        if (eof) break;
        if (chunk.type == R::ChunkType::FrameEnd) {
            R::FrameEndRecord end;
            REQUIRE(R::Decode(chunk.payload, end) == R::ReadError::None);
            capture_rows.emplace_back(end.frame_id, end.call_count);
        }
    }
    const std::vector<std::pair<u64, u64>> expected{{1, 2}, {2, 3}, {3, 1}};
    REQUIRE(capture_rows == expected);
    REQUIRE(census.rows == expected);
    REQUIRE(census.dropped == 0);
    REQUIRE(census.accepted_presents == 3);
    REQUIRE(census.closed == 2); // start-up (frame 1) is a row, not a closed frame
}

TEST_CASE("recorder: the publishing guest increment precedes its inline final composite", "[video_core][replay][recorder]") {
    TempDirectory dir;
    HostScript script;
    CaptureGuard guard;
    const auto path = dir.path / "fence.egcap";
    Start(script, MakeConfig(path, 1, 1, 0));
    Tegra::Host1x::SyncpointManager manager;
    using Source = Tegra::Host1x::SyncpointSource;
    Source source = Source::External;
    SECTION("external producer") {}
    SECTION("recorded GPU command producer") { source = Source::GpuCommand; }
    Submit();
    Consume();
    const Service::Nvidia::NvFence fence{7, 1};
    R::NoteAcceptedPresentSlow(1, R::PresentPath::Nx2, {}, {&fence, 1});
    manager.RegisterGuestAction(7, 1, [] { R::NoteFrameComposed(1); });
    manager.IncrementGuest(7, source);
    const auto report = R::EndCapture(true);
    INFO(report.error);
    REQUIRE(report.run_complete);
    std::ifstream ledger{dir.path / "expected_effects.jsonl"};
    std::vector<std::string> kinds;
    for (std::string row; std::getline(ledger, row);) kinds.push_back(nlohmann::json::parse(row).at("kind"));
    const std::vector<std::string> expected{"submit", "consume", source == Source::External ? "fence_signal" : "syncpoint_increment", "present_compose"};
    REQUIRE(kinds == expected);
    R::CaptureReader reader;
    std::string error;
    REQUIRE(reader.Open(path, error) == R::ReadError::None);
    REQUIRE(reader.Validate(error) == R::ReadError::None);
    std::vector<R::ChunkType> boundary;
    for (;;) {
        R::ChunkView chunk;
        bool eof = false;
        REQUIRE(reader.Next(chunk, eof, error) == R::ReadError::None);
        if (eof) break;
        if (chunk.type == R::ChunkType::Fence || chunk.type == R::ChunkType::Present) boundary.push_back(chunk.type);
    }
    std::vector<R::ChunkType> expected_chunks;
    if (source == Source::External) expected_chunks.push_back(R::ChunkType::Fence);
    expected_chunks.push_back(R::ChunkType::Present);
    REQUIRE(boundary == expected_chunks);
}

TEST_CASE("recorder: unrepresentable display dependency refuses a clean capture", "[video_core][replay][recorder]") {
    TempDirectory dir;
    HostScript script;
    CaptureGuard guard;
    Start(script, MakeConfig(dir.path / "bad.egcap", 1, 1, 0));
    u32 reason = R::IncompleteUnresolvedResource;
    SECTION("two fence thresholds") {
        const std::array<Service::Nvidia::NvFence, 2> fences{{{3, 1}, {4, 1}}};
        R::NoteAcceptedPresentSlow(1, R::PresentPath::Nx2, {}, fences);
    }
    SECTION("missing padded backing") {
        // Linear bytes fit MapBytes; the 128-row raw fallback needs 32 KiB.
        Tegra::FramebufferConfig layer{.address = DeviceBase, .width = 64, .height = 32,
            .stride = 64, .pixel_format = Service::android::PixelFormat::Rgba8888};
        R::NoteAcceptedPresentSlow(1, R::PresentPath::Nx2, {&layer, 1}, {});
    }
    SECTION("wrong picture owner") { script.wrong_picture = true; Present(1); }
    SECTION("uncaptured producing threshold") {
        reason = R::IncompleteMissingFence;
        const Service::Nvidia::NvFence fence{7, 3};
        R::NoteAcceptedPresentSlow(1, R::PresentPath::Nx2, {}, {&fence, 1});
    }
    R::NoteFrameComposed(1);
    const auto report = R::EndCapture(true);
    REQUIRE_FALSE(report.run_complete);
    REQUIRE((report.incomplete_reasons & reason) != 0);
    REQUIRE_FALSE(report.error.empty());
}

TEST_CASE("recorder: outstanding compositions drain in order with exact picture ownership", "[video_core][replay][recorder]") {
    R::CaptureCompositeQueue queue;
    std::vector<u64> ids;
    std::vector<u8> pixels;
    bool wrong = false;
    std::latch entered{1}, resume{1};
    const auto make = [&](u64 id) {
        return queue.Enqueue([&, id] {
            R::CaptureComposition scope{id};
            R::CaptureComposition::Target()->pixels = {static_cast<u8>(id + 10)};
            if (id == 1) { entered.count_down(); resume.wait(); }
            R::EndPicture picture;
            wrong |= R::CaptureComposition::Take(id + 1, picture);
            if (R::CaptureComposition::Take(id, picture)) {
                ids.push_back(picture.frame_id);
                pixels.push_back(picture.pixels.at(0));
            }
            wrong |= R::CaptureComposition::Take(id, picture);
        });
    };
    auto first = make(1), second = make(2), third = make(3);
    queue.Ready(second);
    std::thread signaling{[&] { queue.Ready(first); }};
    entered.wait();
    queue.Ready(third);
    resume.count_down();
    signaling.join();
    REQUIRE_FALSE(wrong);
    REQUIRE(ids == std::vector<u64>{1, 2, 3});
    REQUIRE(pixels == std::vector<u8>{11, 12, 13});
}

TEST_CASE("recorder: shutdown retires pins after all admitted hooks leave", "[video_core][replay][recorder]") {
    TempDirectory dir;
    HostScript script;
    CaptureGuard guard;
    std::latch entered{1}, resume{1};
    std::atomic<int> pins{};
    std::function<void()> operation;
    script.on_pin = [&](s32 delta) { pins += delta; };
    SECTION("checkpoint in progress") {
        Start(script, MakeConfig(dir.path / "cut.egcap", 2, 1, 0));
        script.during_cut = [&] { entered.count_down(); resume.wait(); };
        operation = [] { Present(1); };
    }
    SECTION("mapping pin in progress") {
        Start(script, MakeConfig(dir.path / "mapping.egcap", 1, 1, 0));
        script.on_pin = [&](s32 delta) {
            if (delta > 0) { entered.count_down(); resume.wait(); }
            pins += delta;
        };
        operation = [] { R::NoteGpuMap(Space, MapGpu + MapBytes, 4096, DeviceBase + MapBytes, 0, 0); };
    }
    std::thread worker{operation};
    entered.wait();
    std::atomic<bool> ended{};
    R::CaptureReport report;
    std::thread closer{[&] { report = R::EndCapture(false); ended = true; }};
    while (R::CaptureActive()) std::this_thread::yield();
    const bool ended_before_release = ended.load();
    resume.count_down();
    worker.join();
    closer.join();
    REQUIRE_FALSE(ended_before_release);
    REQUIRE_FALSE(report.run_complete);
    REQUIRE(pins.load() == 0);
    REQUIRE_FALSE(R::CaptureActive());
}

TEST_CASE("recorder: non-Immediate VSync cannot advertise an Immediate session", "[video_core][replay][recorder]") {
    TempDirectory dir;
    HostScript script;
    CaptureGuard guard;
    script.vsync = 2; // FIFO
    R::SetAcceptedPresentBase(0);
    std::string error;
    REQUIRE_FALSE(R::BeginCaptureWithHost(std::make_unique<FakeHost>(script), MakeConfig(dir.path / "fifo.egcap", 1, 1, 0), error));
    REQUIRE(error.find("VSync") != std::string::npos);
}

TEST_CASE("recorder: accepted ids advance only for enabled consumers", "[video_core][replay][recorder]") {
    R::SetAcceptedPresentBase(9);
    REQUIRE(R::NoteAcceptedPresent(R::PresentPath::Nx2, {}, {}) == 0);
    REQUIRE(R::LastAcceptedPresentId() == 9);
    REQUIRE(R::NoteAcceptedPresent(R::PresentPath::LegacyBufferQueue, {}, {}, true) == 10);
    REQUIRE(R::NoteAcceptedPresent(R::PresentPath::Nx2, {}, {}, true) == 11);
    R::SetAcceptedPresentBase(0);
}

TEST_CASE("recorder: preroll stays in the live ledger without stealing the selected picture", "[video_core][replay][recorder]") {
    TempDirectory dir;
    HostScript script;
    CaptureGuard guard;
    const auto path = dir.path / "with-preroll.egcap";
    Start(script, MakeConfig(path, 2, 1, 1));
    Present(1);
    R::NoteFrameComposed(1);
    Present(2);
    R::NoteFrameComposed(2);
    const auto report = R::EndCapture(true);
    INFO(report.error);
    REQUIRE(report.run_complete);
    REQUIRE(report.preroll_frames == 1);
    std::ifstream ledger{dir.path / "expected_effects.jsonl"};
    std::vector<u64> effects, pictures;
    for (std::string row; std::getline(ledger, row);) {
        const auto effect = nlohmann::json::parse(row);
        REQUIRE(effect.at("kind") == "present_compose");
        effects.push_back(effect.at("frame_id"));
    }
    REQUIRE(effects == std::vector<u64>{1, 2});
    R::CaptureReader reader;
    std::string error;
    REQUIRE(reader.Open(path, error) == R::ReadError::None);
    REQUIRE(reader.Validate(error) == R::ReadError::None);
    for (;;) {
        R::ChunkView chunk;
        bool eof = false;
        REQUIRE(reader.Next(chunk, eof, error) == R::ReadError::None);
        if (eof) break;
        if (chunk.type == R::ChunkType::EndPicture) pictures.push_back(chunk.header.frame_id);
        if (chunk.type == R::ChunkType::FrameEnd) {
            R::FrameEndRecord end;
            REQUIRE(R::Decode(chunk.payload, end) == R::ReadError::None);
            REQUIRE(end.effect_count == 1);
            REQUIRE(((end.flags & R::FramePreroll) != 0) == (end.frame_id == 1));
        }
    }
    REQUIRE(pictures == std::vector<u64>{2});
}
