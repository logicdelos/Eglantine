// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The GPU checkpoint without a GPU: a checkpoint that has every section round-trips through the
// codec and passes the restore preconditions, and each refusal path of
// ValidateCheckpointForRestore (which RestoreGpuCheckpoint runs first) is taken by a checkpoint
// that cannot be restored. What needs a real Tegra::GPU (CaptureGpuCheckpoint of live engines,
// the writes of RestoreGpuCheckpoint) is not covered here.

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/puller.h"
#include "video_core/native_qmd.h"
#include "video_core/replay/checkpoint.h"
#include "video_core/replay/events.h"
#include "video_core/replay/format.h"

namespace R = VideoCore::Replay;

namespace {

using Bytes = std::vector<u8>;

// Sizes of the register blocks the engines of this build have. A change in an engine shows up as
// a failure here and in ValidateCheckpointForRestore, which is the point of pinning them.
constexpr size_t Maxwell3DWords = 0xE00;
constexpr size_t ComputeWords = 0xCF8;
constexpr size_t NativeComputeWords = 0x1000;
constexpr size_t Fermi2DWords = 0x258;
constexpr size_t MaxwellDmaWords = 0x800;
constexpr size_t KeplerMemoryWords = 0x7F;

// What a live puller really serializes, not the constant the validator is told to expect.
size_t HostWords() {
    const Tegra::Engines::Puller puller{};
    return puller.StateWords().size();
}
constexpr size_t RootTableWords = 8 * 64 + 3;
constexpr size_t ConstBufferWords = 5 * 18 * 4;

std::vector<u32> Words(size_t count, u32 seed) {
    std::vector<u32> words(count);
    for (size_t i = 0; i < count; ++i) {
        words[i] = seed * 2654435761U + static_cast<u32>(i) * 40503U;
    }
    return words;
}

size_t ExpectedWords(R::EngineClass engine_class, bool turing) {
    switch (engine_class) {
    case R::EngineClass::Maxwell3D:
        return Maxwell3DWords;
    case R::EngineClass::KeplerCompute:
        return turing ? NativeComputeWords + ComputeWords : ComputeWords;
    case R::EngineClass::Fermi2D:
        return Fermi2DWords;
    case R::EngineClass::MaxwellDma:
        return MaxwellDmaWords;
    case R::EngineClass::KeplerMemory:
        return KeplerMemoryWords;
    case R::EngineClass::Host:
        return HostWords();
    }
    return 0;
}

// A checkpoint that holds every section of every channel, sized as the engines are.
R::GpuCheckpoint Restorable(bool turing, u32 channels = 2) {
    R::GpuCheckpoint cp;
    cp.address_spaces.push_back({7, 40, 16, 0, u64{1} << 34});
    const u32 flags = turing ? (R::ChannelTuringMacros | R::ChannelNativeTiling) : 0U;
    if (turing) {
        cp.macros.push_back({R::SharedChannel, R::MacroSectionKind::Code, Words(40, 1)});
        cp.macros.push_back({R::SharedChannel, R::MacroSectionKind::Positions, Words(0x80, 2)});
        cp.macros.push_back({R::SharedChannel, R::MacroSectionKind::ComputeCode, Words(9, 3)});
        cp.macros.push_back(
            {R::SharedChannel, R::MacroSectionKind::ComputePositions, Words(0x80, 4)});
        cp.macros.push_back({R::SharedChannel, R::MacroSectionKind::NativeState, {120, 27}});
    }
    for (u32 id = 1; id <= channels; ++id) {
        cp.channels.push_back({id, 7, flags});
        const R::EngineClass classes[] = {R::EngineClass::Maxwell3D, R::EngineClass::KeplerCompute,
                                          R::EngineClass::Fermi2D,   R::EngineClass::MaxwellDma,
                                          R::EngineClass::KeplerMemory, R::EngineClass::Host};
        for (const R::EngineClass engine_class : classes) {
            R::EngineRegisters engine;
            engine.channel_id = id;
            engine.engine_class = engine_class;
            engine.registers = Words(ExpectedWords(engine_class, turing),
                                     id * 16 + static_cast<u32>(engine_class));
            cp.engines.push_back(std::move(engine));
        }
        cp.subchannels.push_back({id,
                                  {R::EngineClass::Maxwell3D, R::EngineClass::KeplerCompute,
                                   R::EngineClass::KeplerMemory, R::EngineClass::Fermi2D,
                                   R::EngineClass::MaxwellDma}});
        if (turing) {
            cp.macros.push_back({id, R::MacroSectionKind::DataRam, Words(0xc00, id + 50)});
            cp.macros.push_back({id, R::MacroSectionKind::NativeState, {3, 1}});
        } else {
            // One macro at method 5 with two words; its compiled-cache key.
            cp.macros.push_back({id, R::MacroSectionKind::Code, {5, 2, 0xA, 0xB}});
            cp.macros.push_back({id, R::MacroSectionKind::Positions, Words(0x80, id)});
            cp.macros.push_back({id, R::MacroSectionKind::Cache, {5, 0x1111, 0x2222}});
        }
        cp.shadows.push_back({id, 1, Words(Maxwell3DWords, id + 90)});
        R::DmaState dma;
        dma.channel_id = id;
        dma.method = 0x40;
        dma.method_count = 3;
        dma.flags = R::DmaNonIncrementing;
        dma.dma_get = 0x1000 * id;
        dma.split_method_end = 0x2000 * id;
        dma.parked.push_back({0x60, 1, 4, R::DmaIncrementOnce, 0x3000});
        cp.dma.push_back(std::move(dma));
        cp.states.push_back({id, R::EngineStateKind::RootTables, Words(RootTableWords, id + 70)});
        cp.states.push_back(
            {id, R::EngineStateKind::ConstBufferBindings, Words(ConstBufferWords, id + 80)});
        cp.fences.push_back({R::FenceKind::SyncpointRaised, id, 10 + id, 3, 4});
    }
    return cp;
}

Bytes Enc(const R::GpuCheckpoint& cp) {
    Bytes out;
    R::Encode(cp, out);
    return out;
}

// Whether restore is refused, and with a message that names `what`.
bool RefusedWith(const R::GpuCheckpoint& cp, const char* what) {
    std::string error;
    return !R::ValidateCheckpointForRestore(cp, error) &&
           error.find(what) != std::string::npos;
}

// Grows the body of the first sub-record by four bytes and says so in its length: a section whose
// size is not the size its own counts describe.
Bytes GrowFirstSubRecord(Bytes bytes) {
    constexpr size_t LengthAt = 4 + 8; // after the sub-record count, the kind and the version
    constexpr size_t BodyAt = 4 + 16;
    u64 length = 0;
    for (size_t i = 0; i < 8; ++i) {
        length |= u64{bytes[LengthAt + i]} << (8 * i);
    }
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(BodyAt + length), 4, u8{0});
    length += 4;
    for (size_t i = 0; i < 8; ++i) {
        bytes[LengthAt + i] = static_cast<u8>(length >> (8 * i));
    }
    return bytes;
}

} // namespace

TEST_CASE("a checkpoint with every section round-trips and can be restored",
          "[video_core][replay][checkpoint]") {
    for (const bool turing : {false, true}) {
        INFO((turing ? "Switch 2 (Turing macros)" : "Switch 1"));
        const R::GpuCheckpoint cp = Restorable(turing);
        const Bytes bytes = Enc(cp);
        R::GpuCheckpoint back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(Enc(back) == bytes);
        REQUIRE(back.channels.size() == 2);
        REQUIRE(back.address_spaces.size() == 1);
        REQUIRE(back.address_spaces[0].split_address == (u64{1} << 34));
        REQUIRE(back.engines.size() == 12);
        for (size_t i = 0; i < back.engines.size(); ++i) {
            REQUIRE(back.engines[i].channel_id == cp.engines[i].channel_id);
            REQUIRE(back.engines[i].engine_class == cp.engines[i].engine_class);
            REQUIRE(back.engines[i].registers == cp.engines[i].registers);
        }
        REQUIRE(back.subchannels.size() == 2);
        REQUIRE(back.macros.size() == (turing ? 5U + 2U * 2U : 3U * 2U));
        REQUIRE(back.shadows.size() == 2);
        REQUIRE(back.shadows[1].words == cp.shadows[1].words);
        REQUIRE(back.dma.size() == 2);
        REQUIRE(back.dma[1].split_method_end == 0x4000);
        REQUIRE(back.dma[1].parked.size() == 1);
        REQUIRE(back.states.size() == 4);
        REQUIRE(back.states[0].words == cp.states[0].words);
        REQUIRE(back.fences.size() == 2);

        std::string error;
        REQUIRE(R::ValidateCheckpointForRestore(cp, error));
        REQUIRE(R::ValidateCheckpointForRestore(back, error));
        REQUIRE(error.empty());
    }
}

TEST_CASE("a channel order that cannot be reproduced is refused",
          "[video_core][replay][checkpoint]") {
    const char* what = "channel order cannot be reproduced";
    R::GpuCheckpoint cp = Restorable(false);
    cp.channels[1].channel_id = 3; // 1, 3: channel 2 was released, ids are never reused
    REQUIRE(RefusedWith(cp, what));
    cp = Restorable(false);
    std::swap(cp.channels[0], cp.channels[1]); // 2, 1
    REQUIRE(RefusedWith(cp, what));
    cp = Restorable(false);
    cp.channels.erase(cp.channels.begin()); // starts at 2
    REQUIRE(RefusedWith(cp, what));
    cp = Restorable(false);
    cp.channels.clear();
    REQUIRE(RefusedWith(cp, "no channel"));
}

TEST_CASE("an unknown engine class is refused", "[video_core][replay][checkpoint]") {
    R::GpuCheckpoint cp = Restorable(false);
    cp.engines[0].engine_class = static_cast<R::EngineClass>(9);
    REQUIRE(RefusedWith(cp, "unknown engine class 9"));
    cp = Restorable(false);
    cp.engines[0].engine_class = static_cast<R::EngineClass>(0);
    REQUIRE(RefusedWith(cp, "unknown engine class 0"));
    cp = Restorable(false);
    cp.subchannels[0].classes[0] = R::EngineClass::Host; // the puller is not a subchannel engine
    REQUIRE(RefusedWith(cp, "no subchannel can hold"));
    // The decoder refuses the number as well, before a restore sees it.
    cp = Restorable(false);
    cp.engines[0].engine_class = static_cast<R::EngineClass>(9);
    R::GpuCheckpoint back;
    REQUIRE(R::Decode(Enc(cp), back) == R::ReadError::BadPayload);
}

TEST_CASE("an oversized or wrongly sized register block is refused",
          "[video_core][replay][checkpoint]") {
    for (const bool turing : {false, true}) {
        const R::GpuCheckpoint good = Restorable(turing);
        for (size_t i = 0; i < 6; ++i) {
            R::GpuCheckpoint cp = good;
            cp.engines[i].registers.push_back(0);
            REQUIRE(RefusedWith(cp, "oversized register block"));
            cp = good;
            cp.engines[i].registers.pop_back();
            REQUIRE(RefusedWith(cp, "register block of"));
            cp = good;
            cp.engines[i].registers.clear();
            REQUIRE(RefusedWith(cp, "register block of"));
        }
        R::GpuCheckpoint cp = good;
        cp.engines.erase(cp.engines.begin() + 3);
        REQUIRE(RefusedWith(cp, "lacks the registers"));
    }
    // Over what the format allows at all: the decoder stops it before any allocation.
    R::GpuCheckpoint cp = Restorable(false);
    cp.engines[0].registers = Words(R::MaxEngineRegisters + 1, 5);
    R::GpuCheckpoint back;
    REQUIRE(R::Decode(Enc(cp), back) == R::ReadError::LimitExceeded);
    REQUIRE(RefusedWith(cp, "oversized register block"));
}

TEST_CASE("a section of the wrong size is BadPayload, not a crash",
          "[video_core][replay][checkpoint]") {
    const Bytes bytes = Enc(Restorable(false));
    R::GpuCheckpoint back;
    REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
    REQUIRE(R::Decode(GrowFirstSubRecord(bytes), back) == R::ReadError::BadPayload);
    // Cut anywhere, the decoder reports an error and does not read past the end.
    for (size_t size = 0; size < bytes.size(); size += 97) {
        const std::span<const u8> prefix{bytes.data(), size};
        REQUIRE(R::Decode(prefix, back) != R::ReadError::None);
    }
}

TEST_CASE("sections that do not match the checkpoint's own shape are refused",
          "[video_core][replay][checkpoint]") {
    R::GpuCheckpoint cp = Restorable(false);
    cp.channels[0].address_space_id = 99;
    REQUIRE(RefusedWith(cp, "does not hold"));
    cp = Restorable(false);
    cp.address_spaces[0].split_address = (u64{1} << 40) + 1;
    REQUIRE(RefusedWith(cp, "no MemoryManager has"));
    cp = Restorable(true);
    cp.channels[1].flags = 0;
    REQUIRE(RefusedWith(cp, "mix Turing"));
    cp = Restorable(false);
    cp.macros.push_back({R::SharedChannel, R::MacroSectionKind::Code, {}});
    REQUIRE(RefusedWith(cp, "does not belong"));
    cp = Restorable(false);
    cp.macros[0].words = {5, 9, 1}; // a run that claims nine words
    REQUIRE(RefusedWith(cp, "runs"));
    cp = Restorable(false);
    cp.macros[2].words = {5, 1};
    REQUIRE(RefusedWith(cp, "triples"));
    cp = Restorable(true);
    std::erase_if(cp.macros, [](const R::MacroSection& m) {
        return m.kind == R::MacroSectionKind::DataRam;
    });
    REQUIRE(RefusedWith(cp, "is missing"));
    cp = Restorable(true);
    cp.macros.back().words.pop_back();
    REQUIRE(RefusedWith(cp, "macro section"));
    cp = Restorable(false);
    cp.shadows[0].words.pop_back();
    REQUIRE(RefusedWith(cp, "shadow RAM"));
    cp = Restorable(false);
    cp.states[1].words.push_back(0);
    REQUIRE(RefusedWith(cp, "engine state"));
    cp = Restorable(false);
    cp.states.pop_back();
    REQUIRE(RefusedWith(cp, "lacks its root tables"));
    cp = Restorable(false);
    cp.dma[0].subchannel = 9;
    REQUIRE(RefusedWith(cp, "subchannel"));
    cp = Restorable(false);
    cp.dma.pop_back();
    REQUIRE(RefusedWith(cp, "no DMA state"));
    cp = Restorable(false);
    cp.fences[0].id = 0;
    REQUIRE(RefusedWith(cp, "syncpoint a channel can hold"));
    cp = Restorable(false);
    cp.fences[0].kind = R::FenceKind::SemaphoreWrite;
    REQUIRE(RefusedWith(cp, "syncpoint a channel can hold"));
}

TEST_CASE("the puller serializes as many words as a checkpoint accepts and restores them",
          "[video_core][replay][checkpoint]") {
    using Tegra::Engines::Puller;
    Puller puller{};
    std::vector<u32> words = puller.StateWords();
    REQUIRE(words.size() == Puller::StateWordCount);
    REQUIRE(words.size() == Puller::RegisterWordCount + 8);

    // Registers with distinct values, and a known engine on five of the eight subchannels.
    const std::vector<u32> registers = Words(Puller::RegisterWordCount, 5);
    std::copy(registers.begin(), registers.end(), words.begin());
    const u32 engines[8] = {0xB197, 0xB1C0, 0xA140, 0x902D, 0xB0B5, 0, 0, 0};
    std::copy(std::begin(engines), std::end(engines), words.begin() + Puller::RegisterWordCount);
    REQUIRE(puller.RestoreStateWords(words));
    REQUIRE(puller.StateWords() == words);

    // A size the puller did not produce, or an engine this build lacks, writes nothing.
    std::vector<u32> longer = words;
    longer.push_back(0);
    REQUIRE_FALSE(puller.RestoreStateWords(longer));
    std::vector<u32> shorter = words;
    shorter.pop_back();
    REQUIRE_FALSE(puller.RestoreStateWords(shorter));
    std::vector<u32> unknown = Words(Puller::StateWordCount, 9);
    unknown[Puller::RegisterWordCount] = 0x1234;
    REQUIRE_FALSE(puller.RestoreStateWords(unknown));
    REQUIRE(puller.StateWords() == words);
}

namespace {

// A serialized QMD table of `count` records at distinct addresses.
std::vector<u32> QmdTable(size_t count, u32 flags = 1) {
    std::vector<u32> words(count * Tegra::NativeQmdScheduler::SerializedStateWords);
    for (size_t i = 0; i < count; ++i) {
        u32* record = words.data() + i * Tegra::NativeQmdScheduler::SerializedStateWords;
        record[0] = 0x1000 + static_cast<u32>(i) * 0x100;
        record[1] = 0x20;
        record[4] = flags;
    }
    return words;
}

// Syncpoint counters in which every id has the guest value `10 + id` and host value 0, except the
// fences of Restorable (id 10 + channel, value 3), which the section has to agree with.
std::vector<u32> Counters() {
    std::vector<u32> words(R::SyncpointCounterWords);
    for (u32 id = 0; id < R::SyncpointCounterCount; ++id) {
        words[size_t{id} * 2] = 10 + id;
    }
    return words;
}

R::GpuCheckpoint WithCounters(R::GpuCheckpoint cp) {
    std::vector<u32> words = Counters();
    for (const R::CheckpointFence& fence : cp.fences) {
        words[size_t{fence.id} * 2] = static_cast<u32>(fence.value);
    }
    cp.states.push_back({R::SharedChannel, R::EngineStateKind::SyncpointCounters, std::move(words)});
    return cp;
}

} // namespace

TEST_CASE("a QMD table in a checkpoint is checked before anything is restored",
          "[video_core][replay][checkpoint]") {
    const auto with_table = [](std::vector<u32> table) {
        R::GpuCheckpoint cp = Restorable(true);
        cp.states.push_back({R::SharedChannel, R::EngineStateKind::NativeQmdStates,
                             std::move(table)});
        return cp;
    };
    R::GpuCheckpoint good = with_table(QmdTable(3, 7));
    std::string error;
    REQUIRE(R::ValidateCheckpointForRestore(good, error));
    R::GpuCheckpoint back;
    REQUIRE(R::Decode(Enc(good), back) == R::ReadError::None);
    REQUIRE(R::ValidateCheckpointForRestore(back, error));

    // The same address twice: restoring would keep whichever came last.
    auto twice = QmdTable(2);
    std::copy_n(twice.begin(), 2, twice.begin() + Tegra::NativeQmdScheduler::SerializedStateWords);
    REQUIRE(RefusedWith(with_table(twice), "QMD table names an address twice"));
    // More records than the scheduler ever holds.
    REQUIRE(RefusedWith(with_table(QmdTable(Tegra::NativeQmdScheduler::MaxStates + 1)),
                        "more records than the scheduler holds"));
    REQUIRE(R::ValidateCheckpointForRestore(with_table(QmdTable(Tegra::NativeQmdScheduler::MaxStates)),
                                            error));
    // A flag no scheduler sets, and a table that is not whole records.
    REQUIRE(RefusedWith(with_table(QmdTable(2, 8)), "unknown flags"));
    auto torn = QmdTable(2);
    torn.pop_back();
    REQUIRE(RefusedWith(with_table(torn), "not whole records"));
    // One table for the whole GPU.
    R::GpuCheckpoint repeated = with_table(QmdTable(1));
    repeated.states.push_back({R::SharedChannel, R::EngineStateKind::NativeQmdStates, QmdTable(1)});
    REQUIRE(RefusedWith(repeated, "misplaced or repeated"));
}

TEST_CASE("the syncpoint counters of a checkpoint are one shared section that its fences agree with",
          "[video_core][replay][checkpoint]") {
    const R::GpuCheckpoint good = WithCounters(Restorable(false));
    std::string error;
    REQUIRE(R::ValidateCheckpointForRestore(good, error));
    R::GpuCheckpoint back;
    REQUIRE(R::Decode(Enc(good), back) == R::ReadError::None);
    REQUIRE(Enc(back) == Enc(good));
    REQUIRE(R::ValidateCheckpointForRestore(back, error));
    const auto counters = std::ranges::find_if(back.states, [](const R::EngineStateSection& s) {
        return s.kind == R::EngineStateKind::SyncpointCounters;
    });
    REQUIRE(counters != back.states.end());
    REQUIRE(counters->channel_id == R::SharedChannel);
    REQUIRE(counters->words.size() == R::SyncpointCounterWords);
    REQUIRE(counters->words[size_t{11} * 2] == 3);

    const char* malformed = "SyncpointCounters section is malformed or repeated";
    R::GpuCheckpoint cp = good;
    cp.states.back().words.pop_back();
    REQUIRE(RefusedWith(cp, malformed));
    cp = good;
    cp.states.back().channel_id = 1;
    REQUIRE(RefusedWith(cp, malformed));
    cp = good;
    cp.states.push_back(cp.states.back());
    REQUIRE(RefusedWith(cp, malformed));
    // A channel's fence is the counter of its syncpoint: they cannot differ.
    cp = good;
    cp.states.back().words[size_t{11} * 2] += 1;
    REQUIRE(RefusedWith(cp, "fence disagrees with the syncpoint counters section"));
}
