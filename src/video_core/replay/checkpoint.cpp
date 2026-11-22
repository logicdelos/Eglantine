// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// CaptureGpuCheckpoint: the live GPU state at a clean cut (see checkpoint.h). Read-only: the
// caller runs it inside GPU::RunQuiesced and no channel is allocated or released meanwhile.
// SettleEngineSinks, called first, is the one write: it applies the engines' held-back writes.
// RestoreGpuCheckpoint: the inverse, into a GPU without channels.
//
// Per channel the capture holds: the registers of all six engine classes (Maxwell3D, compute,
// Fermi2D, MaxwellDMA, KeplerMemory and the puller's host methods), shadow RAM, root tables, the
// bound constant buffers, the macro code with its positions and (Switch 1) the compiled-macro
// cache keys, the Turing data RAM and compute macro state, the subchannel bindings, the DMA
// pusher state with its parked methods, and the channel's syncpoint. State that can only exist in
// the middle of an operation (a macro being fed, an unfinished inline upload, queued instances, a
// pending acquire) is not written down: its presence is a refusal that names it, so the capture
// is incomplete rather than silently wrong. Two things are not held by a channel and are not
// captured: address spaces no channel uses, and the Host1x syncpoint values (the replay session
// raises those itself).

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/container_hash.h"
#include "core/core.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/native_qmd.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/replay/checkpoint.h"

namespace VideoCore::Replay {

namespace {

bool Refuse(std::string& error, std::string text) {
    error = std::move(text);
    return false;
}

std::string ChannelName(const Tegra::Control::ChannelState& channel) {
    return "channel " + std::to_string(channel.bind_id);
}

bool ClassOf(Tegra::Engines::EngineTypes type, EngineClass& out) {
    using Tegra::Engines::EngineTypes;
    switch (type) {
    case EngineTypes::Maxwell3D:
        out = EngineClass::Maxwell3D;
        return true;
    case EngineTypes::KeplerCompute:
        out = EngineClass::KeplerCompute;
        return true;
    case EngineTypes::Fermi2D:
        out = EngineClass::Fermi2D;
        return true;
    case EngineTypes::MaxwellDMA:
        out = EngineClass::MaxwellDma;
        return true;
    case EngineTypes::KeplerMemory:
        out = EngineClass::KeplerMemory;
        return true;
    case EngineTypes::Nv01Timer:
        break;
    }
    return false;
}

template <typename Array>
void AddEngine(GpuCheckpoint& out, u32 channel, EngineClass engine_class, const Array& registers) {
    EngineRegisters entry;
    entry.channel_id = channel;
    entry.engine_class = engine_class;
    entry.registers.assign(registers.begin(), registers.end());
    out.engines.push_back(std::move(entry));
}

// Switch 1 macros: the uploaded code per method, as {method, count, words} runs by method.
void SerializeUploadedCode(const Tegra::MacroEngine& engine, std::vector<u32>& words) {
    std::map<u32, const std::vector<u32>*> sorted;
    for (const auto& [method, code] : engine.uploaded_macro_code) {
        sorted.emplace(method, &code);
    }
    for (const auto& [method, code] : sorted) {
        words.push_back(method);
        words.push_back(static_cast<u32>(code->size()));
        words.insert(words.end(), code->begin(), code->end());
    }
}

// A section of the macro RAM or positions of a channel, in the order the format wants them.
void PutMacro(GpuCheckpoint& out, u32 channel, MacroSectionKind kind, std::vector<u32> words) {
    MacroSection section;
    section.channel_id = channel;
    section.kind = kind;
    section.words = std::move(words);
    out.macros.push_back(std::move(section));
}

// The macro RAM up to its last nonzero word (restore zero-fills the rest).
std::vector<u32> TrimmedCode(const Tegra::MacroEngine& engine) {
    size_t used = engine.native_code.size();
    while (used > 0 && engine.native_code[used - 1] == 0) {
        --used;
    }
    return std::vector<u32>(engine.native_code.begin(),
                            engine.native_code.begin() + static_cast<std::ptrdiff_t>(used));
}

// Macro state of one channel. False with `failure` set when it is not in a state the format holds.
bool AddMacros(GpuCheckpoint& out, u32 channel, Tegra::Engines::Maxwell3D& maxwell,
               Tegra::Engines::KeplerCompute& compute, bool native, bool& shared_done,
               std::string& failure) {
    Tegra::MacroEngine& engine = maxwell.GetNativeMacroEngine();
    const std::span<u32> positions = maxwell.GetNativeMacroPositions();
    if (!native) {
        std::vector<u32> code;
        SerializeUploadedCode(engine, code);
        PutMacro(out, channel, MacroSectionKind::Code, std::move(code));
        PutMacro(out, channel, MacroSectionKind::Positions,
                 std::vector<u32>(positions.begin(), positions.end()));
        // The compiled-macro cache is derived from the uploaded code: it is recorded as the hash
        // of each entry's code so restore can prove it rebuilds the same one.
        std::map<u32, u64> hashes;
        for (const auto& [method, info] : engine.macro_cache) {
            const auto code_it = engine.uploaded_macro_code.find(method);
            if (code_it == engine.uploaded_macro_code.end() ||
                info.hash != Common::HashRange(code_it->second.begin(), code_it->second.end())) {
                failure = "channel " + std::to_string(channel) +
                          " has a compiled macro that does not match its uploaded code";
                return false;
            }
            hashes.emplace(method, info.hash);
        }
        std::vector<u32> cache;
        for (const auto& [method, hash] : hashes) {
            cache.push_back(method);
            cache.push_back(static_cast<u32>(hash));
            cache.push_back(static_cast<u32>(hash >> 32));
        }
        if (!cache.empty()) {
            PutMacro(out, channel, MacroSectionKind::Cache, std::move(cache));
        }
        return true;
    }

    // Switch 2: the macro context is one object shared by every channel; its RAM and write
    // offsets are written once. The per-method caches (memo, JIT, call counters) are derived from
    // the RAM and rebuilt on use.
    const Tegra::NativeMacroContext* context = maxwell.GetNativeMacroContextPtr();
    if (context == nullptr || !compute.IsNativeMode()) {
        failure = "channel " + std::to_string(channel) +
                  " has Turing macros without the shared macro context";
        return false;
    }
    if (!shared_done) {
        shared_done = true;
        PutMacro(out, SharedChannel, MacroSectionKind::Code, TrimmedCode(context->engine));
        PutMacro(out, SharedChannel, MacroSectionKind::Positions,
                 std::vector<u32>(context->positions.begin(), context->positions.end()));
        PutMacro(out, SharedChannel, MacroSectionKind::ComputeCode,
                 TrimmedCode(context->compute_engine));
        PutMacro(out, SharedChannel, MacroSectionKind::ComputePositions,
                 std::vector<u32>(context->compute_positions.begin(),
                                  context->compute_positions.end()));
        PutMacro(out, SharedChannel, MacroSectionKind::NativeState,
                 {context->engine.native_write_offset, context->compute_engine.native_write_offset});
    }
    const auto& data_ram = maxwell.GetNativeDataRam();
    PutMacro(out, channel, MacroSectionKind::DataRam,
             std::vector<u32>(data_ram.begin(), data_ram.end()));
    PutMacro(out, channel, MacroSectionKind::NativeState,
             {compute.NativeBindPointer(), compute.NativeComputeUploaded() ? 1U : 0U});
    return true;
}

// Root tables and bound constant buffers: Maxwell3D state that no register holds.
void AddEngineState(GpuCheckpoint& out, u32 channel, const Tegra::Engines::Maxwell3D& maxwell) {
    EngineStateSection roots;
    roots.channel_id = channel;
    roots.kind = EngineStateKind::RootTables;
    for (const auto& table : maxwell.root_tables.tables) {
        roots.words.insert(roots.words.end(), table.begin(), table.end());
    }
    roots.words.push_back(maxwell.root_tables.selected);
    roots.words.push_back(maxwell.root_tables.offset);
    roots.words.push_back(maxwell.root_tables.loaded ? 1U : 0U);
    out.states.push_back(std::move(roots));

    EngineStateSection buffers;
    buffers.channel_id = channel;
    buffers.kind = EngineStateKind::ConstBufferBindings;
    for (const auto& stage : maxwell.state.shader_stages) {
        for (const auto& binding : stage.const_buffers) {
            buffers.words.push_back(static_cast<u32>(binding.address));
            buffers.words.push_back(static_cast<u32>(binding.address >> 32));
            buffers.words.push_back(static_cast<u32>(binding.size));
            buffers.words.push_back(binding.enabled ? 1U : 0U);
        }
    }
    out.states.push_back(std::move(buffers));
}

} // namespace

namespace {

namespace Engines = Tegra::Engines;

// Sizes of the blocks a checkpoint holds, the ones the engines of this build have.
constexpr size_t MaxwellDmaRegisterWords = 0x800; // MaxwellDMA::Regs is private
constexpr size_t NativeComputeRegisterWords = 0x1000;
constexpr size_t NativeMacroRamWords = 0x8000;
constexpr size_t MacroPositionWords = 0x80;
constexpr size_t DataRamWords = 0xc00;
constexpr size_t RootTableWords = 8 * 64 + 3;
constexpr size_t ConstBufferWords =
    Engines::Maxwell3D::Regs::MaxShaderStage * Engines::Maxwell3D::Regs::MaxConstBuffers * 4;
constexpr u32 EngineClassCount = 6;
constexpr u32 MaxSubchannels = 8;
constexpr u32 MaxSyncpoints = 192;

size_t ExpectedRegisterWords(EngineClass engine_class, bool turing) {
    switch (engine_class) {
    case EngineClass::Maxwell3D:
        return Engines::Maxwell3D::Regs::NUM_REGS;
    case EngineClass::KeplerCompute:
        return turing ? NativeComputeRegisterWords + Engines::KeplerCompute::Regs::NUM_REGS
                      : Engines::KeplerCompute::Regs::NUM_REGS;
    case EngineClass::Fermi2D:
        return Engines::Fermi2D::Regs::NUM_REGS;
    case EngineClass::MaxwellDma:
        return MaxwellDmaRegisterWords;
    case EngineClass::KeplerMemory:
        return Engines::KeplerMemory::Regs::NUM_REGS;
    case EngineClass::Host:
        return Engines::Puller::StateWordCount;
    }
    return 0;
}

const char* EngineClassName(EngineClass engine_class) {
    switch (engine_class) {
    case EngineClass::Maxwell3D:
        return "Maxwell3D";
    case EngineClass::KeplerCompute:
        return "KeplerCompute";
    case EngineClass::Fermi2D:
        return "Fermi2D";
    case EngineClass::MaxwellDma:
        return "MaxwellDMA";
    case EngineClass::KeplerMemory:
        return "KeplerMemory";
    case EngineClass::Host:
        return "host methods";
    }
    return "unknown";
}

// Switch 1 macro code is {method, count, words} runs; the runs must fill the section exactly.
bool CodeRunsWellFormed(const std::vector<u32>& words) {
    size_t at = 0;
    while (at < words.size()) {
        if (words.size() - at < 2) {
            return false;
        }
        const size_t count = words[at + 1];
        at += 2;
        if (count > words.size() - at) {
            return false;
        }
        at += count;
    }
    return true;
}

std::string Num(u64 value) {
    return std::to_string(value);
}

} // namespace

bool ValidateCheckpointForRestore(const GpuCheckpoint& checkpoint, std::string& error) {
    error.clear();
    if (checkpoint.channels.empty()) {
        return Refuse(error, "the checkpoint has no channel");
    }

    // Channels: the GPU hands out ids 1, 2, 3, ... in creation order and nothing else.
    std::map<u32, bool> turing_of;
    u32 next_id = 1;
    for (const ChannelEntry& channel : checkpoint.channels) {
        if (channel.channel_id != next_id) {
            return Refuse(error, "the channel order cannot be reproduced: found channel " +
                                     Num(channel.channel_id) + " where channel " + Num(next_id) +
                                     " is next (the GPU allocates channel ids sequentially from 1)");
        }
        ++next_id;
        turing_of[channel.channel_id] = (channel.flags & ChannelTuringMacros) != 0;
    }
    const bool turing = turing_of.begin()->second;
    for (const auto& [id, channel_turing] : turing_of) {
        if (channel_turing != turing) {
            return Refuse(error, "channels mix Turing and Switch 1 macros (channel " + Num(id) +
                                     "), which one process cannot do");
        }
    }
    const auto known_channel = [&](u32 id) { return turing_of.count(id) != 0; };

    // Address spaces.
    std::set<u32> spaces;
    for (const AddressSpaceEntry& space : checkpoint.address_spaces) {
        if (!spaces.insert(space.address_space_id).second) {
            return Refuse(error, "address space " + Num(space.address_space_id) + " is listed twice");
        }
        if (space.address_space_bits < 20 || space.address_space_bits > 56 ||
            space.big_page_bits < 12 || space.big_page_bits > 24 ||
            space.split_address > (u64{1} << space.address_space_bits) ||
            (space.syncpoint_shim != 0 && space.syncpoint_shim >= (u64{1} << space.address_space_bits))) {
            return Refuse(error, "address space " + Num(space.address_space_id) +
                                     " has a size, page size or split address no MemoryManager has");
        }
    }
    for (const ChannelEntry& channel : checkpoint.channels) {
        if (spaces.count(channel.address_space_id) == 0) {
            return Refuse(error, "channel " + Num(channel.channel_id) +
                                     " uses address space " + Num(channel.address_space_id) +
                                     ", which the checkpoint does not hold");
        }
    }

    // Engine registers: every channel has each class once, with exactly the size the engine has.
    std::set<std::pair<u32, u32>> engines;
    for (const EngineRegisters& engine : checkpoint.engines) {
        const u32 class_id = static_cast<u32>(engine.engine_class);
        if (!known_channel(engine.channel_id)) {
            return Refuse(error, "engine registers of unknown channel " + Num(engine.channel_id));
        }
        if (class_id < 1 || class_id > EngineClassCount) {
            return Refuse(error, "unknown engine class " + Num(class_id) + " on channel " +
                                     Num(engine.channel_id));
        }
        if (!engines.emplace(engine.channel_id, class_id).second) {
            return Refuse(error, std::string("duplicate ") + EngineClassName(engine.engine_class) +
                                     " registers on channel " + Num(engine.channel_id));
        }
        const size_t expected = ExpectedRegisterWords(engine.engine_class, turing);
        if (engine.registers.size() > expected) {
            return Refuse(error, std::string("oversized register block for ") +
                                     EngineClassName(engine.engine_class) + " on channel " +
                                     Num(engine.channel_id) + ": " + Num(engine.registers.size()) +
                                     " words, the engine has " + Num(expected));
        }
        if (engine.registers.size() != expected) {
            return Refuse(error, std::string("register block of ") +
                                     EngineClassName(engine.engine_class) + " on channel " +
                                     Num(engine.channel_id) + " has " + Num(engine.registers.size()) +
                                     " words, the engine has " + Num(expected));
        }
    }
    if (engines.size() != checkpoint.channels.size() * EngineClassCount) {
        return Refuse(error, "a channel lacks the registers of one of its six engine classes");
    }

    // Subchannel bindings, shadow RAM, DMA state: one each per channel.
    std::set<u32> bindings;
    for (const SubchannelBinding& binding : checkpoint.subchannels) {
        if (!known_channel(binding.channel_id) || !bindings.insert(binding.channel_id).second) {
            return Refuse(error, "subchannel bindings of an unknown or repeated channel " +
                                     Num(binding.channel_id));
        }
        if (binding.classes.size() > MaxSubchannels) {
            return Refuse(error, "channel " + Num(binding.channel_id) + " binds more than 8 subchannels");
        }
        for (const EngineClass engine_class : binding.classes) {
            const u32 class_id = static_cast<u32>(engine_class);
            if (class_id < 1 || class_id > 5) {
                return Refuse(error, "subchannel of channel " + Num(binding.channel_id) +
                                         " is bound to engine class " + Num(class_id) +
                                         ", which no subchannel can hold");
            }
        }
    }
    if (bindings.size() != checkpoint.channels.size()) {
        return Refuse(error, "a channel has no subchannel bindings");
    }
    std::set<u32> shadows;
    for (const ShadowRam& shadow : checkpoint.shadows) {
        if (!known_channel(shadow.channel_id) || !shadows.insert(shadow.channel_id).second) {
            return Refuse(error, "shadow RAM of an unknown or repeated channel " +
                                     Num(shadow.channel_id));
        }
        if (shadow.words.size() != Engines::Maxwell3D::Regs::NUM_REGS) {
            return Refuse(error, "shadow RAM of channel " + Num(shadow.channel_id) + " has " +
                                     Num(shadow.words.size()) + " words, the engine has " +
                                     Num(Engines::Maxwell3D::Regs::NUM_REGS));
        }
    }
    if (shadows.size() != checkpoint.channels.size()) {
        return Refuse(error, "a channel has no shadow RAM section");
    }
    std::set<u32> dmas;
    for (const DmaState& dma : checkpoint.dma) {
        if (!known_channel(dma.channel_id) || !dmas.insert(dma.channel_id).second) {
            return Refuse(error, "DMA state of an unknown or repeated channel " + Num(dma.channel_id));
        }
        if (dma.subchannel >= MaxSubchannels || dma.parked.size() > MaxParkedMethods ||
            dma.parked.size() > Tegra::DmaPusher::MaxParkedMethods) {
            return Refuse(error, "DMA state of channel " + Num(dma.channel_id) +
                                     " names a subchannel or holds parked methods beyond the pusher");
        }
        for (const ParkedMethod& parked : dma.parked) {
            if (parked.subchannel >= MaxSubchannels) {
                return Refuse(error, "a parked method of channel " + Num(dma.channel_id) +
                                         " names subchannel " + Num(parked.subchannel));
            }
        }
    }
    if (dmas.size() != checkpoint.channels.size()) {
        return Refuse(error, "a channel has no DMA state");
    }

    // Macro sections: which exist depends on the macro flavour, each exactly once, sized as the
    // engine's RAM is.
    std::map<std::pair<u32, u32>, const MacroSection*> macros;
    for (const MacroSection& macro : checkpoint.macros) {
        const u32 kind = static_cast<u32>(macro.kind);
        if (macro.channel_id != SharedChannel && !known_channel(macro.channel_id)) {
            return Refuse(error, "macro section of unknown channel " + Num(macro.channel_id));
        }
        if (kind < 1 || kind > 7) {
            return Refuse(error, "unknown macro section " + Num(kind));
        }
        if (!macros.emplace(std::make_pair(macro.channel_id, kind), &macro).second) {
            return Refuse(error, "macro section " + Num(kind) + " of channel " +
                                     Num(macro.channel_id) + " is listed twice");
        }
    }
    const auto section = [&](u32 channel, MacroSectionKind kind) -> const MacroSection* {
        const auto it = macros.find({channel, static_cast<u32>(kind)});
        return it == macros.end() ? nullptr : it->second;
    };
    const auto sized = [&](u32 channel, MacroSectionKind kind, size_t words, size_t at_most,
                           const char* name) -> bool {
        const MacroSection* found = section(channel, kind);
        if (found == nullptr) {
            return Refuse(error, std::string("macro section ") + name + " of channel " +
                                     (channel == SharedChannel ? std::string("(shared)") : Num(channel)) +
                                     " is missing");
        }
        if ((words != 0 && found->words.size() != words) ||
            (words == 0 && found->words.size() > at_most)) {
            return Refuse(error, std::string("macro section ") + name + " has " +
                                     Num(found->words.size()) + " words, expected " +
                                     (words != 0 ? Num(words) : "at most " + Num(at_most)));
        }
        return true;
    };
    size_t expected_sections = 0;
    if (turing) {
        if (!sized(SharedChannel, MacroSectionKind::Code, 0, NativeMacroRamWords, "Code") ||
            !sized(SharedChannel, MacroSectionKind::Positions, MacroPositionWords, 0, "Positions") ||
            !sized(SharedChannel, MacroSectionKind::ComputeCode, 0, NativeMacroRamWords, "ComputeCode") ||
            !sized(SharedChannel, MacroSectionKind::ComputePositions, MacroPositionWords, 0,
                   "ComputePositions") ||
            !sized(SharedChannel, MacroSectionKind::NativeState, 2, 0, "NativeState")) {
            return false;
        }
        expected_sections += 5;
        for (const ChannelEntry& channel : checkpoint.channels) {
            if (!sized(channel.channel_id, MacroSectionKind::DataRam, DataRamWords, 0, "DataRam") ||
                !sized(channel.channel_id, MacroSectionKind::NativeState, 2, 0, "NativeState")) {
                return false;
            }
            expected_sections += 2;
        }
    } else {
        for (const ChannelEntry& channel : checkpoint.channels) {
            if (!sized(channel.channel_id, MacroSectionKind::Code, 0, MaxMacroWords, "Code") ||
                !sized(channel.channel_id, MacroSectionKind::Positions, MacroPositionWords, 0,
                       "Positions")) {
                return false;
            }
            if (!CodeRunsWellFormed(section(channel.channel_id, MacroSectionKind::Code)->words)) {
                return Refuse(error, "macro code of channel " + Num(channel.channel_id) +
                                         " is not {method, count, words} runs");
            }
            expected_sections += 2;
            if (const MacroSection* cache = section(channel.channel_id, MacroSectionKind::Cache)) {
                if (cache->words.size() % 3 != 0) {
                    return Refuse(error, "macro cache of channel " + Num(channel.channel_id) +
                                             " is not {method, hash_lo, hash_hi} triples");
                }
                ++expected_sections;
            }
        }
    }
    if (macros.size() != expected_sections) {
        return Refuse(error, "a macro section does not belong to this macro flavour");
    }

    // Root tables, bound constant buffers, and the optional shared NativeQmdStates and
    // SyncpointCounters sections.
    std::set<std::pair<u32, u32>> states;
    bool has_qmd = false;
    const EngineStateSection* counters = nullptr;
    for (const EngineStateSection& state : checkpoint.states) {
        if (state.kind == EngineStateKind::NativeQmdStates) {
            if (state.channel_id != SharedChannel || has_qmd) {
                return Refuse(error, "shared NativeQmdStates section is misplaced or repeated");
            }
            if (const auto why = Tegra::NativeQmdScheduler::CheckSerializedStates(state.words);
                !why.empty()) {
                return Refuse(error, std::string{why});
            }
            has_qmd = true;
            continue;
        }
        if (state.kind == EngineStateKind::SyncpointCounters) {
            if (state.channel_id != SharedChannel || counters != nullptr ||
                state.words.size() != SyncpointCounterWords) {
                return Refuse(error, "shared SyncpointCounters section is malformed or repeated");
            }
            counters = &state;
            continue;
        }
        const u32 kind = static_cast<u32>(state.kind);
        if (!known_channel(state.channel_id) || kind < 1 || kind > 2 ||
            !states.emplace(state.channel_id, kind).second) {
            return Refuse(error, "engine state of an unknown, unnamed or repeated kind on channel " +
                                     Num(state.channel_id));
        }
        const size_t expected =
            state.kind == EngineStateKind::RootTables ? RootTableWords : ConstBufferWords;
        if (state.words.size() != expected) {
            return Refuse(error, "engine state " + Num(kind) + " of channel " +
                                     Num(state.channel_id) + " has " + Num(state.words.size()) +
                                     " words, expected " + Num(expected));
        }
    }
    if (states.size() != checkpoint.channels.size() * 2) {
        return Refuse(error, "a channel lacks its root tables or constant buffer bindings");
    }

    // Fences: the channel's own syncpoint, the only kind a channel holds.
    std::set<u32> fences;
    for (const CheckpointFence& fence : checkpoint.fences) {
        if (fence.kind != FenceKind::SyncpointRaised || !known_channel(fence.channel_id) ||
            !fences.insert(fence.channel_id).second || fence.id == 0 || fence.id >= MaxSyncpoints ||
            fence.value > 0xFFFFFFFFULL || fence.aux > 0xFFFFFFFFULL) {
            return Refuse(error, "a fence of channel " + Num(fence.channel_id) +
                                     " is not a syncpoint a channel can hold");
        }
        if (counters != nullptr && counters->words[size_t{fence.id} * 2] != fence.value) {
            return Refuse(error, "channel " + Num(fence.channel_id) +
                                     " fence disagrees with the syncpoint counters section");
        }
    }
    return true;
}

void SettleEngineSinks(Tegra::GPU& gpu, CutChannels channels) {
    for (const auto& owner : channels) {
        auto& channel = *owner;
        if (!channel.initialized || !channel.payload.has_value() || !channel.memory_manager ||
            channel.engine_memory != channel.memory_manager) {
            continue;
        }
        auto& payload = *channel.payload;
        if (payload.maxwell_3d.method_sink.empty() && payload.kepler_compute.method_sink.empty() &&
            payload.fermi_2d.method_sink.empty() && payload.maxwell_dma.method_sink.empty() &&
            payload.kepler_memory.method_sink.empty()) {
            continue;
        }
        // The writes go through the channel's engines with the channel bound, as the scheduler
        // binds it before it runs one of its command lists.
        gpu.BindChannel(channel);
        Core::System& system = payload.dma_pusher.system;
        payload.maxwell_3d.ConsumeSink(system);
        payload.kepler_compute.ConsumeSink(system);
        payload.fermi_2d.ConsumeSink(system);
        payload.maxwell_dma.ConsumeSink(system);
        payload.kepler_memory.ConsumeSink(system);
    }
}

bool CaptureGpuCheckpoint(CutChannels channels, GpuCheckpoint& out, std::string& error,
                          bool* retry_later) {
    out = GpuCheckpoint{};
    error.clear();
    if (retry_later != nullptr) {
        *retry_later = false;
    }
    std::string failure;
    // Set with a `failure` that a later quiescent point may not have: what the channel was in the
    // middle of may be finished by then. The rest (no address space, an unbound subchannel, a
    // section the format cannot hold) is the same at any point.
    bool transient = false;
    std::map<u32, AddressSpaceEntry> spaces;
    bool shared_macros_done = false;
    Tegra::Host1x::SyncpointManager* syncpoints = nullptr;

    std::ranges::for_each(channels, [&](const auto& owner) {
        auto& channel = *owner;
        if (!failure.empty() || !channel.initialized || !channel.payload.has_value()) {
            return;
        }
        if (!channel.memory_manager || channel.memory_manager != channel.engine_memory) {
            failure = ChannelName(channel) + " has no address space or was rebound after initialization";
            return;
        }
        auto& payload = *channel.payload;
        Tegra::DmaPusher& pusher = payload.dma_pusher;
        const u32 id = static_cast<u32>(channel.bind_id);

        // A clean cut: nothing queued, nothing suspended, nothing half way into an engine. The
        // caller holds the GPU thread between two command lists, so a list still queued can only be
        // one the channel has not finished: the one it is suspended on.
        if (pusher.HasPendingAcquire()) {
            failure = ChannelName(channel) + " is suspended on a semaphore acquire with " +
                      std::to_string(pusher.dma_pushbuffer.size()) + " pushbuffer list(s) queued behind it";
            transient = true;
            return;
        }
        if (!pusher.dma_pushbuffer.empty()) {
            failure = ChannelName(channel) + " has " + std::to_string(pusher.dma_pushbuffer.size()) +
                      " pushbuffer list(s) queued that are not waiting on a semaphore";
            transient = true;
            return;
        }
        if (!payload.maxwell_3d.method_sink.empty() || !payload.kepler_compute.method_sink.empty() ||
            !payload.fermi_2d.method_sink.empty() || !payload.maxwell_dma.method_sink.empty() ||
            !payload.kepler_memory.method_sink.empty()) {
            failure = ChannelName(channel) + " has methods waiting in an engine";
            return;
        }
        if (pusher.parked_methods.size() > Tegra::DmaPusher::MaxParkedMethods ||
            pusher.parked_methods.size() > MaxParkedMethods) {
            failure = ChannelName(channel) + " has more parked methods than the format holds";
            return;
        }
        // Engine state that only exists in the middle of an operation cannot be written down.
        const char* blocker = payload.maxwell_3d.CleanCutBlocker();
        const char* engine_name = "Maxwell3D";
        if (blocker == nullptr) {
            blocker = payload.kepler_compute.CleanCutBlocker();
            engine_name = "compute";
        }
        if (blocker == nullptr && payload.kepler_memory.HasPendingUpload()) {
            blocker = "an inline upload is unfinished";
            engine_name = "KeplerMemory";
        }
        if (blocker != nullptr) {
            failure = ChannelName(channel) + ": " + blocker + " (" + engine_name + ")";
            transient = true;
            return;
        }

        const u32 space_id = static_cast<u32>(channel.memory_manager->GetID());
        const bool native_tiling = channel.memory_manager->UsesNativeTiling();
        const bool turing = payload.maxwell_3d.GetNativeMacroEngine().native_turing;
        ChannelEntry entry;
        entry.channel_id = id;
        entry.address_space_id = space_id;
        entry.flags = (native_tiling ? ChannelNativeTiling : 0U) | (turing ? ChannelTuringMacros : 0U);
        out.channels.push_back(entry);

        AddressSpaceEntry space;
        space.address_space_id = space_id;
        space.address_space_bits = static_cast<u32>(channel.memory_manager->GetAddressSpaceBits());
        space.big_page_bits = static_cast<u32>(channel.memory_manager->GetBigPageBits());
        space.split_address = static_cast<u64>(channel.memory_manager->GetSplitAddress());
        space.syncpoint_shim = static_cast<u64>(channel.memory_manager->GetNativeSyncpointShim());
        if (space.syncpoint_shim != 0) {
            space.flags |= AddressSpaceHasSyncpointShim;
        }
        spaces.emplace(space_id, space);

        AddEngine(out, id, EngineClass::Maxwell3D, payload.maxwell_3d.regs.reg_array);
        if (turing) {
            // The native register file is the truth; `regs` is a copy that lags it, so both are
            // kept (native words first).
            std::vector<u32> both(payload.kepler_compute.NativeRegisterWords().begin(),
                                  payload.kepler_compute.NativeRegisterWords().end());
            both.insert(both.end(), payload.kepler_compute.regs.reg_array.begin(),
                        payload.kepler_compute.regs.reg_array.end());
            AddEngine(out, id, EngineClass::KeplerCompute, both);
        } else {
            AddEngine(out, id, EngineClass::KeplerCompute, payload.kepler_compute.regs.reg_array);
        }
        AddEngine(out, id, EngineClass::Fermi2D, payload.fermi_2d.regs.reg_array);
        AddEngine(out, id, EngineClass::MaxwellDma, payload.maxwell_dma.RegisterWords());
        AddEngine(out, id, EngineClass::KeplerMemory, payload.kepler_memory.regs.reg_array);
        AddEngine(out, id, EngineClass::Host, pusher.puller.StateWords());

        SubchannelBinding binding;
        binding.channel_id = id;
        size_t bound = 0;
        for (size_t i = 0; i < pusher.subchannels.size(); ++i) {
            if (pusher.subchannels[i] != nullptr) {
                bound = i + 1;
            }
        }
        for (size_t i = 0; i < bound; ++i) {
            EngineClass engine_class{};
            if (pusher.subchannels[i] == nullptr ||
                !ClassOf(pusher.subchannel_type[i], engine_class)) {
                failure = ChannelName(channel) + " has an unbound or unsupported subchannel " +
                          std::to_string(i);
                return;
            }
            binding.classes.push_back(engine_class);
        }
        out.subchannels.push_back(std::move(binding));

        if (!AddMacros(out, id, payload.maxwell_3d, payload.kepler_compute, turing,
                       shared_macros_done, failure)) {
            return;
        }

        ShadowRam shadow;
        shadow.channel_id = id;
        shadow.mode = static_cast<u32>(payload.maxwell_3d.regs.shadow_ram_control);
        shadow.words.assign(payload.maxwell_3d.shadow_state.reg_array.begin(),
                            payload.maxwell_3d.shadow_state.reg_array.end());
        out.shadows.push_back(std::move(shadow));
        AddEngineState(out, id, payload.maxwell_3d);

        DmaState dma;
        dma.channel_id = id;
        dma.method = pusher.dma_state.method;
        dma.subchannel = pusher.dma_state.subchannel;
        dma.method_count = pusher.dma_state.method_count;
        dma.length_pending = pusher.dma_state.length_pending;
        dma.flags = (pusher.dma_state.non_incrementing ? DmaNonIncrementing : 0U) |
                    (pusher.dma_state.is_last_call ? DmaLastCall : 0U) |
                    (pusher.dma_increment_once ? DmaIncrementOnce : 0U) |
                    (pusher.signal_sync ? DmaSignalSync : 0U);
        dma.dma_get = pusher.dma_state.dma_get;
        dma.dma_word_offset = pusher.dma_state.dma_word_offset;
        dma.split_method_end = pusher.split_method_end;
        for (const auto& parked : pusher.parked_methods) {
            ParkedMethod method;
            method.method = parked.method;
            method.subchannel = parked.subchannel;
            method.method_count = parked.method_count;
            method.flags = (parked.non_incrementing ? DmaNonIncrementing : 0U) |
                           (parked.increment_once ? DmaIncrementOnce : 0U);
            method.resume_address = parked.resume_address;
            dma.parked.push_back(method);
        }
        out.dma.push_back(std::move(dma));

        if (channel.nx2_syncpoint != 0) {
            // Only a channel with a syncpoint has a Host1x behind it (a bare test system has none).
            syncpoints = &pusher.system.Host1x().GetSyncpointManager();
            CheckpointFence fence;
            fence.kind = FenceKind::SyncpointRaised;
            fence.channel_id = id;
            fence.id = channel.nx2_syncpoint;
            fence.value = syncpoints->GetGuestSyncpointValue(channel.nx2_syncpoint);
            fence.aux = channel.nx2_syncpoint_max.load(std::memory_order_relaxed);
            out.fences.push_back(fence);
        }
    });

    if (!failure.empty()) {
        out = GpuCheckpoint{};
        if (retry_later != nullptr) {
            *retry_later = transient;
        }
        return Refuse(error, std::move(failure));
    }
    for (const auto& [id, space] : spaces) {
        out.address_spaces.push_back(space);
    }
    if (out.channels.empty()) {
        return Refuse(error, "no initialised channel to checkpoint");
    }
    if ((out.channels.front().flags & ChannelTuringMacros) != 0) {
        auto qmd_words = Tegra::NativeQmdScheduler::ExportStates();
        if (!qmd_words.empty()) {
            out.states.push_back(EngineStateSection{
                SharedChannel, EngineStateKind::NativeQmdStates, std::move(qmd_words)});
        }
    }
    if (syncpoints != nullptr) {
        // Every counter, not only a channel's own: a client the shim serves reads any of them.
        // The guest values are taken with increments held off, and the channel fences are made
        // to agree with that one snapshot.
        std::vector<u32> counters(SyncpointCounterWords);
        syncpoints->WithGuestValuesStable([&] {
            for (u32 id = 0; id < SyncpointCounterCount; ++id) {
                counters[size_t{id} * 2] = syncpoints->GetGuestSyncpointValue(id);
                counters[size_t{id} * 2 + 1] = syncpoints->GetHostSyncpointValue(id);
            }
        });
        if (std::ranges::any_of(counters, [](u32 word) { return word != 0; })) {
            for (CheckpointFence& fence : out.fences) {
                if (fence.id < SyncpointCounterCount) {
                    fence.value = counters[size_t{fence.id} * 2];
                }
            }
            out.states.push_back(EngineStateSection{
                SharedChannel, EngineStateKind::SyncpointCounters, std::move(counters)});
        }
    }
    // What was captured has to be restorable: sequential channel ids, sizes the engines have.
    std::string unrestorable;
    if (!ValidateCheckpointForRestore(out, unrestorable)) {
        out = GpuCheckpoint{};
        return Refuse(error, "the captured state cannot be restored: " + unrestorable);
    }
    return true;
}

namespace {

struct RestoredObjects {
    std::map<u32, std::shared_ptr<Tegra::MemoryManager>> spaces;
    std::map<u32, std::shared_ptr<Tegra::Control::ChannelState>> channels;
};

// Published after a complete restore. ReleaseRestoredGpuState drops these replay lookup handles.
RestoredObjects& Registry() {
    static RestoredObjects objects;
    return objects;
}

template <typename Block>
bool CopyBlock(const std::vector<u32>& words, Block& to) {
    if (words.size() != to.size()) {
        return false;
    }
    std::copy(words.begin(), words.end(), to.begin());
    return true;
}

const EngineRegisters* FindEngine(const GpuCheckpoint& cp, u32 channel, EngineClass engine_class) {
    for (const EngineRegisters& engine : cp.engines) {
        if (engine.channel_id == channel && engine.engine_class == engine_class) {
            return &engine;
        }
    }
    return nullptr;
}

const MacroSection* FindMacro(const GpuCheckpoint& cp, u32 channel, MacroSectionKind kind) {
    for (const MacroSection& macro : cp.macros) {
        if (macro.channel_id == channel && macro.kind == kind) {
            return &macro;
        }
    }
    return nullptr;
}

const EngineStateSection* FindState(const GpuCheckpoint& cp, u32 channel, EngineStateKind kind) {
    for (const EngineStateSection& state : cp.states) {
        if (state.channel_id == channel && state.kind == kind) {
            return &state;
        }
    }
    return nullptr;
}

template <typename Item>
const Item* FindByChannel(const std::vector<Item>& items, u32 channel) {
    for (const Item& item : items) {
        if (item.channel_id == channel) {
            return &item;
        }
    }
    return nullptr;
}

bool Missing(std::string& error, u32 channel, const char* what) {
    return Refuse(error, "channel " + Num(channel) + " lacks " + what +
                             " (ValidateCheckpointForRestore should have refused it)");
}

// The macro RAM, positions and data RAM of one channel; the Switch 2 context once for all.
bool RestoreMacros(const GpuCheckpoint& cp, u32 id, Engines::Maxwell3D& maxwell, bool turing,
                   bool& shared_done, std::string& error) {
    if (!turing) {
        const MacroSection* code = FindMacro(cp, id, MacroSectionKind::Code);
        const MacroSection* where = FindMacro(cp, id, MacroSectionKind::Positions);
        if (code == nullptr || where == nullptr) {
            return Missing(error, id, "macro code or positions");
        }
        Tegra::MacroEngine& engine = maxwell.GetNativeMacroEngine();
        engine.uploaded_macro_code.clear();
        engine.macro_cache.clear();
        const std::vector<u32>& words = code->words;
        for (size_t at = 0; at < words.size();) {
            const u32 method = words[at];
            const size_t count = words[at + 1];
            at += 2;
            engine.uploaded_macro_code[method].assign(words.data() + at, words.data() + at + count);
            at += count;
        }
        std::span<u32> positions = maxwell.GetNativeMacroPositions();
        if (!CopyBlock(where->words, positions)) {
            return Refuse(error, "macro positions of channel " + Num(id) + " have the wrong size");
        }
        // The compiled-macro cache is rebuilt on the first call of each macro; what was recorded
        // is proof that the uploaded code is the code that was compiled.
        if (const MacroSection* cache = FindMacro(cp, id, MacroSectionKind::Cache)) {
            for (size_t i = 0; i + 2 < cache->words.size(); i += 3) {
                const auto found = engine.uploaded_macro_code.find(cache->words[i]);
                const u64 hash = u64{cache->words[i + 1]} | (u64{cache->words[i + 2]} << 32);
                if (found == engine.uploaded_macro_code.end() ||
                    hash != static_cast<u64>(Common::HashRange(found->second.begin(),
                                                               found->second.end()))) {
                    return Refuse(error, "the compiled macro " + Num(cache->words[i]) +
                                             " of channel " + Num(id) +
                                             " does not match the restored code");
                }
            }
        }
        return true;
    }

    Tegra::NativeMacroContext* context = maxwell.GetNativeMacroContextPtr();
    if (context == nullptr) {
        return Refuse(error, "channel " + Num(id) + " has no Switch 2 macro context");
    }
    if (!shared_done) {
        shared_done = true;
        const MacroSection* code = FindMacro(cp, SharedChannel, MacroSectionKind::Code);
        const MacroSection* where = FindMacro(cp, SharedChannel, MacroSectionKind::Positions);
        const MacroSection* compute_code = FindMacro(cp, SharedChannel, MacroSectionKind::ComputeCode);
        const MacroSection* compute_where =
            FindMacro(cp, SharedChannel, MacroSectionKind::ComputePositions);
        const MacroSection* state = FindMacro(cp, SharedChannel, MacroSectionKind::NativeState);
        if (code == nullptr || where == nullptr || compute_code == nullptr ||
            compute_where == nullptr || state == nullptr) {
            return Missing(error, id, "the shared macro sections");
        }
        const auto load = [](Tegra::MacroEngine& engine, const std::vector<u32>& words, u32 offset) {
            if (words.size() > engine.native_code.size() || offset > engine.native_code.size()) {
                return false;
            }
            engine.native_code.fill(0);
            std::copy(words.begin(), words.end(), engine.native_code.begin());
            engine.native_write_offset = offset;
            ++engine.native_code_generation; // the replay memo is keyed on it
            return true;
        };
        if (!load(context->engine, code->words, state->words[0]) ||
            !load(context->compute_engine, compute_code->words, state->words[1]) ||
            !CopyBlock(where->words, context->positions) ||
            !CopyBlock(compute_where->words, context->compute_positions)) {
            return Refuse(error, "the shared macro RAM does not fit the macro engine");
        }
    }
    const MacroSection* data_ram = FindMacro(cp, id, MacroSectionKind::DataRam);
    if (data_ram == nullptr) {
        return Missing(error, id, "the MME data RAM");
    }
    if (!CopyBlock(data_ram->words, maxwell.GetNativeDataRam())) {
        return Refuse(error, "the MME data RAM of channel " + Num(id) + " has the wrong size");
    }
    return true;
}

// Writes everything one channel holds. No method runs.
bool RestoreChannel(const GpuCheckpoint& cp, Tegra::Control::ChannelState& channel, bool turing,
                    bool& shared_macros_done, std::string& error) {
    const u32 id = static_cast<u32>(channel.bind_id);
    auto& payload = *channel.payload;
    const char* const bad_size = " has a register block of the wrong size";

    // Engine registers.
    const EngineRegisters* maxwell = FindEngine(cp, id, EngineClass::Maxwell3D);
    const EngineRegisters* compute = FindEngine(cp, id, EngineClass::KeplerCompute);
    const EngineRegisters* fermi = FindEngine(cp, id, EngineClass::Fermi2D);
    const EngineRegisters* dma_engine = FindEngine(cp, id, EngineClass::MaxwellDma);
    const EngineRegisters* memory = FindEngine(cp, id, EngineClass::KeplerMemory);
    const EngineRegisters* host = FindEngine(cp, id, EngineClass::Host);
    const ShadowRam* shadow = FindByChannel(cp.shadows, id);
    const SubchannelBinding* binding = FindByChannel(cp.subchannels, id);
    const DmaState* dma = FindByChannel(cp.dma, id);
    const EngineStateSection* roots = FindState(cp, id, EngineStateKind::RootTables);
    const EngineStateSection* buffers = FindState(cp, id, EngineStateKind::ConstBufferBindings);
    if (maxwell == nullptr || compute == nullptr || fermi == nullptr || dma_engine == nullptr ||
        memory == nullptr || host == nullptr || shadow == nullptr || binding == nullptr ||
        dma == nullptr || roots == nullptr || buffers == nullptr) {
        return Missing(error, id, "a section");
    }
    if (!CopyBlock(maxwell->registers, payload.maxwell_3d.regs.reg_array) ||
        !CopyBlock(shadow->words, payload.maxwell_3d.shadow_state.reg_array)) {
        return Refuse(error, "Maxwell3D of channel " + Num(id) + bad_size);
    }
    if (!CopyBlock(fermi->registers, payload.fermi_2d.regs.reg_array)) {
        return Refuse(error, "Fermi2D of channel " + Num(id) + bad_size);
    }
    if (!CopyBlock(memory->registers, payload.kepler_memory.regs.reg_array)) {
        return Refuse(error, "KeplerMemory of channel " + Num(id) + bad_size);
    }
    if (!payload.maxwell_dma.RestoreRegisterWords(dma_engine->registers)) {
        return Refuse(error, "MaxwellDMA of channel " + Num(id) + bad_size);
    }
    if (!payload.dma_pusher.puller.RestoreStateWords(host->registers)) {
        return Refuse(error, "the host methods of channel " + Num(id) +
                                 " have a wrong size or name an engine this build lacks");
    }

    // Macros, and the compute engine that shares their RAM on Switch 2.
    if (!RestoreMacros(cp, id, payload.maxwell_3d, turing, shared_macros_done, error)) {
        return false;
    }
    if (turing) {
        const MacroSection* state = FindMacro(cp, id, MacroSectionKind::NativeState);
        if (state == nullptr) {
            return Missing(error, id, "the compute native state");
        }
        const std::span<const u32> both{compute->registers};
        const size_t native_words = both.size() - Engines::KeplerCompute::Regs::NUM_REGS;
        if (!payload.kepler_compute.RestoreNativeState(both.first(native_words),
                                                       both.subspan(native_words), state->words[0],
                                                       state->words[1] != 0)) {
            return Refuse(error, "KeplerCompute of channel " + Num(id) + bad_size);
        }
    } else if (!CopyBlock(compute->registers, payload.kepler_compute.regs.reg_array)) {
        return Refuse(error, "KeplerCompute of channel " + Num(id) + bad_size);
    }

    // Root tables and bound constant buffers.
    auto& tables = payload.maxwell_3d.root_tables;
    for (size_t table = 0; table < tables.tables.size(); ++table) {
        std::copy_n(roots->words.begin() + static_cast<std::ptrdiff_t>(table * 64), 64,
                    tables.tables[table].begin());
    }
    tables.selected = roots->words[512];
    tables.offset = roots->words[513];
    tables.loaded = roots->words[514] != 0;
    size_t at = 0;
    for (auto& stage : payload.maxwell_3d.state.shader_stages) {
        for (auto& slot : stage.const_buffers) {
            slot.address = u64{buffers->words[at]} | (u64{buffers->words[at + 1]} << 32);
            slot.size = buffers->words[at + 2];
            slot.enabled = buffers->words[at + 3] != 0;
            at += 4;
        }
    }

    // Subchannel bindings.
    auto& pusher = payload.dma_pusher;
    for (size_t i = 0; i < binding->classes.size(); ++i) {
        const u32 sub = static_cast<u32>(i);
        switch (binding->classes[i]) {
        case EngineClass::Maxwell3D:
            pusher.BindSubchannel(&payload.maxwell_3d, sub, Engines::EngineTypes::Maxwell3D);
            break;
        case EngineClass::KeplerCompute:
            pusher.BindSubchannel(&payload.kepler_compute, sub, Engines::EngineTypes::KeplerCompute);
            break;
        case EngineClass::Fermi2D:
            pusher.BindSubchannel(&payload.fermi_2d, sub, Engines::EngineTypes::Fermi2D);
            break;
        case EngineClass::MaxwellDma:
            pusher.BindSubchannel(&payload.maxwell_dma, sub, Engines::EngineTypes::MaxwellDMA);
            break;
        case EngineClass::KeplerMemory:
            pusher.BindSubchannel(&payload.kepler_memory, sub, Engines::EngineTypes::KeplerMemory);
            break;
        case EngineClass::Host:
            return Refuse(error, "subchannel " + Num(i) + " of channel " + Num(id) +
                                     " is bound to the host class");
        }
    }

    // The DMA pusher between two lists.
    pusher.dma_state.method = dma->method;
    pusher.dma_state.subchannel = dma->subchannel;
    pusher.dma_state.method_count = dma->method_count;
    pusher.dma_state.length_pending = dma->length_pending;
    pusher.dma_state.dma_get = dma->dma_get;
    pusher.dma_state.dma_word_offset = dma->dma_word_offset;
    pusher.dma_state.non_incrementing = (dma->flags & DmaNonIncrementing) != 0;
    pusher.dma_state.is_last_call = (dma->flags & DmaLastCall) != 0;
    pusher.dma_increment_once = (dma->flags & DmaIncrementOnce) != 0;
    pusher.signal_sync = (dma->flags & DmaSignalSync) != 0;
    pusher.split_method_end = dma->split_method_end;
    pusher.parked_methods.clear();
    for (const ParkedMethod& parked : dma->parked) {
        Tegra::DmaPusher::ParkedMethod method{};
        method.method = parked.method;
        method.subchannel = parked.subchannel;
        method.method_count = parked.method_count;
        method.non_incrementing = (parked.flags & DmaNonIncrementing) != 0;
        method.increment_once = (parked.flags & DmaIncrementOnce) != 0;
        method.resume_address = parked.resume_address;
        pusher.parked_methods.push_back(method);
    }

    // The channel's own syncpoint (the Host1x value itself is the session's to raise).
    if (const CheckpointFence* fence = FindByChannel(cp.fences, id)) {
        channel.nx2_syncpoint = static_cast<u32>(fence->id);
        channel.nx2_syncpoint_max.store(static_cast<u32>(fence->aux), std::memory_order_relaxed);
    }

    // Every cache the engine derives from its registers starts dirty, as after a fresh start.
    payload.maxwell_3d.dirty.flags.set();
    return true;
}

} // namespace

bool RestoreGpuCheckpoint(Tegra::GPU& gpu, const GpuCheckpoint& checkpoint, std::string& error) {
    error.clear();
    if (!ValidateCheckpointForRestore(checkpoint, error)) {
        return false;
    }
    RestoredObjects& registry = Registry();
    if (!gpu.SnapshotChannels().empty() || !registry.channels.empty()) {
        return Refuse(error, "the GPU already has a channel: a checkpoint restores into an empty GPU");
    }
    const bool turing = (checkpoint.channels.front().flags & ChannelTuringMacros) != 0;
    if (turing != Tegra::g_nx2_ampere.load()) {
        return Refuse(error, "the checkpoint was taken in " +
                                 std::string(turing ? "Switch 2 (Ampere)" : "Switch 1") +
                                 " mode but this process is not: set the mode before restoring");
    }

    RestoredObjects staged; // Published only on success; channels own their address spaces.
    Core::System& system = gpu.Host1x().System();
    for (const AddressSpaceEntry& space : checkpoint.address_spaces) {
        auto manager = std::make_shared<Tegra::MemoryManager>(
            system, space.address_space_bits, space.split_address, space.big_page_bits, 12);
        gpu.InitAddressSpace(*manager);
        if (space.syncpoint_shim != 0) {
            manager->SetNativeSyncpointShim(space.syncpoint_shim);
        }
        staged.spaces.emplace(space.address_space_id, std::move(manager));
    }
    for (const ChannelEntry& entry : checkpoint.channels) {
        std::shared_ptr<Tegra::Control::ChannelState> channel = gpu.AllocateChannel();
        if (channel->bind_id != static_cast<s32>(entry.channel_id)) {
            return Refuse(error, "the GPU allocated channel " + Num(static_cast<u64>(channel->bind_id)) +
                                     " where the checkpoint has channel " + Num(entry.channel_id));
        }
        gpu.BindAddressSpace(*channel, staged.spaces.at(entry.address_space_id));
        staged.channels.emplace(entry.channel_id, channel);
        gpu.InitChannel(*channel, 0);
        const bool channel_turing = channel->payload->maxwell_3d.GetNativeMacroEngine().native_turing;
        const bool native_tiling = channel->memory_manager->UsesNativeTiling();
        if (channel_turing != ((entry.flags & ChannelTuringMacros) != 0) ||
            native_tiling != ((entry.flags & ChannelNativeTiling) != 0)) {
            return Refuse(error, "channel " + Num(entry.channel_id) +
                                     " comes up with other macro or tiling flags than the "
                                     "checkpoint recorded: the build or settings differ");
        }
    }

    Tegra::NativeQmdScheduler::ClearStates();
    if (const EngineStateSection* qmd =
            FindState(checkpoint, SharedChannel, EngineStateKind::NativeQmdStates)) {
        if (!Tegra::NativeQmdScheduler::ImportStates(qmd->words)) {
            return Refuse(error, "the checkpoint's QMD table cannot be imported");
        }
    }
    // Before the first command is fed: the session takes its syncpoint baseline from these, and
    // a client reads them through the syncpoint shim.
    if (const EngineStateSection* counters =
            FindState(checkpoint, SharedChannel, EngineStateKind::SyncpointCounters)) {
        auto& syncpoints = gpu.Host1x().GetSyncpointManager();
        for (u32 id = 0; id < SyncpointCounterCount; ++id) {
            if (!syncpoints.RestoreValues(id, counters->words[size_t{id} * 2],
                                          counters->words[size_t{id} * 2 + 1])) {
                return Refuse(error, "syncpoint " + Num(id) +
                                         " cannot be restored: an action already waits on it");
            }
        }
    }

    bool shared_macros_done = false;
    for (const auto& [id, channel] : staged.channels) {
        if (!RestoreChannel(checkpoint, *channel, turing, shared_macros_done, error)) {
            return false;
        }
    }
    registry = std::move(staged);
    return true;
}

Tegra::Control::ChannelState* RestoredChannel(Core::System& system, u32 channel_id) {
    static_cast<void>(system);
    const RestoredObjects& registry = Registry();
    const auto found = registry.channels.find(channel_id);
    return found == registry.channels.end() ? nullptr : found->second.get();
}

std::shared_ptr<Tegra::MemoryManager> RestoredAddressSpace(Core::System& system, u32 address_space_id) {
    static_cast<void>(system);
    const RestoredObjects& registry = Registry();
    const auto found = registry.spaces.find(address_space_id);
    return found == registry.spaces.end() ? nullptr : found->second;
}

void ReleaseRestoredGpuState() {
    Registry() = RestoredObjects{};
}

} // namespace VideoCore::Replay
