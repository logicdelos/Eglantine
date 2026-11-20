// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The replay session's decisions and refusal paths. None of these tests needs a GPU, a window, a
// game or a capture file made by a game: bytes are built here, and the open paths that need a
// file write a throwaway one into the temp directory.

#include <array>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/framebuffer_config.h"
#include "video_core/replay/replay_internal.h"
#include "video_core/replay/replay_session.h"

namespace R = VideoCore::Replay;
using Reason = Common::RunOutcome::Reason;

namespace {

std::vector<u8> ValidHeaderBytes() {
    R::FileHeader header;
    header.flags = R::HeaderComplete;
    // DecodeFileHeader refuses a header with no first frame or no requested frames.
    header.first_frame = 1;
    header.requested_frames = 1;
    return R::EncodeFileHeader(header);
}

R::CompleteRecord CleanComplete() {
    R::CompleteRecord record;
    record.status = R::CompleteStatus::Complete;
    record.dependency_complete = 1;
    return record;
}

std::vector<u8> Bgra(u32 width, u32 height, u32 stride, u8 seed) {
    std::vector<u8> bytes(static_cast<size_t>(stride) * height, 0xEE);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width * 4U; ++x) {
            bytes[static_cast<size_t>(y) * stride + x] = static_cast<u8>(seed + y * 7U + x);
        }
    }
    return bytes;
}

std::filesystem::path TempFile(const char* name, const std::vector<u8>& bytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return path;
}

R::MemoryRange Range(R::RangeKind kind, u64 offset, u64 length, u64 data_offset = 0,
                     u32 ref_sequence = 0) {
    R::MemoryRange range;
    range.offset = offset;
    range.length = length;
    range.kind = kind;
    range.data_offset = data_offset;
    range.ref_sequence = ref_sequence;
    return range;
}

R::SubmitEvent SubmitOf(u64 submit_id, std::vector<R::SubmitEntry> entries) {
    R::SubmitEvent event;
    event.channel_id = 3;
    event.submit_id = submit_id;
    event.entries = std::move(entries);
    return event;
}

R::ConsumeEvent ConsumeOf(u64 submit_id, u32 entry, u64 va, u32 word_offset, u32 count) {
    R::ConsumeEvent consume;
    consume.channel_id = 3;
    consume.submit_id = submit_id;
    consume.entry_index = entry;
    consume.gpu_va = va + u64{word_offset} * 4U;
    consume.word_offset = word_offset;
    consume.words.assign(count, 0x11223344U);
    return consume;
}

} // namespace

TEST_CASE("Replay session: the capture prefix is classified before a reader runs",
          "[video_core][replay]") {
    const std::vector<u8> good = ValidHeaderBytes();
    REQUIRE(good.size() == R::FileHeaderBytes);
    CHECK(R::ClassifyCapturePrefix("a.capture", good).error == R::ReadError::None);
    CHECK(R::ClassifyCapturePrefix("a.capture.partial", good).error == R::ReadError::NotComplete);
    CHECK(R::ClassifyCapturePrefix("", good).error == R::ReadError::Truncated);
    CHECK(R::ClassifyCapturePrefix("a.capture", std::span<const u8>{good.data(), 100}).error ==
          R::ReadError::Truncated);

    std::vector<u8> wrong_magic = good;
    wrong_magic[0] ^= 0xFFU;
    CHECK(R::ClassifyCapturePrefix("a.capture", wrong_magic).error == R::ReadError::BadMagic);

    std::vector<u8> damaged = good;
    damaged[R::Magic.size() + 20] ^= 0x5AU;
    CHECK(R::ClassifyCapturePrefix("a.capture", damaged).error != R::ReadError::None);
}

TEST_CASE("Replay session: an unclean complete record is refused", "[video_core][replay]") {
    std::string detail;
    CHECK(R::RejectUnclean(CleanComplete(), detail) == R::ReadError::None);
    CHECK(R::RejectUnclean(R::CompleteRecord{}, detail) == R::ReadError::NotComplete);

    R::CompleteRecord dropped = CleanComplete();
    dropped.dropped_events = 1;
    CHECK(R::RejectUnclean(dropped, detail) == R::ReadError::NotComplete);
    R::CompleteRecord unresolved = CleanComplete();
    unresolved.unresolved_resources = 2;
    CHECK(R::RejectUnclean(unresolved, detail) == R::ReadError::NotComplete);
    R::CompleteRecord no_dependency = CleanComplete();
    no_dependency.dependency_complete = 0;
    CHECK(R::RejectUnclean(no_dependency, detail) == R::ReadError::NotComplete);
}

TEST_CASE("Replay session: chunk checksum and sequence are refused", "[video_core][replay]") {
    std::vector<u8> payload{1, 2, 3, 4, 5, 6, 7, 8};
    R::ChunkHeader header;
    header.type = static_cast<u32>(R::ChunkType::Session);
    header.sequence = 0;
    const auto bytes = R::EncodeChunkHeader(header, payload);
    std::string detail;
    CHECK(R::RejectChunkBytes(bytes, payload, detail) == R::ReadError::None);

    std::vector<u8> damaged = payload;
    damaged[3] ^= 1U;
    CHECK(R::RejectChunkBytes(bytes, damaged, detail) != R::ReadError::None);

    R::SequenceCheck state;
    R::ChunkHeader second = header;
    second.sequence = 1;
    const auto second_bytes = R::EncodeChunkHeader(second, payload);
    (void)second_bytes;
    CHECK(R::RejectDecodedChunk(state, header, payload, detail) == R::ReadError::None);
    CHECK(R::RejectDecodedChunk(state, second, payload, detail) == R::ReadError::None);
    R::ChunkHeader skipped = header;
    skipped.sequence = 3;
    (void)R::EncodeChunkHeader(skipped, payload);
    CHECK(R::RejectDecodedChunk(state, skipped, payload, detail) == R::ReadError::BadOrdering);
}

TEST_CASE("Replay session: pins are interpreted or refused", "[video_core][replay]") {
    const auto pin = [](R::SettingKey key, u64 value) {
        return R::SessionPin{static_cast<u32>(key), value};
    };
    CHECK(R::InterpretPins({}).ok);

    const std::vector<R::SessionPin> good{pin(R::SettingKey::Backend, 1),
                                          pin(R::SettingKey::FrameWidth, 1920),
                                          pin(R::SettingKey::FrameHeight, 1080),
                                          pin(R::SettingKey::Docked, 1),
                                          pin(R::SettingKey::GpuAccuracy, 1),
                                          pin(R::SettingKey::VSync, 0)};
    const R::PinDecision decision = R::InterpretPins(good);
    REQUIRE(decision.ok);
    CHECK(decision.has_width);
    CHECK(decision.width == 1920);
    CHECK(decision.has_height);
    CHECK(decision.height == 1080);
    CHECK(decision.has_docked);
    CHECK(decision.docked == 1);
    CHECK(decision.has_accuracy);
    CHECK(decision.accuracy == 1);
    CHECK_FALSE(decision.has_resolution);

    CHECK_FALSE(R::InterpretPins(std::vector<R::SessionPin>{pin(R::SettingKey::Backend, 2)}).ok);
    CHECK_FALSE(R::InterpretPins(std::vector<R::SessionPin>{pin(R::SettingKey::VSync, 2)}).ok);
    CHECK_FALSE(R::InterpretPins(std::vector<R::SessionPin>{R::SessionPin{99, 1}}).ok);
    CHECK_FALSE(R::InterpretPins(std::vector<R::SessionPin>{R::SessionPin{0, 1}}).ok);
    CHECK_FALSE(R::InterpretPins(std::vector<R::SessionPin>{
                                     pin(R::SettingKey::NativeTiling, R::SessionPinUnknown)})
                    .ok);
    CHECK_FALSE(R::InterpretPins(std::vector<R::SessionPin>{pin(R::SettingKey::Docked, 1),
                                                            pin(R::SettingKey::Docked, 1)})
                    .ok);
    CHECK_FALSE(
        R::InterpretPins(std::vector<R::SessionPin>{pin(R::SettingKey::ResolutionSetup, 13)}).ok);
    CHECK_FALSE(
        R::InterpretPins(std::vector<R::SessionPin>{pin(R::SettingKey::AntiAliasing, 3)}).ok);
    CHECK_FALSE(
        R::InterpretPins(std::vector<R::SessionPin>{pin(R::SettingKey::ScalingFilter, 17)}).ok);
}

TEST_CASE("Replay session: the picture extent is the first layer's, whatever the console mode",
          "[video_core][replay]") {
    // Capture and replay size their pictures with this one function: a docked console showing a
    // 1280x720 crop gives 1280x720, not the 1920x1080 a guess from the console mode would.
    u32 width = 0;
    u32 height = 0;
    CHECK_FALSE(Tegra::PictureExtent({}, width, height));

    Tegra::FramebufferConfig cropped;
    cropped.width = 1920;
    cropped.height = 1088;
    cropped.crop_rect = Common::Rectangle<int>(0, 0, 1280, 720);
    CHECK(Tegra::PictureExtent(std::span<const Tegra::FramebufferConfig>{&cropped, 1}, width,
                               height));
    CHECK(width == 1280);
    CHECK(height == 720);

    Tegra::FramebufferConfig whole;
    whole.width = 854;
    whole.height = 480;
    CHECK(Tegra::PictureExtent(std::span<const Tegra::FramebufferConfig>{&whole, 1}, width, height));
    CHECK(width == 854);
    CHECK(height == 480);

    // The first layer decides; a later one does not change the size.
    const std::array<Tegra::FramebufferConfig, 2> two{whole, cropped};
    CHECK(Tegra::PictureExtent(two, width, height));
    CHECK(width == 854);
    CHECK(height == 480);

    // A frame with an empty extent composes nothing.
    Tegra::FramebufferConfig empty;
    CHECK_FALSE(Tegra::PictureExtent(std::span<const Tegra::FramebufferConfig>{&empty, 1}, width,
                                     height));
}

TEST_CASE("Replay session: a replay opens a Vulkan window whatever the local backend is",
          "[video_core][replay]") {
    using Settings::RendererBackend;
    // The window is built before the session applies the capture's pins: a Null or OpenGL local
    // setting must not give the replay a window the Vulkan renderer cannot present to.
    for (const RendererBackend local :
         {RendererBackend::Null, RendererBackend::OpenGL_GLSL, RendererBackend::OpenGL_SPIRV,
          RendererBackend::Vulkan}) {
        CHECK(R::WindowBackendFor(true, local) == RendererBackend::Vulkan);
    }
    // A run that is not a replay keeps its setting.
    CHECK(R::WindowBackendFor(false, RendererBackend::Null) == RendererBackend::Null);
    CHECK(R::WindowBackendFor(false, RendererBackend::OpenGL_GLSL) == RendererBackend::OpenGL_GLSL);
}

TEST_CASE("Replay session: the phases run in one order", "[video_core][replay]") {
    using R::Phase;
    using R::PhaseEvent;
    auto step = R::AdvancePhase(Phase::Closed, PhaseEvent::Opened);
    REQUIRE(step.allowed);
    CHECK(step.next == Phase::Opened);
    step = R::AdvancePhase(step.next, PhaseEvent::PinsApplied);
    REQUIRE(step.allowed);
    step = R::AdvancePhase(step.next, PhaseEvent::Running);
    REQUIRE(step.allowed);
    step = R::AdvancePhase(step.next, PhaseEvent::Done);
    REQUIRE(step.allowed);
    CHECK(step.next == Phase::Done);

    CHECK_FALSE(R::AdvancePhase(Phase::Opened, PhaseEvent::Running).allowed);
    CHECK_FALSE(R::AdvancePhase(Phase::Closed, PhaseEvent::PinsApplied).allowed);
    CHECK_FALSE(R::AdvancePhase(Phase::PinsApplied, PhaseEvent::PinsApplied).allowed);
    CHECK_FALSE(R::AdvancePhase(Phase::Done, PhaseEvent::Failed).allowed);
    CHECK_FALSE(R::AdvancePhase(Phase::Failed, PhaseEvent::Opened).allowed);
    CHECK(R::AdvancePhase(Phase::Running, PhaseEvent::Failed).next == Phase::Failed);
}

TEST_CASE("Replay session: picture checksums ignore row padding", "[video_core][replay]") {
    const std::vector<u8> tight = Bgra(5, 3, 20, 9);
    std::vector<u8> padded = Bgra(5, 3, 32, 9);
    for (u32 y = 0; y < 3; ++y) {
        for (u32 x = 20; x < 32; ++x) {
            padded[static_cast<size_t>(y) * 32U + x] = 0x77;
        }
    }
    const R::ChecksumResult a = R::PictureChecksum(tight, 5, 3, 20);
    const R::ChecksumResult b = R::PictureChecksum(padded, 5, 3, 32);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(a.hash == b.hash);

    std::vector<u8> changed = tight;
    changed[17] ^= 1U;
    CHECK(R::PictureChecksum(changed, 5, 3, 20).hash != a.hash);
    CHECK_FALSE(R::PictureChecksum(tight, 5, 3, 19).ok);
    CHECK_FALSE(R::PictureChecksum(tight, 0, 3, 20).ok);
    CHECK_FALSE(R::PictureChecksum(tight, 9000, 3, 36000).ok);
}

TEST_CASE("Replay session: pictures swizzle and survive a PNG round trip", "[video_core][replay]") {
    const std::vector<u8> bgra{10, 20, 30, 255, 40, 50, 60, 128};
    std::vector<u8> rgba;
    REQUIRE(R::SwizzleBgraToRgba(bgra, 2, 1, 8, rgba));
    REQUIRE(rgba.size() == 8);
    CHECK(rgba[0] == 30);
    CHECK(rgba[1] == 20);
    CHECK(rgba[2] == 10);
    CHECK(rgba[3] == 255);
    CHECK(rgba[4] == 60);
    CHECK(rgba[7] == 128);
    CHECK_FALSE(R::SwizzleBgraToRgba(bgra, 2, 1, 7, rgba));

    const std::vector<u8> picture = Bgra(37, 11, 37 * 4, 3);
    std::vector<u8> source;
    REQUIRE(R::SwizzleBgraToRgba(picture, 37, 11, 37 * 4, source));
    std::vector<u8> png;
    std::string error;
    REQUIRE(R::EncodeRgbaPng(source, 37, 11, png, error));
    std::vector<u8> decoded;
    u32 width = 0;
    u32 height = 0;
    REQUIRE(R::DecodeRgbaPng(png, decoded, width, height, error));
    CHECK(width == 37);
    CHECK(height == 11);
    CHECK(decoded == source);

    png[png.size() / 2] ^= 0xFFU;
    CHECK_FALSE(R::DecodeRgbaPng(png, decoded, width, height, error));
    CHECK_FALSE(R::EncodeRgbaPng(source, 0, 11, png, error));
}

TEST_CASE("Replay session: semantic outputs compare structurally", "[video_core][replay]") {
    const std::vector<R::SemanticOutput> expected{
        {R::SemanticKind::SemaphoreValue, 0x1000, 7, 0},
        {R::SemanticKind::QueryResult, 0x2000, 99, 0},
        {R::SemanticKind::ReadbackChecksum, 5, 0xABCDEF, 64},
    };
    CHECK(R::CompareSemantics(expected, expected) == 0);

    std::vector<R::SemanticOutput> shuffled{expected[2], expected[0], expected[1]};
    CHECK(R::CompareSemantics(expected, shuffled) == 0);

    std::vector<R::SemanticOutput> wrong = expected;
    wrong[1].value_lo = 100;
    CHECK(R::CompareSemantics(expected, wrong) >= 1);

    std::vector<R::SemanticOutput> high = expected;
    high[2].value_hi = 63;
    CHECK(R::CompareSemantics(expected, high) >= 1);

    const std::vector<R::SemanticOutput> missing{expected[0], expected[1]};
    CHECK(R::CompareSemantics(expected, missing) == 1);

    std::vector<R::SemanticOutput> extra = expected;
    extra.push_back({R::SemanticKind::SyncpointValue, 4, 1, 0});
    CHECK(R::CompareSemantics(expected, extra) == 1);
}

TEST_CASE("Replay session: an output is read from the address space it was recorded in",
          "[video_core][replay]") {
    // Two spaces map the same address to different buffers; the output was written in space 4.
    R::SemanticOutput in_space_three{R::SemanticKind::SemaphoreValue, 0x1000, 0x11, 0, R::SpaceTag(3)};
    R::SemanticOutput in_space_four{R::SemanticKind::SemaphoreValue, 0x1000, 0x22, 0, R::SpaceTag(4)};
    const std::vector<R::SemanticOutput> expected{in_space_three, in_space_four};
    CHECK(R::CompareSemantics(expected, expected) == 0);
    // The value of the other space does not satisfy an output, and both are counted.
    const std::vector<R::SemanticOutput> crossed{
        R::SemanticOutput{R::SemanticKind::SemaphoreValue, 0x1000, 0x22, 0, R::SpaceTag(3)},
        R::SemanticOutput{R::SemanticKind::SemaphoreValue, 0x1000, 0x11, 0, R::SpaceTag(4)}};
    CHECK(R::CompareSemantics(expected, crossed) == 4);

    const std::array<u32, 2> both{3, 4};
    u32 space = 99;
    CHECK(R::ResolveOutputSpace(R::SpaceTag(4), both, space));
    CHECK(space == 4);
    CHECK(R::ResolveOutputSpace(R::SpaceTag(3), both, space));
    CHECK(space == 3);
    // The named space does not map the range: nothing is read, not another space's bytes.
    const std::array<u32, 1> only_three{3};
    CHECK_FALSE(R::ResolveOutputSpace(R::SpaceTag(4), only_three, space));
    CHECK_FALSE(R::ResolveOutputSpace(R::SpaceTag(0), only_three, space));
    // A file that names no space: one candidate is read, two are refused, none is refused.
    CHECK(R::ResolveOutputSpace(0, only_three, space));
    CHECK(space == 3);
    const std::array<u32, 2> same{3, 3};
    CHECK(R::ResolveOutputSpace(0, same, space));
    CHECK_FALSE(R::ResolveOutputSpace(0, both, space));
    CHECK_FALSE(R::ResolveOutputSpace(0, std::span<const u32>{}, space));
}

TEST_CASE("Replay session: an unsupported state keeps its name through the report and outcome",
          "[video_core][replay]") {
    // The first stop wins and keeps its error; a later stop changes neither text nor kind.
    std::string message;
    R::ReadError kind = R::ReadError::None;
    R::RecordStop(message, kind, "checkpoint cannot be restored: no such channel kind",
                  R::ReadError::UnsupportedState);
    R::RecordStop(message, kind, "later failure", R::ReadError::BadDigest);
    CHECK(message == "checkpoint cannot be restored: no such channel kind");
    CHECK(kind == R::ReadError::UnsupportedState);
    CHECK(R::ErrorKindText(kind) == "unsupported_state");
    CHECK(R::DescribeStop(message, kind) ==
          "checkpoint cannot be restored: no such channel kind (unsupported_state)");

    // An unnamed stop adds no kind and no suffix.
    std::string plain;
    R::ReadError plain_kind = R::ReadError::UnsupportedState;
    R::RecordStop(plain, plain_kind, "channel is created twice", R::ReadError::None);
    CHECK(plain_kind == R::ReadError::None);
    CHECK(R::ErrorKindText(plain_kind).empty());
    CHECK(R::DescribeStop(plain, plain_kind) == "channel is created twice");
}

TEST_CASE("Replay session: the frame count and the present base", "[video_core][replay]") {
    CHECK(R::FramesAsked(10, 0) == 10);
    CHECK(R::FramesAsked(10, 4) == 4);
    CHECK(R::FramesAsked(10, 10) == 10);
    CHECK(R::FramesAsked(10, 99) == 10);
    CHECK(R::FramesAsked(0, 5) == 0);

    CHECK(R::PresentBaseBeforeFirst(100, 0) == 99);
    CHECK(R::PresentBaseBeforeFirst(100, 3) == 96);
    CHECK(R::PresentBaseBeforeFirst(2, 5) == 0);
}

TEST_CASE("Replay session: the exit reason follows what happened", "[video_core][replay]") {
    R::OutcomeInput done;
    done.ran = true;
    done.frames_asked = 8;
    done.frames_replayed = 8;
    CHECK(R::JudgeReplay(done) == Reason::FrameLimit);

    R::OutcomeInput short_run = done;
    short_run.frames_replayed = 7;
    CHECK(R::JudgeReplay(short_run) == Reason::BadArguments);

    R::OutcomeInput failed = done;
    failed.failed = true;
    CHECK(R::JudgeReplay(failed) == Reason::BadArguments);

    R::OutcomeInput nothing_asked = done;
    nothing_asked.frames_asked = 0;
    nothing_asked.frames_replayed = 0;
    CHECK(R::JudgeReplay(nothing_asked) == Reason::BadArguments);

    R::OutcomeInput bad_capture;
    bad_capture.open_error = R::ReadError::BadDigest;
    CHECK(R::JudgeReplay(bad_capture) == Reason::BadArguments);
    R::OutcomeInput bad_pins;
    bad_pins.pins_ok = false;
    CHECK(R::JudgeReplay(bad_pins) == Reason::BadArguments);
    R::OutcomeInput bad_prefix;
    bad_prefix.prefix_ok = false;
    CHECK(R::JudgeReplay(bad_prefix) == Reason::BadArguments);

    R::OutcomeInput lost = short_run;
    lost.device_lost = true;
    CHECK(R::JudgeReplay(lost) == Reason::DeviceLost);
    R::OutcomeInput shader = short_run;
    shader.shader_failure = true;
    CHECK(R::JudgeReplay(shader) == Reason::ShaderFailure);
    R::OutcomeInput io = short_run;
    io.report_io = true;
    CHECK(R::JudgeReplay(io) == Reason::ReportIoError);
    R::OutcomeInput interrupted = short_run;
    interrupted.interrupted = true;
    CHECK(R::JudgeReplay(interrupted) == Reason::Interrupted);
    R::OutcomeInput startup = short_run;
    startup.startup_failed = true;
    CHECK(R::JudgeReplay(startup) == Reason::StartupError);

    // A capture counts as a clean exit only when it is complete (exit code 0 or 76).
    R::CaptureReport capture;
    capture.status = R::SectionStatus::Complete;
    CHECK(R::JudgeCapture(capture) == Reason::FrameLimit);
    capture.status = R::SectionStatus::Incomplete;
    CHECK(R::JudgeCapture(capture) == Reason::CaptureFailed);
    capture.status = R::SectionStatus::Failed;
    CHECK(R::JudgeCapture(capture) == Reason::CaptureFailed);
    capture.status = R::SectionStatus::Off;
    CHECK(R::JudgeCapture(capture) == Reason::CaptureFailed);
    CHECK(Common::RunOutcome::ExitCode(Reason::CaptureFailed) == 76);
    CHECK(Common::RunOutcome::ReasonName(Reason::CaptureFailed) == "capture_failed");

    CHECK(R::StartsWith("startup failed: no seam", R::StartupPrefix));
    CHECK_FALSE(R::StartsWith("startup", R::StartupPrefix));
}

TEST_CASE("Replay session: a consume reaches the pusher where it was recorded",
          "[video_core][replay]") {
    // A fake pusher: a word is a copy (source << 8 | destination) in a small memory, run the
    // moment the words reach it. The recorded order is: two words, an external write to cell 1,
    // then the last two words of the same entry.
    std::array<u32, 8> memory{};
    memory[1] = 7;
    std::vector<std::string> order;
    std::deque<R::PendingSubmit> queue;
    std::string error;

    const auto run = [&](const R::SubmitPiece& piece) {
        for (const u32 word : piece.words) {
            memory[word & 0xFFU] = memory[word >> 8];
            order.push_back("copy " + std::to_string(word & 0xFFU));
        }
    };
    struct Pushed {
        size_t words = 0;
        u64 gpu_va = 0;
        u32 header_flags = 0;
    };
    const auto consume = [&](u32 offset, std::vector<u32> words, u32 flags) {
        R::ConsumeEvent event = ConsumeOf(40, 0, 0x1000, offset, 0);
        event.words = std::move(words);
        event.flags = flags;
        R::SubmitPiece piece;
        REQUIRE(R::FeedConsumeEvent(queue, event, piece, error));
        run(piece);  // the piece points into `event`: use it before the event goes
        return Pushed{piece.words.size(), piece.gpu_va, piece.header_flags};
    };

    REQUIRE(R::QueueSubmitEvent(queue, SubmitOf(40, {R::SubmitEntry{0x1000, 4, 5}}), error));
    REQUIRE(queue.size() == 1);
    const Pushed first = consume(0, {(1U << 8) | 2U, (1U << 8) | 3U}, 0);
    // The submit is still short of its words, and the words it has consumed already ran.
    CHECK_FALSE(R::SubmitComplete(queue.front()));
    CHECK(first.words == 2);
    CHECK(first.gpu_va == 0x1000);
    CHECK(first.header_flags == 5);
    order.push_back("external write");
    memory[1] = 99;
    const Pushed second = consume(2, {(1U << 8) | 4U, (1U << 8) | 5U}, R::ConsumeEntryEnd);
    CHECK(second.gpu_va == 0x1008);   // the address of its own first word
    CHECK(second.header_flags == 0);  // the entry's flags go with its first piece only
    CHECK(queue.empty());

    const std::vector<std::string> expected{"copy 2", "copy 3", "external write", "copy 4",
                                            "copy 5"};
    CHECK(order == expected);
    CHECK(memory[2] == 7);   // the prefix saw the value before the external write
    CHECK(memory[3] == 7);
    CHECK(memory[4] == 99);  // the rest saw the write
    CHECK(memory[5] == 99);
}

TEST_CASE("Replay session: submits are consumed in order and checked", "[video_core][replay]") {
    std::deque<R::PendingSubmit> queue;
    std::string error;
    R::SubmitPiece piece;
    // A submit with no word has nothing to push and does not stay in the queue.
    REQUIRE(R::QueueSubmitEvent(queue, SubmitOf(39, {R::SubmitEntry{0x500, 0, 0}}), error));
    CHECK(queue.empty());

    REQUIRE(R::QueueSubmitEvent(
        queue, SubmitOf(40, {R::SubmitEntry{0x1000, 6, 0}, R::SubmitEntry{0x2000, 2, 1}}), error));
    REQUIRE(R::QueueSubmitEvent(queue, SubmitOf(41, {R::SubmitEntry{0x3000, 2, 0}}), error));
    CHECK(queue.size() == 2);
    // A later submit cannot be consumed before the earlier one is, and a stranger is never queued.
    CHECK_FALSE(R::FeedConsumeEvent(queue, ConsumeOf(41, 0, 0x3000, 0, 2), piece, error));
    CHECK(error == "consume names a submit that is not the oldest one");
    CHECK_FALSE(R::FeedConsumeEvent(queue, ConsumeOf(42, 0, 0x3000, 0, 2), piece, error));
    CHECK(error == "consume names a submit that was never queued");
    // An entry cannot continue before the entry ahead of it is full.
    CHECK_FALSE(R::FeedConsumeEvent(queue, ConsumeOf(40, 1, 0x2000, 0, 2), piece, error));
    CHECK(error == "consume skips an entry that is not full");

    CHECK(R::FeedConsumeEvent(queue, ConsumeOf(40, 0, 0x1000, 0, 4), piece, error));
    CHECK(piece.header_flags == 0);
    CHECK(R::FeedConsumeEvent(queue, ConsumeOf(40, 0, 0x1000, 4, 2), piece, error));
    CHECK(piece.gpu_va == 0x1010);
    CHECK(queue.size() == 2);  // entry 1 is still to come
    CHECK(R::FeedConsumeEvent(queue, ConsumeOf(40, 1, 0x2000, 0, 2), piece, error));
    CHECK(piece.header_flags == 1);
    REQUIRE(queue.size() == 1);
    CHECK(queue.front().event.submit_id == 41);
    CHECK(R::FeedConsumeEvent(queue, ConsumeOf(41, 0, 0x3000, 0, 2), piece, error));
    CHECK(queue.empty());
    CHECK_FALSE(R::FeedConsumeEvent(queue, ConsumeOf(41, 0, 0x3000, 0, 2), piece, error));
    CHECK(error == "consume names a channel with no submit");

    R::PendingSubmit again;
    const R::SubmitEvent event =
        SubmitOf(40, {R::SubmitEntry{0x1000, 6, 0}, R::SubmitEntry{0x2000, 2, 1}});
    REQUIRE(R::BeginSubmit(event, again, error));
    CHECK_FALSE(R::SubmitComplete(again));
    CHECK_FALSE(R::AddConsume(again, ConsumeOf(41, 0, 0x1000, 0, 2), error));   // another submit
    CHECK_FALSE(R::AddConsume(again, ConsumeOf(40, 2, 0x1000, 0, 2), error));   // no such entry
    CHECK_FALSE(R::AddConsume(again, ConsumeOf(40, 0, 0x1000, 2, 2), error));   // gap
    CHECK_FALSE(R::AddConsume(again, ConsumeOf(40, 0, 0x1000, 0, 7), error));   // too many words
    R::ConsumeEvent wrong_address = ConsumeOf(40, 0, 0x1000, 0, 2);
    wrong_address.gpu_va = 0x9000;
    CHECK_FALSE(R::AddConsume(again, wrong_address, error));
    R::ConsumeEvent early_end = ConsumeOf(40, 0, 0x1000, 0, 2);
    early_end.flags = R::ConsumeEntryEnd;
    CHECK_FALSE(R::AddConsume(again, early_end, error));
    CHECK(again.received[0] == 0);  // a refused consume changes nothing

    R::PendingSubmit refused;
    CHECK_FALSE(R::BeginSubmit(SubmitOf(1, {}), refused, error));
    CHECK_FALSE(R::BeginSubmit(SubmitOf(1, {R::SubmitEntry{0x1000, 4, 8}}), refused, error));
    CHECK_FALSE(R::BeginSubmit(SubmitOf(1, {R::SubmitEntry{1ULL << 40, 4, 0}}), refused, error));
    CHECK_FALSE(R::BeginSubmit(
        SubmitOf(1, {R::SubmitEntry{0x1000, R::SubmitHeaderMaxWords + 1U, 0}}), refused, error));
}

TEST_CASE("Replay session: a command list header is built from its parts", "[video_core][replay]") {
    CHECK(R::SubmitHeaderFits(1));
    CHECK(R::SubmitHeaderFits(R::SubmitHeaderMaxWords));
    CHECK_FALSE(R::SubmitHeaderFits(R::SubmitHeaderMaxWords + 1U));
    const u64 raw = R::SubmitHeaderRaw(0x123456000ULL, 17, 0);
    CHECK((raw & 0xFFFFFFFFFFULL) == 0x123456000ULL);
    CHECK(((raw >> 42U) & 0x1FFFFFU) == 17);
    const u64 flagged = R::SubmitHeaderRaw(0x1000, 4, 1);
    CHECK(flagged != R::SubmitHeaderRaw(0x1000, 4, 0));
}

TEST_CASE("Replay session: memory blobs write bytes and check their references",
          "[video_core][replay]") {
    std::vector<u8> backing(64, 0xAA);
    R::RangeWriteLog log;
    std::vector<R::WrittenRange> written;
    std::string error;

    R::MemoryBlob first;
    first.allocation_size = 64;
    first.data = {1, 2, 3, 4, 5, 6, 7, 8};
    first.ranges = {Range(R::RangeKind::Data, 0, 8, 0), Range(R::RangeKind::Zero, 8, 8),
                    Range(R::RangeKind::Undefined, 16, 16)};
    REQUIRE(R::ApplyBlobToBytes(first, 5, backing, log, written, error));
    CHECK(backing[0] == 1);
    CHECK(backing[7] == 8);
    CHECK(backing[8] == 0);
    CHECK(backing[15] == 0);
    CHECK(backing[16] == 0xAA);
    CHECK(written.size() == 2);

    // A delta refers to the bytes of chunk 5 it leaves as they were.
    R::MemoryBlob delta;
    delta.allocation_size = 64;
    delta.data = {9, 9};
    delta.ranges = {Range(R::RangeKind::Ref, 0, 8, 0, 5), Range(R::RangeKind::Data, 32, 2, 0)};
    written.clear();
    REQUIRE(R::ApplyBlobToBytes(delta, 9, backing, log, written, error));
    CHECK(backing[0] == 1);
    CHECK(backing[32] == 9);
    CHECK(written.size() == 1);

    // Chunk 9 wrote [32, 34): a later reference to chunk 5 over it is refused, not guessed.
    R::MemoryBlob stale;
    stale.allocation_size = 64;
    stale.ranges = {Range(R::RangeKind::Ref, 32, 2, 0, 5)};
    CHECK_FALSE(R::ApplyBlobToBytes(stale, 12, backing, log, written, error));
    CHECK_FALSE(error.empty());

    // A reference to a chunk that is not earlier is refused.
    R::MemoryBlob future;
    future.allocation_size = 64;
    future.ranges = {Range(R::RangeKind::Ref, 0, 8, 0, 12)};
    CHECK_FALSE(R::ApplyBlobToBytes(future, 12, backing, log, written, error));
}

TEST_CASE("Replay session: malformed blob ranges are refused before any byte is written",
          "[video_core][replay]") {
    std::vector<u8> backing(32, 0xAA);
    R::RangeWriteLog log;
    std::vector<R::WrittenRange> written;
    std::string error;

    R::MemoryBlob wrong_size;
    wrong_size.allocation_size = 16;
    CHECK_FALSE(R::ApplyBlobToBytes(wrong_size, 1, backing, log, written, error));

    R::MemoryBlob outside;
    outside.allocation_size = 32;
    outside.data = {1, 2, 3, 4};
    outside.ranges = {Range(R::RangeKind::Data, 30, 4, 0)};
    CHECK_FALSE(R::ApplyBlobToBytes(outside, 1, backing, log, written, error));

    R::MemoryBlob overlap;
    overlap.allocation_size = 32;
    overlap.data = {1, 2, 3, 4, 5, 6};
    overlap.ranges = {Range(R::RangeKind::Data, 0, 4, 0), Range(R::RangeKind::Data, 2, 2, 4)};
    CHECK_FALSE(R::ApplyBlobToBytes(overlap, 1, backing, log, written, error));

    R::MemoryBlob short_data;
    short_data.allocation_size = 32;
    short_data.data = {1, 2};
    short_data.ranges = {Range(R::RangeKind::Data, 0, 4, 0)};
    CHECK_FALSE(R::ApplyBlobToBytes(short_data, 1, backing, log, written, error));

    R::MemoryBlob empty_range;
    empty_range.allocation_size = 32;
    empty_range.ranges = {Range(R::RangeKind::Zero, 4, 0)};
    CHECK_FALSE(R::ApplyBlobToBytes(empty_range, 1, backing, log, written, error));
}

TEST_CASE("Replay session: the write log splits and merges ranges", "[video_core][replay]") {
    R::RangeWriteLog log;
    log.Note(0, 100, 5);
    log.Note(40, 20, 9);
    CHECK(log.SegmentCount() == 3);
    CHECK(log.UnchangedSince(0, 40, 5));
    CHECK(log.UnchangedSince(60, 40, 5));
    CHECK_FALSE(log.UnchangedSince(0, 100, 5));
    CHECK_FALSE(log.UnchangedSince(50, 1, 8));
    CHECK(log.UnchangedSince(50, 1, 9));
    CHECK_FALSE(log.UnchangedSince(200, 10, 0)); // Never captured: not a valid reference.

    log.Note(30, 40, 12);
    CHECK_FALSE(log.UnchangedSince(35, 5, 9));
    CHECK(log.UnchangedSince(0, 30, 5));
}

TEST_CASE("Replay session: the index must agree with the header", "[video_core][replay]") {
    R::FileHeader header;
    header.first_frame = 10;
    header.preroll_frames = 1;
    header.finalized_frames = 2;
    R::IndexRecord index;
    const auto frame = [](u64 id, u32 flags) {
        R::FrameIndexEntry entry;
        entry.frame_id = id;
        entry.flags = flags;
        return entry;
    };
    index.frames = {frame(9, R::FrameComplete | R::FramePreroll), frame(10, R::FrameComplete),
                    frame(11, R::FrameComplete)};
    R::FrameSelection selection;
    std::string detail;
    REQUIRE(R::SelectFrames(header, index, selection, detail) == R::ReadError::None);
    CHECK(selection.preroll == 1);
    CHECK(selection.selected == 2);

    R::IndexRecord gap = index;
    gap.frames[2].frame_id = 12;
    CHECK(R::SelectFrames(header, gap, selection, detail) == R::ReadError::BadIndex);

    R::IndexRecord incomplete = index;
    incomplete.frames[1].flags = 0;
    CHECK(R::SelectFrames(header, incomplete, selection, detail) == R::ReadError::NotComplete);

    R::IndexRecord late_preroll = index;
    late_preroll.frames[2].flags = R::FrameComplete | R::FramePreroll;
    CHECK(R::SelectFrames(header, late_preroll, selection, detail) == R::ReadError::BadIndex);

    R::IndexRecord short_index = index;
    short_index.frames.pop_back();
    CHECK(R::SelectFrames(header, short_index, selection, detail) == R::ReadError::BadIndex);

    R::FileHeader empty = header;
    empty.finalized_frames = 0;
    CHECK(R::SelectFrames(empty, index, selection, detail) == R::ReadError::NotComplete);
}

TEST_CASE("Replay session: presents, fences and live pictures are validated",
          "[video_core][replay]") {
    std::string error;
    R::PresentRecord present;
    present.present_id = 12;
    R::PresentLayer layer;
    layer.alloc_id = 4;
    layer.width = 1280;
    layer.height = 720;
    layer.stride = 5120;
    layer.crop_right = 1280;
    layer.crop_bottom = 720;
    present.layers = {layer};
    CHECK(R::ValidatePresent(present, 12, error));
    CHECK_FALSE(R::ValidatePresent(present, 13, error));

    R::PresentRecord no_layer = present;
    no_layer.layers.clear();
    CHECK_FALSE(R::ValidatePresent(no_layer, 12, error));
    R::PresentRecord inverted = present;
    inverted.layers[0].crop_right = -1;
    CHECK_FALSE(R::ValidatePresent(inverted, 12, error));
    R::PresentRecord blending = present;
    blending.layers[0].blending = 3;
    CHECK_FALSE(R::ValidatePresent(blending, 12, error));
    R::PresentRecord huge = present;
    huge.layers[0].width = 9000;
    CHECK_FALSE(R::ValidatePresent(huge, 12, error));

    R::FenceEntry syncpoint;
    syncpoint.kind = R::FenceKind::SyncpointRaised;
    syncpoint.id = 5;
    syncpoint.value = 100;
    CHECK(R::ValidateFence(syncpoint, error));
    CHECK(R::FenceWriteBytes(syncpoint) == 0);
    syncpoint.id = 192;
    CHECK_FALSE(R::ValidateFence(syncpoint, error));

    R::FenceEntry semaphore;
    semaphore.kind = R::FenceKind::SemaphoreWrite;
    semaphore.id = 0x4000;
    semaphore.value = 7;
    CHECK(R::ValidateFence(semaphore, error));
    CHECK(R::FenceWriteBytes(semaphore) == 4);
    semaphore.value = 1ULL << 40;
    CHECK_FALSE(R::ValidateFence(semaphore, error));
    semaphore.aux = 8;
    CHECK(R::FenceWriteBytes(semaphore) == 8);
    CHECK(R::ValidateFence(semaphore, error));

    R::FenceEntry query;
    query.kind = R::FenceKind::QueryValue;
    query.id = 0x4000;
    CHECK(R::FenceWriteBytes(query) == 8);
    R::FenceEntry unknown;
    unknown.kind = static_cast<R::FenceKind>(77);
    CHECK_FALSE(R::ValidateFence(unknown, error));

    R::EndPicture picture;
    picture.frame_id = 12;
    picture.width = 4;
    picture.height = 2;
    picture.stride_bytes = 16;
    picture.pixels = Bgra(4, 2, 16, 1);
    picture.pixel_checksum = R::Fnv1a64(picture.pixels);
    CHECK(R::ValidateLivePicture(picture, 12, error));
    CHECK_FALSE(R::ValidateLivePicture(picture, 13, error));

    R::EndPicture guest = picture;
    guest.kind = R::PictureKind::GuestFrame;
    CHECK_FALSE(R::ValidateLivePicture(guest, 12, error));
    R::EndPicture damaged = picture;
    damaged.pixels[3] ^= 1U;
    CHECK_FALSE(R::ValidateLivePicture(damaged, 12, error));
    R::EndPicture short_pixels = picture;
    short_pixels.pixels.pop_back();
    CHECK_FALSE(R::ValidateLivePicture(short_pixels, 12, error));
    R::EndPicture narrow = picture;
    narrow.stride_bytes = 8;
    CHECK_FALSE(R::ValidateLivePicture(narrow, 12, error));
}

TEST_CASE("Replay session: a frame line is one line and the options carry over",
          "[video_core][replay]") {
    R::ReplayFrameRecord record;
    record.frame_id = 31;
    record.width = 1280;
    record.height = 720;
    record.live_checksum = 5;
    record.replay_checksum = 6;
    record.replay_ns = 1234;
    const std::string line = R::ReplayFrameLine(record);
    CHECK_FALSE(line.empty());
    CHECK(line.find('\n') == std::string::npos);
    CHECK(line.find("31") != std::string::npos);
    CHECK(R::LivePictureName(31) != R::ReplayPictureName(31));
    CHECK(R::LivePictureName(31).find("31") != std::string::npos);

    R::Options options;
    options.replay_path = "run.capture";
    options.replay_images_dir = "pictures";
    options.replay_benchmark = true;
    options.frame_times_path = "times.jsonl";
    const R::ReplayConfig config = R::ReplayConfigFromOptions(options, 12);
    CHECK(config.capture_path == "run.capture");
    CHECK(config.images_dir == "pictures");
    CHECK(config.benchmark);
    CHECK(config.max_frames == 12);
    CHECK(config.frame_times_path == "times.jsonl");
}

TEST_CASE("Replay session: open refuses what is not a clean capture", "[video_core][replay]") {
    std::string detail;

    SECTION("a missing file") {
        R::ReplaySession session;
        R::ReplayConfig config;
        config.capture_path =
            (std::filesystem::temp_directory_path() / "eglantine_no_such_capture.bin").string();
        CHECK(session.Open(config, detail) == R::ReadError::Truncated);
        CHECK_FALSE(detail.empty());
    }

    SECTION("a provisional file") {
        const auto path = TempFile("eglantine_replay_open.capture.partial", ValidHeaderBytes());
        R::ReplaySession session;
        R::ReplayConfig config;
        config.capture_path = path.string();
        CHECK(session.Open(config, detail) == R::ReadError::NotComplete);
        std::filesystem::remove(path);
    }

    SECTION("a file that is not a capture") {
        const auto path = TempFile("eglantine_replay_open_junk.capture", std::vector<u8>(512, 0x41));
        R::ReplaySession session;
        R::ReplayConfig config;
        config.capture_path = path.string();
        CHECK(session.Open(config, detail) == R::ReadError::BadMagic);
        std::filesystem::remove(path);
    }

    SECTION("a short file") {
        const auto path = TempFile("eglantine_replay_open_short.capture", std::vector<u8>(40, 0));
        R::ReplaySession session;
        R::ReplayConfig config;
        config.capture_path = path.string();
        CHECK(session.Open(config, detail) == R::ReadError::Truncated);
        std::filesystem::remove(path);
    }

    SECTION("pins before open, and a second open") {
        R::ReplaySession session;
        std::string error;
        CHECK_FALSE(session.ApplyPins(error));
        CHECK_FALSE(error.empty());

        R::ReplayConfig config;
        config.capture_path = "missing.capture";
        CHECK(session.Open(config, detail) != R::ReadError::None);
        CHECK(session.Open(config, detail) == R::ReadError::BadOrdering);
        CHECK_FALSE(session.ApplyPins(error));
    }
}
