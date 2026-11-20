// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen: the GPU_CHECKPOINT payload, the state of the GPU at the start of the first recorded
// frame. Slice B implements Encode/Decode and the three functions that read and apply live GPU
// state (checkpoint.cpp); slice C calls RestoreGpuCheckpoint before the first MAP chunk and
// before the first event, then asks RestoredChannel / RestoredAddressSpace for the objects it made.
//
// The checkpoint is taken at a clean cut: no pushbuffer list is queued or half consumed on any
// channel, so the command stream after it is fully described by the Submit and Consume events.
// The writer takes the checkpoint at the first accepted present P with
// first_frame - preroll_frames - 1 <= P <= first_frame - 1 that is a clean cut (it retries at the
// next present). The first stored frame is then start_frame = P + 1, and the header's
// preroll_frames records first_frame - start_frame, which is smaller than the requested value
// when the cut came late. When no clean cut exists up to first_frame - 1 the capture stops as
// incomplete (IncompleteCheckpointUnavailable). Mappings and memory are not part of the
// checkpoint: they are the MAP and MEMORY_BLOB chunks that follow it.
//
// Payload: u32 sub_record_count (<= MaxSubRecords), then per sub-record
//   u32 kind (SubRecordKind), u32 version (1), u64 body_length, body
// Sub-records are in ascending kind then channel order. An unknown kind is UnsupportedState
// (the checkpoint is required). Bodies, all little-endian, counts bounded and checked as in
// format.h:
//   ChannelTable       u32 count (<= MaxChannels), count * {u32 channel_id, u32 address_space_id,
//                      u32 flags (ChannelFlag), u32 reserved}
//   AddressSpaces      u32 count (<= MaxChannels), count * {u32 address_space_id,
//                      u32 address_space_bits, u32 big_page_bits, u32 flags, u64 split_address}
//   EngineRegisters    u32 channel_id, u32 engine_class (EngineClass), u32 reg_count
//                      (<= MaxEngineRegisters), reg_count * u32: the register file in method order
//                      (KeplerCompute of a Switch 2 channel: the native 0x1000-word file, then the
//                      NUM_REGS-word `regs` copy that lags it; Host: the
//                      puller's 0x800 registers, then the engine id bound to each of 8 subchannels)
//   SubchannelBindings u32 channel_id, u32 count (<= 8), count * u32 engine_class
//   MacroSection       u32 channel_id (0xFFFFFFFF = shared), u32 section (MacroSectionKind),
//                      u32 word_count (<= MaxMacroWords), word_count * u32. Code: Switch 1 runs of
//                      {method, count, words}, Switch 2 the macro RAM up to its last nonzero word;
//                      Positions: the 0x80 macro start positions; Cache: Switch 1 runs of
//                      {method, hash_lo, hash_hi} of the compiled macros (derived state, checked
//                      against the restored code, never restored); ComputeCode, ComputePositions:
//                      the compute engine's own macro RAM and positions (Switch 2, shared);
//                      DataRam: the channel's 0xc00-word MME data RAM; NativeState: shared =
//                      {graphics write offset, compute write offset}, per channel = {compute bind
//                      pointer, compute macros uploaded}
//   ShadowRam          u32 channel_id, u32 mode, u32 word_count (<= MaxMacroWords),
//                      word_count * u32
//   DmaState           u32 channel_id, u32 method, u32 subchannel, u32 method_count,
//                      u32 length_pending, u32 flags (DmaStateFlag), u64 dma_get,
//                      u64 dma_word_offset, u32 parked_count (<= MaxParkedMethods), u32 reserved,
//                      u64 split_method_end (where the entry that ended inside a method ends),
//                      parked_count * {u32 method, u32 subchannel, u32 method_count, u32 flags,
//                      u64 resume_address}
//   Fences             u32 count, count * {u32 kind (FenceKind), u32 channel_id, u64 id,
//                      u64 value, u64 aux}  (current syncpoint and semaphore state; the values a
//                      replay compares its own against at FRAME_END)
//   EngineState        u32 channel_id, u32 state (EngineStateKind), u32 word_count
//                      (<= MaxMacroWords), word_count * u32. RootTables: 8 tables of 64 words,
//                      then selected, offset, loaded; ConstBufferBindings: per shader stage and
//                      slot {address_lo, address_hi, size, enabled}
// The meaning of a register or macro word is whatever the emulator's own state holds; the
// checkpoint is only valid for the build and settings pinned in the header and SESSION chunk, and
// a replayer that cannot restore a sub-record fails with UnsupportedState rather than skipping it.

#pragma once

#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/replay/events.h"
#include "video_core/replay/format.h"

namespace Core {
class System;
}

namespace Tegra {
class GPU;
class MemoryManager;
namespace Control {
struct ChannelState;
}
} // namespace Tegra

namespace VideoCore::Replay {

enum class SubRecordKind : u32 {
    ChannelTable = 1,
    AddressSpaces = 2,
    EngineRegisters = 3,
    SubchannelBindings = 4,
    MacroSection = 5,
    ShadowRam = 6,
    DmaState = 7,
    Fences = 8,
    EngineState = 9,
};

enum class EngineClass : u32 {
    Maxwell3D = 1,
    KeplerCompute = 2,
    Fermi2D = 3,
    MaxwellDma = 4,
    KeplerMemory = 5,
    Host = 6,
};

enum class MacroSectionKind : u32 {
    Code = 1,
    Positions = 2,
    Cache = 3,
    ComputeCode = 4,
    ComputePositions = 5,
    DataRam = 6,
    NativeState = 7,
};

enum class EngineStateKind : u32 { RootTables = 1, ConstBufferBindings = 2 };

enum DmaStateFlag : u32 {
    DmaNonIncrementing = 1U << 0,
    DmaLastCall = 1U << 1,
    DmaIncrementOnce = 1U << 2,
    DmaSignalSync = 1U << 3,
};

inline constexpr u32 SharedChannel = 0xFFFFFFFFu;

struct ChannelEntry {
    u32 channel_id = 0;
    u32 address_space_id = 0;
    u32 flags = 0;
};

struct AddressSpaceEntry {
    u32 address_space_id = 0;
    u32 address_space_bits = 0;
    u32 big_page_bits = 0;
    u32 flags = 0;
    u64 split_address = 0;
};

struct EngineRegisters {
    u32 channel_id = 0;
    EngineClass engine_class = EngineClass::Maxwell3D;
    std::vector<u32> registers;
};

struct SubchannelBinding {
    u32 channel_id = 0;
    std::vector<EngineClass> classes;
};

struct MacroSection {
    u32 channel_id = SharedChannel;
    MacroSectionKind kind = MacroSectionKind::Code;
    std::vector<u32> words;
};

struct EngineStateSection {
    u32 channel_id = 0;
    EngineStateKind kind = EngineStateKind::RootTables;
    std::vector<u32> words;
};

struct ShadowRam {
    u32 channel_id = 0;
    u32 mode = 0;
    std::vector<u32> words;
};

struct ParkedMethod {
    u32 method = 0;
    u32 subchannel = 0;
    u32 method_count = 0;
    u32 flags = 0;
    u64 resume_address = 0;
};

struct DmaState {
    u32 channel_id = 0;
    u32 method = 0;
    u32 subchannel = 0;
    u32 method_count = 0;
    u32 length_pending = 0;
    u32 flags = 0;
    u64 dma_get = 0;
    u64 dma_word_offset = 0;
    std::vector<ParkedMethod> parked;
    u64 split_method_end = 0;
};

struct CheckpointFence {
    FenceKind kind = FenceKind::SyncpointRaised;
    u32 channel_id = 0;
    u64 id = 0;
    u64 value = 0;
    u64 aux = 0;
};

struct GpuCheckpoint {
    std::vector<ChannelEntry> channels;
    std::vector<AddressSpaceEntry> address_spaces;
    std::vector<EngineRegisters> engines;
    std::vector<SubchannelBinding> subchannels;
    std::vector<MacroSection> macros;
    std::vector<ShadowRam> shadows;
    std::vector<DmaState> dma;
    std::vector<CheckpointFence> fences;
    std::vector<EngineStateSection> states;
};

void Encode(const GpuCheckpoint& value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, GpuCheckpoint& out);

// Applies the state writes an engine holds back until its next method (its method sink), channel
// by channel with the channel bound. The next method applies them first anyway, so the running
// game cannot tell. Call it only with the GPU thread between two command lists (see
// CaptureHost::RunQuiescent); CaptureGpuCheckpoint refuses an engine that still holds some.
void SettleEngineSinks(Tegra::GPU& gpu);
// Reads the live state of the GPU at a clean cut. False with a message when there is no clean cut
// or a part of the state cannot be written down; nothing is skipped silently. `retry_later` (when
// given) tells a refusal that a later cut may not have (a list still waiting on a semaphore, an
// operation half way) from one that holds at any time.
[[nodiscard]] bool CaptureGpuCheckpoint(Tegra::GPU& gpu, GpuCheckpoint& out, std::string& error,
                                        bool* retry_later = nullptr);
// Checks what can be checked without a GPU: channel ids sequential from 1 in recorded order,
// every address space and channel referenced exists once, every sub-record names a known channel,
// engine classes and block sizes are the ones this build has. False with a message naming the
// section. RestoreGpuCheckpoint runs it first.
[[nodiscard]] bool ValidateCheckpointForRestore(const GpuCheckpoint& checkpoint,
                                                std::string& error);
// Creates the address spaces and channels (empty address spaces: the MAP chunks fill them) and
// writes the engine, macro, DMA and fence state into a GPU that has no channel yet, then marks all
// engine state dirty once. No method runs, so no draw or other side effect happens. False with a
// message naming the section when any sub-record cannot be restored; the GPU keeps the channels it
// had made by then (it cannot release one), so a refused restore needs a fresh GPU. The registry
// (RestoredChannel, RestoredAddressSpace) is filled only on success; ReleaseRestoredGpuState
// empties it. The process must already be in the checkpoint's Switch 1 or Switch 2 mode
// (Tegra::g_nx2_ampere). Call it before the first MAP chunk and before the first event.
[[nodiscard]] bool RestoreGpuCheckpoint(Tegra::GPU& gpu, const GpuCheckpoint& checkpoint,
                                        std::string& error);
// The objects the last successful restore made, by recorded id; nullptr when there is none. These
// are the replay host's `channel` and `address_space` seams.
[[nodiscard]] Tegra::Control::ChannelState* RestoredChannel(Core::System& system, u32 channel_id);
[[nodiscard]] Tegra::MemoryManager* RestoredAddressSpace(Core::System& system,
                                                         u32 address_space_id);
// Drops the registry of restored objects (the GPU keeps its channels: it cannot release them).
void ReleaseRestoredGpuState();

} // namespace VideoCore::Replay
