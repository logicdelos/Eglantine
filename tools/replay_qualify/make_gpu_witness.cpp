// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Original two-frame scene. Uses exactly the file writer and reader used by the emulator.
// No emulator process, proprietary files, window, keys or platform-specific image library.
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include "video_core/replay/checkpoint.h"
#include "video_core/replay/format_io.h"

namespace R = VideoCore::Replay;
namespace {
constexpr u32 Width = 32, Height = 18;
constexpr u64 Base = 0x10000, Fence = 0x20000, AllocationSize = Fence - Base + 4096;
// 32 RGBA pixels occupy two 64-byte GOBs; block-height 4 (log2) pads to 128 rows.
constexpr u32 TiledBytes = Width * 4 * 128;
constexpr u32 InlineNop = (4u << 29) | 2u;

R::GpuCheckpoint MaxwellReset() {
    R::GpuCheckpoint c;
    c.channels.push_back({1, 0, 0});
    c.address_spaces.push_back({0, 40, 16, 0, 1ULL << 32});
    // Maxwell hardware register files: sizes mirror ValidateCheckpointForRestore, not bytes
    // obtained from a running process. Start the pusher at a method boundary, with no bindings.
    constexpr std::array<u32, 6> registers{0xe00, 0xcf8, 0x258, 0x800, 0x7f, 0x40 + 8};
    for (u32 i = 0; i < registers.size(); ++i) {
        c.engines.push_back({1, static_cast<R::EngineClass>(i + 1), std::vector<u32>(registers[i])});
    }
    c.subchannels.push_back({1, {}});
    c.shadows.push_back({1, 0, std::vector<u32>(registers.front())});
    c.dma.push_back({.channel_id = 1});
    c.macros.push_back({1, R::MacroSectionKind::Code, {}});
    c.macros.push_back({1, R::MacroSectionKind::Positions, std::vector<u32>(128)});
    c.states.push_back({1, R::EngineStateKind::RootTables, std::vector<u32>(8 * 64 + 3)});
    c.states.push_back({1, R::EngineStateKind::ConstBufferBindings, std::vector<u32>(5 * 18 * 4)});
    return c;
}

// A block of opaque monochrome needs no swizzle: all texels have the same value in any order.
std::vector<u8> Pixels(u32 bytes, u8 gray) {
    std::vector<u8> data(bytes, gray);
    for (u32 i = 3; i < bytes; i += 4) data[i] = 255;
    return data;
}

void Make(const std::filesystem::path& path, std::string_view mode) {
    R::FileHeader h;
    const bool omit_page = mode == "--missing-page";
    const bool lost_effect = mode == "--lost-effect";
    const bool preroll = mode == "--preroll";
    h.first_frame = preroll ? 2 : 1;
    h.preroll_frames = preroll ? 1 : 0;
    h.requested_frames = preroll ? 1 : 2;
    h.flags = R::HeaderHasLivePictures;
    h.optional_features = R::OptionalLivePictures;
    h.gpu_architecture = static_cast<u32>(R::GpuArchitecture::Maxwell);
    std::string why;
    R::CaptureWriter out;
    if (!out.Open(path, h, why)) throw std::runtime_error(why);
    std::vector<u8> payload;
    const auto append = [&](R::ChunkType t, u64 frame, const auto& value) {
        payload.clear();
        R::Encode(value, payload);
        if (!out.Append(t, frame, payload, why)) throw std::runtime_error(why);
    };
    R::SessionRecord session;
    session.pins = {{static_cast<u32>(R::SettingKey::Backend), 1},
                    {static_cast<u32>(R::SettingKey::ScalingFilter), 0},
                    {static_cast<u32>(R::SettingKey::ResolutionSetup), 3},
                    {static_cast<u32>(R::SettingKey::AntiAliasing), 0}};
    append(R::ChunkType::Session, 0, session);
    append(R::ChunkType::GpuCheckpoint, 1, MaxwellReset());
    for (u64 frame = 1; frame <= 2; ++frame) {
        const u64 first = out.NextSequence();
        const u8 gray = frame == 1 ? 0 : 255;
        R::MemoryBlob blob{.alloc_id = 1, .generation = 1,
                           .flags = frame == 1 ? R::BlobInitial : R::BlobDelta,
                           .allocation_size = AllocationSize};
        blob.data = Pixels(TiledBytes, gray);
        blob.ranges.push_back({0, TiledBytes, R::RangeKind::Data});
        if (frame == 1) {
            blob.ranges.push_back({TiledBytes, AllocationSize - TiledBytes - (omit_page ? 4096 : 0),
                                   R::RangeKind::Zero});
        }
        append(frame == 1 ? R::ChunkType::MemoryBlob : R::ChunkType::ExternalWrite, frame, blob);
        if (frame == 1) {
            append(R::ChunkType::Map, frame, std::array{R::MapEntry{0, 0, Base, AllocationSize, 1, 0, 1, 0}});
        }
        R::GpuEvent submit, consume;
        submit.submit = {.channel_id = 1, .submit_id = frame, .entries = {{Base + TiledBytes, 1, 0}}};
        consume.kind = R::GpuEventKind::Consume;
        consume.consume = {.channel_id = 1, .submit_id = frame, .consume_id = frame,
                           .gpu_va = Base + TiledBytes, .flags = R::ConsumeEntryEnd, .words = {InlineNop}};
        append(R::ChunkType::GpuEvent, frame, std::array{submit, consume});
        append(R::ChunkType::Fence, frame, std::array{R::FenceEntry{R::FenceKind::SemaphoreWrite, 1, Fence, frame}});
        R::PresentRecord present{.path = R::PresentPath::LegacyBufferQueue, .present_id = frame,
                                 .output_id = frame};
        if (mode == "--unmet-present-fence" && frame == 2) {
            present.fence_id = 3;
            present.fence_value = 1; // No syncpoint raise was captured; GPU idle cannot satisfy it.
        }
        present.layers.push_back({.alloc_id = 1, .generation = 1, .width = Width, .height = Height,
                                  .stride = Width, .pixel_format = 1, .layer_stack_mask = 1});
        append(R::ChunkType::Present, frame, present);
        R::EndPicture image{.frame_id = frame, .width = Width, .height = Height, .stride_bytes = Width * 4};
        image.pixels = Pixels(Width * Height * 4, gray);
        image.pixel_checksum = R::Fnv1a64(image.pixels);
        const bool warmup = preroll && frame == 1;
        if (!warmup) append(R::ChunkType::EndPicture, frame, image);
        R::FrameEndRecord end{.frame_id = frame, .flags = R::FrameComplete | (warmup ? R::FramePreroll : R::FrameHasLivePicture),
                              .gpu_words = 1, .first_sequence = first, .last_sequence = out.NextSequence(),
                              .output_id = frame, .call_count = 0,
                              .effect_count = lost_effect && frame == 2 ? 5U : 4U};
        end.outputs.push_back({R::SemanticKind::SemaphoreValue, Fence, frame, 0, R::SpaceTag(0)});
        append(R::ChunkType::FrameEnd, frame, end);
    }
    h.finalized_frames = h.requested_frames;
    R::CompleteRecord complete{.status = R::CompleteStatus::Complete, .gpu_words = 2,
                               .initial_backing_bytes = AllocationSize, .delta_bytes = TiledBytes,
                               .dependency_complete = 1};
    if (!out.Finalize(h, complete, why)) throw std::runtime_error(why);
    R::CaptureReader reader;
    if (reader.Open(path, why) != R::ReadError::None) throw std::runtime_error(why);
    const auto valid = reader.Validate(why);
    if (omit_page ? valid != R::ReadError::BadMapping : valid != R::ReadError::None) {
        throw std::runtime_error("unexpected validation result: " + why);
    }
}
} // namespace

int main(int argc, char** argv) try {
    const std::string_view mode = argc == 3 ? argv[2] : "";
    if (argc < 2 || argc > 3 || (!mode.empty() && mode != "--missing-page" && mode != "--lost-effect" &&
                                 mode != "--preroll" && mode != "--unmet-present-fence")) {
        std::cerr << "usage: make_gpu_witness OUTPUT.egcap [--missing-page | --lost-effect | --preroll | --unmet-present-fence]\n";
        return 2;
    }
    Make(argv[1], mode);
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
