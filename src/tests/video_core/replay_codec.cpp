// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests of the 0.6 capture codec and file layer (format_io.cpp). Every byte here is synthetic:
// the captures are built in memory with the writer from made-up allocations, commands and calls.
// Files go to a unique temporary directory that each test removes when it ends.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/replay/checkpoint.h"
#include "video_core/replay/events.h"
#include "video_core/replay/format.h"
#include "video_core/replay/present_extent.h"
#include "video_core/replay/format_io.h"
#include "video_core/replay/library_call.h"

namespace R = VideoCore::Replay;
namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<u8>;

// ---- files -------------------------------------------------------------------------------------

class TempDir {
public:
    TempDir() {
        static unsigned counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path() /
              ("eglantine_replay_codec_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        fs::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] fs::path File(const std::string& name) const {
        return dir / name;
    }

private:
    fs::path dir;
};

Bytes ReadFile(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return Bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void WriteFile(const fs::path& path, std::span<const u8> bytes) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

// ---- payload builders ----------------------------------------------------------------------------

template <class T>
Bytes Enc(const T& value) {
    Bytes out;
    R::Encode(value, out);
    return out;
}

Bytes EncMaps(const std::vector<R::MapEntry>& value) {
    Bytes out;
    R::Encode(std::span<const R::MapEntry>{value}, out);
    return out;
}

Bytes EncUnmaps(const std::vector<R::UnmapEntry>& value) {
    Bytes out;
    R::Encode(std::span<const R::UnmapEntry>{value}, out);
    return out;
}

Bytes EncFences(const std::vector<R::FenceEntry>& value) {
    Bytes out;
    R::Encode(std::span<const R::FenceEntry>{value}, out);
    return out;
}

Bytes EncEvents(const std::vector<R::GpuEvent>& value) {
    Bytes out;
    R::Encode(std::span<const R::GpuEvent>{value}, out);
    return out;
}

R::SessionRecord MakeSession() {
    R::SessionRecord session;
    session.pins.push_back({static_cast<u32>(R::SettingKey::Backend), 1});
    session.pins.push_back({static_cast<u32>(R::SettingKey::VSync), 0});
    session.device.vendor_id = 0x1111;
    session.device.device_id = 0x2222;
    session.device.driver_id = 3;
    session.device.driver_version = 4;
    session.device.pipeline_cache_uuid[0] = 0xAB;
    return session;
}

R::GpuCheckpoint MakeCheckpoint() {
    R::GpuCheckpoint checkpoint;
    checkpoint.channels.push_back({1, 1, 0});
    checkpoint.address_spaces.push_back({1, 40, 16, 0, u64{1} << 34});
    R::EngineRegisters engine;
    engine.channel_id = 1;
    engine.engine_class = R::EngineClass::Maxwell3D;
    engine.registers = {1, 2, 3};
    checkpoint.engines.push_back(engine);
    R::EngineRegisters host;
    host.channel_id = 1;
    host.engine_class = R::EngineClass::Host;
    host.registers = {7, 8};
    checkpoint.engines.push_back(host);
    checkpoint.macros.push_back({1, R::MacroSectionKind::DataRam, {9, 10}});
    checkpoint.macros.push_back({R::SharedChannel, R::MacroSectionKind::ComputePositions, {11}});
    checkpoint.states.push_back({1, R::EngineStateKind::RootTables, {12, 13}});
    checkpoint.states.push_back({1, R::EngineStateKind::ConstBufferBindings, {14}});
    R::DmaState dma;
    dma.channel_id = 1;
    dma.split_method_end = 0x1234;
    dma.parked.push_back({1, 2, 3, 0, 0x5678});
    checkpoint.dma.push_back(dma);
    checkpoint.fences.push_back({R::FenceKind::SyncpointRaised, 1, 5, 1, 0});
    return checkpoint;
}

R::MemoryBlob MakeBlob() {
    R::MemoryBlob blob;
    blob.alloc_id = 1;
    blob.generation = 1;
    blob.flags = R::BlobInitial;
    blob.allocation_size = 64;
    R::MemoryRange data;
    data.offset = 0;
    data.length = 16;
    data.kind = R::RangeKind::Data;
    data.writer = R::WriterKind::Cpu;
    data.data_offset = 0;
    R::MemoryRange zero;
    zero.offset = 16;
    zero.length = 16;
    zero.kind = R::RangeKind::Zero;
    R::MemoryRange undefined;
    undefined.offset = 32;
    undefined.length = 32;
    undefined.kind = R::RangeKind::Undefined;
    undefined.writer = R::WriterKind::GpuDerived;
    blob.ranges = {data, zero, undefined};
    for (u8 i = 0; i < 16; ++i) {
        blob.data.push_back(static_cast<u8>(0x10 + i));
    }
    return blob;
}

// A delta blob: bytes 0..16 equal those of the blob chunk with `reference`, 32..40 are new.
R::MemoryBlob MakeDeltaBlob(u32 reference) {
    R::MemoryBlob blob;
    blob.alloc_id = 1;
    blob.generation = 1;
    blob.flags = R::BlobDelta;
    blob.allocation_size = 64;
    R::MemoryRange ref;
    ref.offset = 0;
    ref.length = 16;
    ref.kind = R::RangeKind::Ref;
    ref.ref_sequence = reference;
    R::MemoryRange data;
    data.offset = 32;
    data.length = 8;
    data.kind = R::RangeKind::Data;
    data.data_offset = 0;
    blob.ranges = {ref, data};
    blob.data = {9, 8, 7, 6, 5, 4, 3, 2};
    return blob;
}

std::vector<R::MapEntry> MakeMaps() {
    R::MapEntry map;
    map.address_space_id = 1;
    map.gpu_va = 0x1000;
    map.size = 64;
    map.alloc_id = 1;
    map.alloc_offset = 0;
    map.generation = 1;
    return {map};
}

std::vector<R::GpuEvent> MakeEvents(u64 submit_id) {
    std::vector<R::GpuEvent> events;
    R::GpuEvent create;
    create.kind = R::GpuEventKind::ChannelCreate;
    create.channel_create = {1, 1, 0};
    R::GpuEvent submit;
    submit.kind = R::GpuEventKind::Submit;
    submit.submit.channel_id = 1;
    submit.submit.submit_id = submit_id;
    submit.submit.entries.push_back({0x1000, 2, 0});
    R::GpuEvent consume;
    consume.kind = R::GpuEventKind::Consume;
    consume.consume.channel_id = 1;
    consume.consume.entry_index = 0;
    consume.consume.submit_id = submit_id;
    consume.consume.consume_id = submit_id;
    consume.consume.gpu_va = 0x1000;
    consume.consume.flags = R::ConsumeEntryEnd;
    consume.consume.words = {1, 2};
    if (submit_id == 1) {
        events.push_back(create);
    }
    events.push_back(submit);
    events.push_back(consume);
    return events;
}

R::PresentRecord MakePresent(u64 frame) {
    R::PresentRecord present;
    present.path = R::PresentPath::Nx2;
    present.present_id = frame;
    present.output_id = 1;
    R::PresentLayer layer;
    layer.alloc_id = 1;
    layer.generation = 1;
    layer.width = 2;
    layer.height = 2;
    layer.stride = 2;
    layer.pixel_format = 1;
    layer.crop_right = 2;
    layer.crop_bottom = 2;
    present.layers.push_back(layer);
    return present;
}

R::EndPicture MakePicture(u64 frame) {
    R::EndPicture picture;
    picture.frame_id = frame;
    picture.kind = R::PictureKind::Composed;
    picture.width = 2;
    picture.height = 2;
    picture.stride_bytes = 8;
    for (u8 i = 0; i < 16; ++i) {
        picture.pixels.push_back(static_cast<u8>(i * 3 + static_cast<u8>(frame)));
    }
    picture.pixel_checksum = R::Fnv1a64(picture.pixels);
    return picture;
}

R::FrameEndRecord MakeFrameEnd(u64 frame, u64 first, u64 last) {
    R::FrameEndRecord end;
    end.frame_id = frame;
    end.flags = R::FrameComplete | R::FrameHasLivePicture;
    end.gpu_words = 2;
    end.call_count = (u64{1} << 40) + frame;
    end.effect_count = 3;
    end.first_sequence = first;
    end.last_sequence = last;
    end.output_id = 1;
    end.outputs.push_back({R::SemanticKind::SemaphoreValue, 0x1000, 7, 0});
    return end;
}

R::IndexRecord MakeIndex() {
    R::IndexRecord index;
    index.entries.push_back({1, 1, 0, 256, 0, 40});
    index.entries.push_back({4, 1, 0, 360, 10, 100});
    index.frames.push_back({10, 1, 1, R::FrameComplete, 1});
    return index;
}

R::CompleteRecord MakeComplete() {
    R::CompleteRecord complete;
    complete.status = R::CompleteStatus::Complete;
    complete.chain_digest = 0x1234;
    complete.total_frames = 1;
    complete.total_chunks = 4;
    complete.total_bytes = 1000;
    complete.dependency_complete = 1;
    return complete;
}

// The library of the tests. Two generic functions: one creates an object through an output
// argument, one destroys the object it is given. Names carry no meaning.
R::LibrarySchema MakeSchema() {
    R::LibrarySchema schema;
    schema.abi_hash = 0x77;
    schema.api_word0 = 1;
    schema.api_word1 = 2;
    schema.modules.push_back({1, 0x99});
    schema.types.push_back({1, "handle"});
    R::FunctionSchema create;
    create.function_id = 1;
    create.flags = R::SchemaComplete | R::SchemaCreatesObject;
    create.signature_hash = 0xA1;
    create.name = "make_thing";
    create.args.push_back({R::ArgKind::ObjectRef, 0, R::ArgDescOut, 1});
    R::FunctionSchema destroy;
    destroy.function_id = 2;
    destroy.flags = R::SchemaComplete | R::SchemaDestroysObject;
    destroy.signature_hash = 0xA2;
    destroy.name = "drop_thing";
    destroy.args.push_back({R::ArgKind::ObjectRef, 0, R::ArgDescIn, 1});
    schema.functions = {create, destroy};
    return schema;
}

R::ArgValue Obj(u64 id, u64 generation) {
    R::ArgValue value;
    value.kind = R::ArgKind::ObjectRef;
    value.aux = 1;
    value.value = id;
    value.value2 = generation;
    return value;
}

// A LIBRARY_EVENT / LIBRARY_CHECKPOINT payload under construction.
class RecordList {
public:
    void Add(R::CallKind kind, u64 seq, u32 function, u64 frame, R::LinkKind link, u64 link_id,
             const std::vector<R::ArgValue>& args) {
        R::CallRecordView view;
        view.kind = kind;
        view.call_seq = seq;
        view.function_id = function;
        view.thread_id = 1;
        view.frame_id = frame;
        view.link_kind = link;
        view.link_id = link_id;
        view.args = args;
        REQUIRE(R::Encode(view, body));
        ++count;
    }

    [[nodiscard]] Bytes Payload() const {
        Bytes out;
        R::ByteWriter writer{out};
        writer.Put32(count);
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

private:
    Bytes body;
    u32 count = 0;
};

// ---- building captures ---------------------------------------------------------------------------

enum class LibraryMode {
    None,
    Clean,           // every call returns, every reference resolves: replay ready
    OpenCall,        // a call never returns
    DanglingObject,  // a call names an object nothing created
    BadLink,         // a record links to a submit that does not exist
    ClaimsReady,     // an open call, but the producer still claims library_replay_ready
};

struct Built {
    fs::path path;
    R::FileHeader header;
    R::CompleteRecord complete;
};

R::FileHeader MakeHeader(bool library) {
    R::FileHeader header;
    header.first_frame = 10;
    header.requested_frames = 2;
    header.finalized_frames = 2;
    header.preroll_frames = 0;
    header.flags = R::HeaderHasLivePictures | (library ? R::HeaderHasLibrarySection : 0U);
    header.session_id = 0x1234;
    header.revision[0] = 't';
    return header;
}

void Add(R::CaptureWriter& writer, R::ChunkType type, u64 frame, const Bytes& payload) {
    std::string error;
    const bool ok = writer.Append(type, frame, payload, error);
    INFO(error);
    REQUIRE(ok);
}

// Two frames (10 and 11) with the whole chunk grammar. With `clean` false the capture is
// finalized as interrupted, which leaves the partial file in place.
Built Build(const TempDir& dir, const std::string& name, LibraryMode mode, bool clean = true,
            u64 mapped_bytes = 32, u64 present_bytes = 65536) {
    Built built;
    built.path = dir.File(name + ".egcap");
    const bool library = mode != LibraryMode::None;
    built.header = MakeHeader(library);
    R::CaptureWriter writer;
    std::string error;
    {
        const bool opened = writer.Open(built.path, built.header, error);
        INFO(error);
        REQUIRE(opened);
    }
    Add(writer, R::ChunkType::Session, 0, Enc(MakeSession()));
    if (library) {
        Add(writer, R::ChunkType::LibrarySchema, 0, Enc(MakeSchema()));
    }
    // The checkpoint chunks belong to the first stored frame, frame 10.
    const u64 first10 = writer.NextSequence();
    Add(writer, R::ChunkType::GpuCheckpoint, 10, Enc(MakeCheckpoint()));
    if (library) {
        Add(writer, R::ChunkType::LibraryCheckpoint, 10, RecordList{}.Payload());
    }

    // Frame 10.
    const u64 blob10 = writer.NextSequence();
    auto initial = MakeBlob();
    initial.allocation_size += present_bytes;
    R::MemoryRange pixels{};
    pixels.offset = 64;
    pixels.length = present_bytes;
    pixels.kind = R::RangeKind::Zero;
    initial.ranges.push_back(pixels);
    Add(writer, R::ChunkType::MemoryBlob, 10, Enc(initial));
    const auto present = [](u64 frame) {
        auto value = MakePresent(frame);
        auto& layer = value.layers.front();
        layer.width = layer.stride = 128;
        layer.height = 65;
        layer.alloc_offset = 64;
        return value;
    };
    auto maps = MakeMaps();
    maps.front().size = mapped_bytes;
    Add(writer, R::ChunkType::Map, 10, EncMaps(maps));
    Add(writer, R::ChunkType::GpuEvent, 10, EncEvents(MakeEvents(1)));
    Add(writer, R::ChunkType::Fence, 10,
        EncFences({{R::FenceKind::SyncpointRaised, 1, 5, 1, 0, 1}}));
    if (library) {
        RecordList calls;
        calls.Add(R::CallKind::Call, 1, 1, 10, R::LinkKind::Submit,
                  mode == LibraryMode::BadLink ? 99 : 1, {R::ArgValue{}});
        calls.Add(R::CallKind::Return, 1, 1, 10, R::LinkKind::None, 0, {Obj(1, 1)});
        Add(writer, R::ChunkType::LibraryEvent, 10, calls.Payload());
    }
    Add(writer, R::ChunkType::Present, 10, Enc(present(10)));
    Add(writer, R::ChunkType::EndPicture, 10, Enc(MakePicture(10)));
    Add(writer, R::ChunkType::FrameEnd, 10,
        Enc(MakeFrameEnd(10, first10, writer.NextSequence())));

    // Frame 11.
    const u64 first11 = writer.NextSequence();
    auto delta = MakeDeltaBlob(static_cast<u32>(blob10));
    delta.allocation_size = initial.allocation_size;
    Add(writer, R::ChunkType::MemoryBlob, 11, Enc(delta));
    Add(writer, R::ChunkType::GpuEvent, 11, EncEvents(MakeEvents(2)));
    if (library) {
        RecordList calls;
        const u64 target = mode == LibraryMode::DanglingObject ? 7 : 1;
        calls.Add(R::CallKind::Call, 2, 2, 11, R::LinkKind::Present, 11, {Obj(target, 1)});
        if (mode != LibraryMode::OpenCall && mode != LibraryMode::ClaimsReady) {
            calls.Add(R::CallKind::Return, 2, 2, 11, R::LinkKind::None, 0, {});
        }
        Add(writer, R::ChunkType::LibraryEvent, 11, calls.Payload());
    }
    Add(writer, R::ChunkType::Present, 11, Enc(present(11)));
    Add(writer, R::ChunkType::EndPicture, 11, Enc(MakePicture(11)));
    Add(writer, R::ChunkType::FrameEnd, 11,
        Enc(MakeFrameEnd(11, first11, writer.NextSequence())));

    built.complete.status = clean ? R::CompleteStatus::Complete : R::CompleteStatus::Incomplete;
    built.complete.incomplete_reasons = clean ? 0U : static_cast<u32>(R::IncompleteInterrupted);
    built.complete.dependency_complete = 1;
    built.complete.library_replay_ready =
        (mode == LibraryMode::Clean || mode == LibraryMode::ClaimsReady) ? 1U : 0U;
    built.complete.gpu_words = 4;
    const bool finalized = writer.Finalize(built.header, built.complete, error);
    INFO(error);
    REQUIRE(finalized == clean);
    return built;
}

R::ReadError OpenFile(R::CaptureReader& reader, const fs::path& path, std::string& detail) {
    return reader.Open(path, detail);
}

R::ReadError OpenAndValidate(R::CaptureReader& reader, const fs::path& path,
                             std::string& detail) {
    const R::ReadError opened = OpenFile(reader, path, detail);
    if (opened != R::ReadError::None) {
        return opened;
    }
    return reader.Validate(detail);
}

// ---- tampering -----------------------------------------------------------------------------------

[[nodiscard]] u64 OffsetOf(const R::CaptureReader& reader, R::ChunkType type, size_t nth = 0) {
    size_t seen = 0;
    for (const R::IndexEntry& entry : reader.Index().entries) {
        if (entry.type == static_cast<u32>(type) && seen++ == nth) {
            return entry.file_offset;
        }
    }
    REQUIRE(false);
    return 0;
}

[[nodiscard]] u64 IndexChunkOffset(const R::CaptureReader& reader) {
    return reader.Header().index_offset;
}

// The COMPLETE chunk ends the file, so it starts a fixed distance before the end.
[[nodiscard]] u64 CompleteChunkOffset(const Bytes& file) {
    return file.size() - (u64{R::ChunkHeaderBytes} + 136);
}

R::ChunkHeader HeaderAt(const Bytes& file, u64 offset) {
    R::ChunkHeader header;
    const R::ReadError error = R::DecodeChunkHeader(
        std::span<const u8>{file}.subspan(static_cast<size_t>(offset), R::ChunkHeaderBytes), header);
    REQUIRE(error == R::ReadError::None);
    return header;
}

// Rewrites the chunk at `offset` with a new payload of the same length and fresh checksums.
void RewriteChunk(Bytes& file, u64 offset, const Bytes& payload) {
    R::ChunkHeader header = HeaderAt(file, offset);
    REQUIRE(header.stored_length == payload.size());
    const std::array<u8, R::ChunkHeaderBytes> head = R::EncodeChunkHeader(header, payload);
    std::copy(head.begin(), head.end(), file.begin() + static_cast<std::ptrdiff_t>(offset));
    std::copy(payload.begin(), payload.end(),
              file.begin() + static_cast<std::ptrdiff_t>(offset + R::ChunkHeaderBytes));
}

// Rewrites only the header of the chunk at `offset` (payload bytes stay).
void RewriteHeader(Bytes& file, u64 offset, const R::ChunkHeader& changed) {
    R::ChunkHeader header = changed;
    const Bytes payload(file.begin() + static_cast<std::ptrdiff_t>(offset + R::ChunkHeaderBytes),
                        file.begin() + static_cast<std::ptrdiff_t>(offset + R::ChunkHeaderBytes +
                                                                   header.stored_length));
    const std::array<u8, R::ChunkHeaderBytes> head = R::EncodeChunkHeader(header, payload);
    std::copy(head.begin(), head.end(), file.begin() + static_cast<std::ptrdiff_t>(offset));
}

void WriteHeaderBytes(Bytes& file, const R::FileHeader& header) {
    const Bytes bytes = R::EncodeFileHeader(header);
    REQUIRE(bytes.size() == R::FileHeaderBytes);
    std::copy(bytes.begin(), bytes.end(), file.begin());
}

R::FileHeader ParseHeader(const Bytes& file) {
    R::FileHeader header;
    const R::ReadError error = R::DecodeFileHeader(file, header);
    REQUIRE(error == R::ReadError::None);
    return header;
}

Bytes Prefix(const Bytes& bytes, size_t length) {
    return Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
}

template <class T>
void RequireEveryPrefixFails(const Bytes& encoded) {
    for (size_t length = 0; length < encoded.size(); ++length) {
        T value;
        const Bytes cut = Prefix(encoded, length);
        INFO(length);
        REQUIRE(R::Decode(cut, value) != R::ReadError::None);
    }
}

}  // namespace

// ---- payload codecs -------------------------------------------------------------------------------

TEST_CASE("Replay codec: every payload round-trips byte for byte", "[video_core][replay][codec]") {
    {
        const Bytes bytes = Enc(MakeSession());
        R::SessionRecord back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.pins.size() == 2);
        REQUIRE(back.pins[1].key == static_cast<u32>(R::SettingKey::VSync));
        REQUIRE(back.device.vendor_id == 0x1111);
        REQUIRE(back.device.pipeline_cache_uuid[0] == 0xAB);
        REQUIRE(Enc(back) == bytes);
    }
    {
        const Bytes bytes = Enc(MakeCheckpoint());
        R::GpuCheckpoint back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.channels.size() == 1);
        REQUIRE(back.address_spaces.size() == 1);
        REQUIRE(back.address_spaces[0].split_address == (u64{1} << 34));
        REQUIRE(back.engines.size() == 2);
        REQUIRE(back.engines[0].registers.size() == 3);
        REQUIRE(back.engines[1].engine_class == R::EngineClass::Host);
        REQUIRE(back.macros.size() == 2);
        REQUIRE(back.macros[0].kind == R::MacroSectionKind::DataRam);
        REQUIRE(back.macros[1].kind == R::MacroSectionKind::ComputePositions);
        REQUIRE(back.states.size() == 2);
        REQUIRE(back.states[1].kind == R::EngineStateKind::ConstBufferBindings);
        REQUIRE(back.states[1].words == std::vector<u32>{14});
        REQUIRE(back.fences.size() == 1);
        REQUIRE(back.dma.size() == 1);
        REQUIRE(back.dma[0].split_method_end == 0x1234);
        REQUIRE(back.dma[0].parked.size() == 1);
        REQUIRE(Enc(back) == bytes);
    }
    {
        const Bytes bytes = Enc(MakeSchema());
        R::LibrarySchema back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.functions.size() == 2);
        REQUIRE(back.functions[1].name == "drop_thing");
        REQUIRE(back.functions[0].args[0].kind == R::ArgKind::ObjectRef);
        REQUIRE(back.modules.size() == 1);
        REQUIRE(back.types.size() == 1);
        REQUIRE(Enc(back) == bytes);
    }
    {
        RecordList list;
        list.Add(R::CallKind::Call, 1, 1, 10, R::LinkKind::Submit, 4, {R::ArgValue{}});
        list.Add(R::CallKind::Return, 1, 1, 10, R::LinkKind::None, 0, {Obj(1, 1)});
        const Bytes bytes = list.Payload();
        std::vector<R::CallRecord> back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.size() == 2);
        REQUIRE(back[0].link_kind == R::LinkKind::Submit);
        REQUIRE(back[0].link_id == 4);
        REQUIRE(back[1].args[0].value == 1);
        REQUIRE(back[1].kind == R::CallKind::Return);
    }
    {
        const Bytes bytes = Enc(MakeBlob());
        R::MemoryBlob back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.ranges.size() == 3);
        REQUIRE(back.data.size() == 16);
        REQUIRE(back.data[3] == 0x13);
        REQUIRE(back.ranges[2].writer == R::WriterKind::GpuDerived);
        REQUIRE(Enc(back) == bytes);
    }
    {
        const Bytes bytes = Enc(MakeDeltaBlob(5));
        R::MemoryBlob back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.ranges[0].kind == R::RangeKind::Ref);
        REQUIRE(back.ranges[0].ref_sequence == 5);
        REQUIRE(Enc(back) == bytes);
    }
    {
        const Bytes bytes = EncMaps(MakeMaps());
        std::vector<R::MapEntry> back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.size() == 1);
        REQUIRE(back[0].gpu_va == 0x1000);
        REQUIRE(EncMaps(back) == bytes);
    }
    {
        const Bytes bytes = EncUnmaps({{1, 0, 0x1000, 64}});
        std::vector<R::UnmapEntry> back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.size() == 1);
        REQUIRE(back[0].size == 64);
        REQUIRE(EncUnmaps(back) == bytes);
    }
    {
        const Bytes bytes = EncFences({{R::FenceKind::QueryValue, 2, 0x2000, 9, 8, 0}});
        std::vector<R::FenceEntry> back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.size() == 1);
        REQUIRE(back[0].kind == R::FenceKind::QueryValue);
        REQUIRE(back[0].aux == 8);
        REQUIRE(EncFences(back) == bytes);
    }
    {
        std::vector<R::GpuEvent> events = MakeEvents(1);
        R::GpuEvent release;
        release.kind = R::GpuEventKind::ChannelRelease;
        release.channel_release.channel_id = 1;
        events.push_back(release);
        const Bytes bytes = EncEvents(events);
        std::vector<R::GpuEvent> back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.size() == 4);
        REQUIRE(back[1].submit.entries.size() == 1);
        REQUIRE(back[2].consume.words.size() == 2);
        REQUIRE(back[3].kind == R::GpuEventKind::ChannelRelease);
        REQUIRE(EncEvents(back) == bytes);
    }
    {
        const Bytes bytes = Enc(MakePresent(10));
        R::PresentRecord back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.present_id == 10);
        REQUIRE(back.layers.size() == 1);
        REQUIRE(back.layers[0].crop_right == 2);
        REQUIRE(Enc(back) == bytes);
    }
    {
        const Bytes bytes = Enc(MakeFrameEnd(10, 4, 9));
        R::FrameEndRecord back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.first_sequence == 4);
        REQUIRE(back.last_sequence == 9);
        REQUIRE(back.call_count == (u64{1} << 40) + 10);
        REQUIRE(back.effect_count == 3);
        REQUIRE(back.outputs.size() == 1);
        REQUIRE(Enc(back) == bytes);
    }
    {
        const Bytes bytes = Enc(MakePicture(10));
        R::EndPicture back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.pixels.size() == 16);
        REQUIRE(back.pixel_checksum == R::Fnv1a64(back.pixels));
        REQUIRE(Enc(back) == bytes);
    }
    {
        const Bytes bytes = Enc(MakeIndex());
        R::IndexRecord back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.entries.size() == 2);
        REQUIRE(back.frames.size() == 1);
        REQUIRE(back.entries[1].frame_id == 10);
        REQUIRE(Enc(back) == bytes);
    }
    {
        const Bytes bytes = Enc(MakeComplete());
        REQUIRE(bytes.size() == 136);
        R::CompleteRecord back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::None);
        REQUIRE(back.chain_digest == 0x1234);
        REQUIRE(back.dependency_complete == 1);
        REQUIRE(R::CompleteIsClean(back));
        REQUIRE(Enc(back) == bytes);
    }
}

TEST_CASE("Replay codec: a payload cut at any byte is refused", "[video_core][replay][codec]") {
    RequireEveryPrefixFails<R::SessionRecord>(Enc(MakeSession()));
    RequireEveryPrefixFails<R::GpuCheckpoint>(Enc(MakeCheckpoint()));
    RequireEveryPrefixFails<R::LibrarySchema>(Enc(MakeSchema()));
    RequireEveryPrefixFails<R::MemoryBlob>(Enc(MakeBlob()));
    RequireEveryPrefixFails<std::vector<R::MapEntry>>(EncMaps(MakeMaps()));
    RequireEveryPrefixFails<std::vector<R::UnmapEntry>>(EncUnmaps({{1, 0, 0x1000, 64}}));
    RequireEveryPrefixFails<std::vector<R::FenceEntry>>(
        EncFences({{R::FenceKind::Timestamp, 1, 1, 1, 1, 0}}));
    RequireEveryPrefixFails<std::vector<R::GpuEvent>>(EncEvents(MakeEvents(1)));
    RequireEveryPrefixFails<R::PresentRecord>(Enc(MakePresent(10)));
    RequireEveryPrefixFails<R::FrameEndRecord>(Enc(MakeFrameEnd(10, 1, 2)));
    RequireEveryPrefixFails<R::EndPicture>(Enc(MakePicture(10)));
    RequireEveryPrefixFails<R::IndexRecord>(Enc(MakeIndex()));
    RequireEveryPrefixFails<R::CompleteRecord>(Enc(MakeComplete()));
    RecordList list;
    list.Add(R::CallKind::Call, 1, 1, 10, R::LinkKind::None, 0, {R::ArgValue{}});
    RequireEveryPrefixFails<std::vector<R::CallRecord>>(list.Payload());

    // Bytes after the end of a payload are refused too.
    Bytes longer = Enc(MakeComplete());
    longer.push_back(0);
    R::CompleteRecord complete;
    REQUIRE(R::Decode(longer, complete) != R::ReadError::None);
    Bytes session = Enc(MakeSession());
    session.push_back(0);
    R::SessionRecord back;
    REQUIRE(R::Decode(session, back) == R::ReadError::BadPayload);
}

TEST_CASE("Replay codec: counts and lengths over their bounds are refused",
          "[video_core][replay][codec]") {
    // A count over its bound is LimitExceeded; a count within the bound that the bytes cannot
    // hold is Truncated. Neither allocates for the count.
    Bytes huge_count(24, 0);
    huge_count[0] = huge_count[1] = huge_count[2] = huge_count[3] = 0xFF;
    std::vector<R::MapEntry> maps;
    REQUIRE(R::Decode(huge_count, maps) == R::ReadError::LimitExceeded);
    std::vector<R::FenceEntry> fences;
    REQUIRE(R::Decode(huge_count, fences) == R::ReadError::LimitExceeded);
    std::vector<R::GpuEvent> events;
    REQUIRE(R::Decode(huge_count, events) == R::ReadError::LimitExceeded);
    std::vector<R::UnmapEntry> unmaps;
    REQUIRE(R::Decode(huge_count, unmaps) == R::ReadError::LimitExceeded);
    R::SessionRecord session;
    REQUIRE(R::Decode(huge_count, session) == R::ReadError::LimitExceeded);
    std::vector<R::CallRecord> records;
    REQUIRE(R::Decode(huge_count, records) == R::ReadError::LimitExceeded);

    Bytes few(12, 0);
    few[0] = 100;  // within the bound, not within the bytes
    REQUIRE(R::Decode(few, maps) == R::ReadError::Truncated);
    REQUIRE(R::Decode(few, fences) == R::ReadError::Truncated);
    REQUIRE(R::Decode(few, events) == R::ReadError::Truncated);

    // Range count of a blob.
    Bytes blob;
    {
        R::ByteWriter writer{blob};
        writer.Put64(1);
        writer.Put32(1);
        writer.Put32(0);
        writer.Put64(64);
        writer.Put32(R::MaxRangesPerBlob + 1);
    }
    R::MemoryBlob decoded_blob;
    REQUIRE(R::Decode(blob, decoded_blob) == R::ReadError::LimitExceeded);

    // Function count of a schema.
    Bytes schema;
    {
        R::ByteWriter writer{schema};
        writer.Put64(1);
        writer.Put32(0);
        writer.Put32(0);
        writer.Put32(64);
        writer.Put32(0);
        writer.Put32(0);  // modules
        writer.Put32(0);  // types
        writer.Put32(R::MaxFunctions + 1);
    }
    R::LibrarySchema decoded_schema;
    REQUIRE(R::Decode(schema, decoded_schema) == R::ReadError::LimitExceeded);

    // A call record whose length field lies.
    for (const u32 length : {u32{4}, R::MaxRecordBytes + 1}) {
        Bytes record;
        R::ByteWriter writer{record};
        writer.Put32(1);
        writer.Put32(length);
        writer.PutZero(80);
        REQUIRE(R::Decode(record, records) == R::ReadError::LimitExceeded);
    }

    // A payload over the chunk limit is refused before any parsing.
    const Bytes oversize(R::MaxChunkBytes + 1, 0);
    REQUIRE(R::Decode(oversize, session) == R::ReadError::LimitExceeded);
    REQUIRE(R::Decode(oversize, maps) == R::ReadError::LimitExceeded);
}

// An inline extent is checked without adding its offset to its length: the sum of a hostile
// length wraps to a small number.
TEST_CASE("Replay codec: an inline extent is bounded by the inline area", "[video_core][replay][codec]") {
    const auto decode = [](u32 inline_offset, u64 length, std::vector<u8> inline_data) {
        R::Extent extent;
        extent.role = R::ExtentRole::InBytes;
        extent.length = length;
        extent.inline_offset = inline_offset;
        R::CallRecordView view;
        view.call_seq = 1;
        view.function_id = 1;
        view.thread_id = 1;
        view.frame_id = 10;
        view.extents = std::span<const R::Extent>{&extent, 1};
        view.inline_data = inline_data;
        Bytes body;
        REQUIRE(R::Encode(view, body));
        Bytes payload;
        R::ByteWriter writer{payload};
        writer.Put32(1);
        payload.insert(payload.end(), body.begin(), body.end());
        std::vector<R::CallRecord> records;
        return R::Decode(payload, records);
    };
    REQUIRE(decode(0, 0, {}) == R::ReadError::None);
    REQUIRE(decode(0, 4, {1, 2, 3, 4}) == R::ReadError::None);
    REQUIRE(decode(4, 0, {1, 2, 3, 4}) == R::ReadError::None);
    REQUIRE(decode(1, 4, {1, 2, 3, 4}) == R::ReadError::BadPayload);
    REQUIRE(decode(5, 0, {1, 2, 3, 4}) == R::ReadError::BadPayload);
    REQUIRE(decode(1, ~u64{0}, {}) == R::ReadError::BadPayload);
    REQUIRE(decode(2, ~u64{0} - 1, {1, 2, 3, 4}) == R::ReadError::BadPayload);
}

TEST_CASE("Replay codec: reserved fields, ranges and ids are checked", "[video_core][replay][codec]") {
    // Reserved fields must be zero.
    {
        Bytes bytes = Enc(MakeSession());
        bytes[8] = 1;  // reserved word of the first pin
        R::SessionRecord back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::BadPayload);
    }
    {
        Bytes bytes = Enc(MakeComplete());
        bytes.back() = 1;
        R::CompleteRecord back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::BadPayload);
    }
    {
        Bytes bytes = Enc(MakeSchema());
        bytes[20] = 1;  // the reserved word after pointer_bits
        R::LibrarySchema back;
        REQUIRE(R::Decode(bytes, back) == R::ReadError::BadPayload);
    }
    {
        R::LibrarySchema schema = MakeSchema();
        schema.pointer_bits = 32;
        R::LibrarySchema back;
        REQUIRE(R::Decode(Enc(schema), back) == R::ReadError::BadPayload);
    }
    {
        // The split address of an address space cannot lie beyond the end of the space.
        R::GpuCheckpoint checkpoint = MakeCheckpoint();
        checkpoint.address_spaces[0].split_address = (u64{1} << 40) + 1;
        R::GpuCheckpoint back;
        REQUIRE(R::Decode(Enc(checkpoint), back) == R::ReadError::BadPayload);
        checkpoint.address_spaces[0].split_address = u64{1} << 40;
        REQUIRE(R::Decode(Enc(checkpoint), back) == R::ReadError::None);
        REQUIRE(back.address_spaces[0].split_address == (u64{1} << 40));
        checkpoint.address_spaces[0].address_space_bits = 0;
        R::GpuCheckpoint none;
        REQUIRE(R::Decode(Enc(checkpoint), none) == R::ReadError::BadPayload);
    }
    {
        // Section, state and class numbers outside the ones that exist are refused.
        R::GpuCheckpoint back;
        R::GpuCheckpoint checkpoint = MakeCheckpoint();
        checkpoint.macros[0].kind = static_cast<R::MacroSectionKind>(8);
        REQUIRE(R::Decode(Enc(checkpoint), back) == R::ReadError::BadPayload);
        checkpoint = MakeCheckpoint();
        checkpoint.states[0].kind = static_cast<R::EngineStateKind>(4);
        REQUIRE(R::Decode(Enc(checkpoint), back) == R::ReadError::BadPayload);
        checkpoint = MakeCheckpoint();
        checkpoint.engines[1].engine_class = static_cast<R::EngineClass>(7);
        REQUIRE(R::Decode(Enc(checkpoint), back) == R::ReadError::BadPayload);
    }

    // Memory ranges: sorted, not overlapping, inside the allocation, kind consistent.
    const auto zero_range = [](u64 offset, u64 length) {
        R::MemoryRange range;
        range.offset = offset;
        range.length = length;
        range.kind = R::RangeKind::Zero;
        return range;
    };
    const auto blob_with = [&](std::vector<R::MemoryRange> ranges) {
        R::MemoryBlob blob;
        blob.alloc_id = 1;
        blob.generation = 1;
        blob.allocation_size = 64;
        blob.ranges = std::move(ranges);
        return blob;
    };
    const auto decode = [](const R::MemoryBlob& blob) {
        R::MemoryBlob back;
        return R::Decode(Enc(blob), back);
    };
    REQUIRE(decode(blob_with({zero_range(0, 16), zero_range(16, 16)})) == R::ReadError::None);
    REQUIRE(decode(blob_with({zero_range(16, 16), zero_range(0, 16)})) == R::ReadError::BadPayload);
    REQUIRE(decode(blob_with({zero_range(0, 16), zero_range(8, 16)})) == R::ReadError::BadPayload);
    REQUIRE(decode(blob_with({zero_range(56, 16)})) == R::ReadError::BadPayload);
    REQUIRE(decode(blob_with({zero_range(0, 0)})) == R::ReadError::BadPayload);
    REQUIRE(decode(blob_with({zero_range(~u64{0} - 3, 16)})) == R::ReadError::BadPayload);
    {
        R::MemoryRange ref = zero_range(0, 8);
        ref.kind = R::RangeKind::Ref;  // ref_sequence stays 0
        REQUIRE(decode(blob_with({ref})) == R::ReadError::BadPayload);
        R::MemoryRange stray = zero_range(0, 8);
        stray.ref_sequence = 3;  // a non-Ref range with a reference
        REQUIRE(decode(blob_with({stray})) == R::ReadError::BadPayload);
    }
    {
        R::MemoryRange derived = zero_range(0, 8);
        derived.kind = R::RangeKind::Data;
        derived.writer = R::WriterKind::GpuDerived;
        R::MemoryBlob blob = blob_with({derived});
        blob.data.assign(8, 1);
        REQUIRE(decode(blob) == R::ReadError::BadPayload);
    }
    {
        R::MemoryBlob blob = blob_with({zero_range(0, 8)});
        blob.alloc_id = 0;
        REQUIRE(decode(blob) == R::ReadError::BadPayload);
        blob = blob_with({zero_range(0, 8)});
        blob.flags = R::BlobGpuDerived;  // GPU-derived allocations hold no Zero range
        REQUIRE(decode(blob) == R::ReadError::BadPayload);
    }

    // Mappings: a nonzero size, no overflow, a backing allocation unless sparse or reserved.
    {
        std::vector<R::MapEntry> maps = MakeMaps();
        std::vector<R::MapEntry> back;
        maps[0].size = 0;
        REQUIRE(R::Decode(EncMaps(maps), back) == R::ReadError::BadPayload);
        maps = MakeMaps();
        maps[0].gpu_va = ~u64{0};
        REQUIRE(R::Decode(EncMaps(maps), back) == R::ReadError::BadPayload);
        maps = MakeMaps();
        maps[0].alloc_id = 0;
        REQUIRE(R::Decode(EncMaps(maps), back) == R::ReadError::BadPayload);
        maps[0].flags = R::MapSparse;
        REQUIRE(R::Decode(EncMaps(maps), back) == R::ReadError::None);
    }

    // A picture whose pixels do not match their checksum is refused.
    {
        R::EndPicture picture = MakePicture(10);
        picture.pixel_checksum ^= 1;
        R::EndPicture back;
        REQUIRE(R::Decode(Enc(picture), back) == R::ReadError::BadChunkChecksum);
    }
}

TEST_CASE("Replay codec: both headers refuse a cut at any byte", "[video_core][replay][codec]") {
    TempDir dir;
    const Built built = Build(dir, "headers", LibraryMode::None);
    const Bytes file = ReadFile(built.path);
    REQUIRE(file.size() > R::FileHeaderBytes + R::ChunkHeaderBytes);

    for (size_t length = 0; length < R::FileHeaderBytes; ++length) {
        R::FileHeader header;
        INFO(length);
        REQUIRE(R::DecodeFileHeader(std::span<const u8>{file.data(), length}, header) ==
                R::ReadError::Truncated);
    }
    for (size_t length = 0; length < R::ChunkHeaderBytes; ++length) {
        R::ChunkHeader header;
        INFO(length);
        REQUIRE(R::DecodeChunkHeader(std::span<const u8>{file.data() + R::FileHeaderBytes, length},
                                     header) == R::ReadError::Truncated);
    }
}

// ---- files ------------------------------------------------------------------------------------------

TEST_CASE("Replay codec: a captured file reads back and validates", "[video_core][replay][codec]") {
    TempDir dir;
    const Built built = Build(dir, "plain", LibraryMode::None);
    REQUIRE(fs::exists(built.path));
    REQUIRE_FALSE(fs::exists(fs::path{built.path.string() + ".partial"}));

    R::CaptureReader reader;
    std::string detail;
    REQUIRE(OpenAndValidate(reader, built.path, detail) == R::ReadError::None);
    REQUIRE(reader.Header().first_frame == 10);
    REQUIRE(reader.Header().finalized_frames == 2);
    REQUIRE((reader.Header().flags & R::HeaderComplete) != 0);
    REQUIRE(reader.Header().file_length == fs::file_size(built.path));
    REQUIRE(reader.Complete().total_frames == 2);
    REQUIRE(reader.Complete().total_chunks == reader.Header().chunk_count);
    REQUIRE(reader.Complete().total_bytes == reader.Header().file_length);
    REQUIRE(reader.Index().frames.size() == 2);
    REQUIRE(reader.Index().frames[0].frame_id == 10);
    REQUIRE(reader.Index().frames[1].frame_id == 11);
    REQUIRE(reader.Index().entries.size() + 2 == reader.Header().chunk_count);
    REQUIRE_FALSE(reader.LibraryClosure().ready);

    // Next yields every chunk once, in sequence order, ends at COMPLETE and restarts on Rewind.
    for (int pass = 0; pass < 2; ++pass) {
        reader.Rewind();
        u64 count = 0;
        R::ChunkType last = R::ChunkType::Session;
        for (;;) {
            R::ChunkView view;
            bool at_end = false;
            REQUIRE(reader.Next(view, at_end, detail) == R::ReadError::None);
            if (at_end) {
                break;
            }
            REQUIRE(view.header.sequence == count);
            REQUIRE(view.header.stored_length == view.payload.size());
            last = view.type;
            ++count;
        }
        REQUIRE(count == reader.Header().chunk_count);
        REQUIRE(last == R::ChunkType::Complete);
    }
}

TEST_CASE("Replay codec: a file that ends early is reported with the offset",
          "[video_core][replay][codec]") {
    TempDir dir;
    const Built built = Build(dir, "short", LibraryMode::None);
    const Bytes file = ReadFile(built.path);
    const fs::path cut_path = dir.File("cut.egcap");

    // Every length inside both headers, then a spread over the rest of the file.
    std::vector<size_t> lengths;
    for (size_t length = 0; length <= R::FileHeaderBytes + R::ChunkHeaderBytes + 8; ++length) {
        lengths.push_back(length);
    }
    for (size_t length = lengths.back(); length < file.size(); length += 53) {
        lengths.push_back(length);
    }
    lengths.push_back(file.size() - 1);
    for (const size_t length : lengths) {
        WriteFile(cut_path, Prefix(file, length));
        R::CaptureReader reader;
        std::string detail;
        INFO(length);
        REQUIRE(reader.Open(cut_path, detail) == R::ReadError::Truncated);
        REQUIRE(detail.find(std::to_string(length)) != std::string::npos);
    }
}

TEST_CASE("Replay codec: a damaged file is refused with the right reason",
          "[video_core][replay][codec]") {
    TempDir dir;
    const Built built = Build(dir, "damage", LibraryMode::None);
    const Bytes original = ReadFile(built.path);
    const fs::path path = dir.File("damaged.egcap");
    R::CaptureReader probe;
    std::string detail;
    REQUIRE(probe.Open(built.path, detail) == R::ReadError::None);
    const u64 map_offset = OffsetOf(probe, R::ChunkType::Map);
    const u64 fence_offset = OffsetOf(probe, R::ChunkType::Fence);
    const u64 index_offset = IndexChunkOffset(probe);
    const u64 complete_offset = CompleteChunkOffset(original);

    const auto validate = [&](const Bytes& file) {
        WriteFile(path, file);
        R::CaptureReader reader;
        std::string reason;
        const R::ReadError error = OpenAndValidate(reader, path, reason);
        INFO(reason);
        return error;
    };
    REQUIRE(validate(original) == R::ReadError::None);

    SECTION("a flipped payload byte") {
        Bytes file = original;
        file[static_cast<size_t>(map_offset + R::ChunkHeaderBytes + 1)] ^= 0x01;
        REQUIRE(validate(file) == R::ReadError::BadChunkChecksum);
        R::CaptureReader reader;
        REQUIRE(reader.Open(path, detail) == R::ReadError::None);
        REQUIRE(reader.Validate(detail) == R::ReadError::BadChunkChecksum);
        REQUIRE(detail.find(std::to_string(map_offset + R::ChunkHeaderBytes)) != std::string::npos);
        R::ChunkView view;
        bool at_end = false;
        REQUIRE(reader.Next(view, at_end, detail) == R::ReadError::NotComplete);
    }
    SECTION("a flipped chunk header byte") {
        Bytes file = original;
        file[static_cast<size_t>(map_offset + 10)] ^= 0x01;
        REQUIRE(validate(file) == R::ReadError::BadChunkChecksum);
    }
    SECTION("a broken chain digest") {
        Bytes file = original;
        R::CompleteRecord complete;
        const R::ChunkHeader header = HeaderAt(file, complete_offset);
        const Bytes payload(file.begin() + static_cast<std::ptrdiff_t>(complete_offset + 64),
                            file.begin() + static_cast<std::ptrdiff_t>(complete_offset + 64 +
                                                                       header.stored_length));
        REQUIRE(R::Decode(payload, complete) == R::ReadError::None);
        complete.chain_digest ^= 1;
        RewriteChunk(file, complete_offset, Enc(complete));
        REQUIRE(validate(file) == R::ReadError::BadDigest);
    }
    SECTION("an index that disagrees with the walk") {
        Bytes file = original;
        const R::ChunkHeader header = HeaderAt(file, index_offset);
        const Bytes payload(file.begin() + static_cast<std::ptrdiff_t>(index_offset + 64),
                            file.begin() + static_cast<std::ptrdiff_t>(index_offset + 64 +
                                                                       header.stored_length));
        R::IndexRecord index;
        REQUIRE(R::Decode(payload, index) == R::ReadError::None);
        index.entries[3].stored_length += 1;
        RewriteChunk(file, index_offset, Enc(index));
        REQUIRE(validate(file) == R::ReadError::BadIndex);

        index.entries[3].stored_length -= 1;
        index.frames[1].output_id ^= 1;
        RewriteChunk(file, index_offset, Enc(index));
        REQUIRE(validate(file) == R::ReadError::BadIndex);
    }
    SECTION("a Ref to bytes after the coverage of the blob it names") {
        // The earlier blob defines [0, 32). A Ref at 40 lies after it: subtracting the offset
        // from the interval end wrapped and accepted the range.
        Bytes file = original;
        const u64 delta_offset = OffsetOf(probe, R::ChunkType::MemoryBlob, 1);
        const R::ChunkHeader header = HeaderAt(file, delta_offset);
        const Bytes payload(
            file.begin() + static_cast<std::ptrdiff_t>(delta_offset + R::ChunkHeaderBytes),
            file.begin() + static_cast<std::ptrdiff_t>(delta_offset + R::ChunkHeaderBytes +
                                                       header.stored_length));
        R::MemoryBlob blob;
        REQUIRE(R::Decode(payload, blob) == R::ReadError::None);
        REQUIRE(blob.ranges.size() == 2);
        REQUIRE(blob.ranges[0].kind == R::RangeKind::Ref);
        blob.ranges[0].offset = 40;
        blob.ranges[0].length = 4;
        blob.ranges[1].offset = 48;
        RewriteChunk(file, delta_offset, Enc(blob));
        REQUIRE(validate(file) == R::ReadError::DanglingId);
    }
    SECTION("a sequence that skips") {
        Bytes file = original;
        R::ChunkHeader header = HeaderAt(file, map_offset);
        header.sequence += 1;
        RewriteHeader(file, map_offset, header);
        REQUIRE(validate(file) == R::ReadError::BadOrdering);
    }
    SECTION("a chunk of the wrong frame") {
        Bytes file = original;
        R::ChunkHeader header = HeaderAt(file, map_offset);
        header.frame_id = 11;
        RewriteHeader(file, map_offset, header);
        REQUIRE(validate(file) == R::ReadError::BadOrdering);
    }
    SECTION("an unknown required chunk type") {
        Bytes file = original;
        R::ChunkHeader header = HeaderAt(file, fence_offset);
        header.type = 999;
        RewriteHeader(file, fence_offset, header);
        REQUIRE(validate(file) == R::ReadError::UnsupportedFeature);
    }
    SECTION("an unknown optional chunk type is skipped") {
        Bytes file = original;
        R::ChunkHeader header = HeaderAt(file, fence_offset);
        header.type = 999;
        header.flags = R::ChunkOptional;
        RewriteHeader(file, fence_offset, header);
        // The walk does not stumble on it; only the index, which lists the old type, disagrees.
        REQUIRE(validate(file) == R::ReadError::BadIndex);
        R::CaptureReader reader;
        REQUIRE(reader.Open(path, detail) == R::ReadError::None);
        REQUIRE(reader.Validate(detail) == R::ReadError::BadIndex);
        R::ChunkView view;
        bool at_end = false;
        REQUIRE(reader.Next(view, at_end, detail) == R::ReadError::NotComplete);
    }
    SECTION("an oversize chunk length") {
        Bytes file = original;
        R::ChunkHeader header = HeaderAt(file, map_offset);
        Bytes head(file.begin() + static_cast<std::ptrdiff_t>(map_offset),
                   file.begin() + static_cast<std::ptrdiff_t>(map_offset + 56));
        const u64 oversize = R::MaxChunkBytes + 1;
        for (size_t i = 0; i < 8; ++i) {
            head[24 + i] = static_cast<u8>(oversize >> (8 * i));
            head[32 + i] = static_cast<u8>(oversize >> (8 * i));
        }
        const u64 checksum = R::Fnv1a64(head);
        for (size_t i = 0; i < 8; ++i) {
            head.push_back(static_cast<u8>(checksum >> (8 * i)));
        }
        std::copy(head.begin(), head.end(), file.begin() + static_cast<std::ptrdiff_t>(map_offset));
        REQUIRE(header.stored_length < oversize);
        REQUIRE(validate(file) == R::ReadError::LimitExceeded);
    }
    SECTION("a chunk length past the end of the file") {
        Bytes file = original;
        R::ChunkHeader header = HeaderAt(file, map_offset);
        Bytes head(file.begin() + static_cast<std::ptrdiff_t>(map_offset),
                   file.begin() + static_cast<std::ptrdiff_t>(map_offset + 56));
        const u64 length = file.size();
        for (size_t i = 0; i < 8; ++i) {
            head[24 + i] = static_cast<u8>(length >> (8 * i));
            head[32 + i] = static_cast<u8>(length >> (8 * i));
        }
        const u64 checksum = R::Fnv1a64(head);
        for (size_t i = 0; i < 8; ++i) {
            head.push_back(static_cast<u8>(checksum >> (8 * i)));
        }
        std::copy(head.begin(), head.end(), file.begin() + static_cast<std::ptrdiff_t>(map_offset));
        REQUIRE(header.stored_length < length);
        REQUIRE(validate(file) == R::ReadError::Truncated);
    }
    SECTION("an unknown required feature") {
        Bytes file = original;
        R::FileHeader header = ParseHeader(file);
        header.required_features |= 1ULL << 40;
        WriteHeaderBytes(file, header);
        REQUIRE(validate(file) == R::ReadError::UnsupportedFeature);
    }
    SECTION("a header without the Complete flag") {
        Bytes file = original;
        R::FileHeader header = ParseHeader(file);
        header.flags &= ~static_cast<u32>(R::HeaderComplete);
        WriteHeaderBytes(file, header);
        REQUIRE(validate(file) == R::ReadError::NotComplete);
    }
    SECTION("a header length that disagrees with the file") {
        Bytes file = original;
        file.push_back(0);
        REQUIRE(validate(file) == R::ReadError::BadLength);
    }
    SECTION("a flipped header byte") {
        Bytes file = original;
        file[100] ^= 0x01;
        REQUIRE(validate(file) == R::ReadError::BadHeaderChecksum);
    }
    SECTION("a wrong magic") {
        Bytes file = original;
        file[0] ^= 0x01;
        REQUIRE(validate(file) == R::ReadError::BadMagic);
    }
}

TEST_CASE("Replay codec: the library section must close for replay readiness",
          "[video_core][replay][codec]") {
    TempDir dir;
    std::string detail;

    SECTION("a closed section is ready") {
        const Built built = Build(dir, "lib_clean", LibraryMode::Clean);
        R::CaptureReader reader;
        REQUIRE(OpenAndValidate(reader, built.path, detail) == R::ReadError::None);
        REQUIRE((reader.Header().flags & R::HeaderHasLibrarySection) != 0);
        REQUIRE((reader.Header().flags & R::HeaderLibraryReplayReady) != 0);
        REQUIRE(reader.LibraryClosure().ready);
        REQUIRE(reader.LibraryClosure().unresolved_calls == 0);
        REQUIRE(reader.LibraryClosure().dangling_references == 0);
        REQUIRE(reader.LibraryClosure().unresolved_links == 0);
        REQUIRE(reader.LibraryClosure().unsupported_schemas == 0);
    }
    SECTION("a call without its return") {
        const Built built = Build(dir, "lib_open", LibraryMode::OpenCall);
        R::CaptureReader reader;
        REQUIRE(OpenAndValidate(reader, built.path, detail) == R::ReadError::None);
        REQUIRE_FALSE(reader.LibraryClosure().ready);
        REQUIRE(reader.LibraryClosure().unresolved_calls == 1);
        REQUIRE((reader.Header().flags & R::HeaderLibraryReplayReady) == 0);
    }
    SECTION("an object nothing created") {
        const Built built = Build(dir, "lib_dangling", LibraryMode::DanglingObject);
        R::CaptureReader reader;
        REQUIRE(OpenAndValidate(reader, built.path, detail) == R::ReadError::None);
        REQUIRE_FALSE(reader.LibraryClosure().ready);
        REQUIRE(reader.LibraryClosure().dangling_references >= 1);
    }
    SECTION("a link to a submit that does not exist") {
        const Built built = Build(dir, "lib_link", LibraryMode::BadLink);
        R::CaptureReader reader;
        REQUIRE(OpenAndValidate(reader, built.path, detail) == R::ReadError::None);
        REQUIRE_FALSE(reader.LibraryClosure().ready);
        REQUIRE(reader.LibraryClosure().unresolved_links == 1);
    }
    SECTION("a producer that claims readiness the closure disproves") {
        const Built built = Build(dir, "lib_claim", LibraryMode::ClaimsReady);
        R::CaptureReader reader;
        REQUIRE(reader.Open(built.path, detail) == R::ReadError::None);
        REQUIRE(reader.Validate(detail) == R::ReadError::BadIndex);
        REQUIRE_FALSE(reader.LibraryClosure().ready);
    }
    SECTION("a capture without a library section is never ready") {
        const Built built = Build(dir, "lib_none", LibraryMode::None);
        R::CaptureReader reader;
        REQUIRE(OpenAndValidate(reader, built.path, detail) == R::ReadError::None);
        REQUIRE_FALSE(reader.LibraryClosure().ready);
        REQUIRE((reader.Header().flags & R::HeaderHasLibrarySection) == 0);
    }
}

// ---- the writer -------------------------------------------------------------------------------------

TEST_CASE("Replay codec: the writer works on a partial file and never replaces a capture",
          "[video_core][replay][codec]") {
    TempDir dir;
    const fs::path final_path = dir.File("race.egcap");
    const fs::path partial_path = dir.File("race.egcap.partial");
    const R::FileHeader header = MakeHeader(false);
    std::string error;

    SECTION("the partial name exists until the clean end") {
        R::CaptureWriter writer;
        REQUIRE(writer.Open(final_path, header, error));
        REQUIRE(fs::exists(partial_path));
        REQUIRE_FALSE(fs::exists(final_path));
        writer.Abandon();
        REQUIRE(fs::exists(partial_path));
        REQUIRE_FALSE(fs::exists(final_path));
    }
    SECTION("a file that exists is not replaced when the writer opens") {
        WriteFile(final_path, Bytes{1, 2, 3});
        R::CaptureWriter writer;
        REQUIRE_FALSE(writer.Open(final_path, header, error));
        REQUIRE_FALSE(error.empty());
        REQUIRE(ReadFile(final_path) == Bytes({1, 2, 3}));
        REQUIRE_FALSE(fs::exists(partial_path));
    }
    SECTION("a partial file that exists is not reused") {
        WriteFile(partial_path, Bytes{4, 5});
        R::CaptureWriter writer;
        REQUIRE_FALSE(writer.Open(final_path, header, error));
        REQUIRE(ReadFile(partial_path) == Bytes({4, 5}));
    }
    SECTION("a second writer to a name a running capture holds is refused and the first file stays") {
        R::CaptureWriter first;
        REQUIRE(first.Open(final_path, header, error));
        const Bytes before = ReadFile(partial_path);
        R::CaptureWriter second;
        std::string second_error;
        REQUIRE_FALSE(second.Open(final_path, header, second_error));
        REQUIRE(second_error.rfind("cannot take the name lock ", 0) == 0);
        REQUIRE(ReadFile(partial_path) == before);
        first.Abandon();
    }
    SECTION("the partial of an abandoned capture is replaced once its lock is released") {
        R::CaptureWriter first;
        REQUIRE(first.Open(final_path, header, error));
        first.Abandon();
        {
            std::ofstream append{partial_path, std::ios::binary | std::ios::app};
            append << "tail";
        }
        const Bytes stale = ReadFile(partial_path);
        R::CaptureWriter second;
        REQUIRE(second.Open(final_path, header, error));
        const Bytes fresh = ReadFile(partial_path);
        REQUIRE(fresh.size() + 4 == stale.size());
        second.Abandon();
        REQUIRE_FALSE(fs::exists(final_path.string() + ".lock"));
    }
    SECTION("a file that appears before the rename is not replaced") {
        // The race this closes: another file takes the final name while the capture runs.
        const Built built = Build(dir, "other", LibraryMode::None);
        REQUIRE(fs::exists(built.path));
        R::CaptureWriter writer;
        REQUIRE(writer.Open(final_path, header, error));
        Add(writer, R::ChunkType::Session, 0, Enc(MakeSession()));
        // The checkpoint belongs to the first stored frame, which is frame 10 here as in Build.
        const u64 first = writer.NextSequence();
        Add(writer, R::ChunkType::GpuCheckpoint, 10, Enc(MakeCheckpoint()));
        Add(writer, R::ChunkType::MemoryBlob, 10, Enc(MakeBlob()));
        Add(writer, R::ChunkType::Present, 10, Enc(MakePresent(10)));
        Add(writer, R::ChunkType::EndPicture, 10, Enc(MakePicture(10)));
        Add(writer, R::ChunkType::FrameEnd, 10, Enc(MakeFrameEnd(10, first, writer.NextSequence())));
        Add(writer, R::ChunkType::MemoryBlob, 11, Enc(MakeBlob()));
        Add(writer, R::ChunkType::Present, 11, Enc(MakePresent(11)));
        Add(writer, R::ChunkType::EndPicture, 11, Enc(MakePicture(11)));
        Add(writer, R::ChunkType::FrameEnd, 11,
            Enc(MakeFrameEnd(11, first + 5, writer.NextSequence())));
        WriteFile(final_path, Bytes{9, 9, 9});
        R::CompleteRecord complete;
        complete.status = R::CompleteStatus::Complete;
        complete.dependency_complete = 1;
        REQUIRE_FALSE(writer.Finalize(header, complete, error));
        REQUIRE_FALSE(error.empty());
        REQUIRE(ReadFile(final_path) == Bytes({9, 9, 9}));
        REQUIRE(fs::exists(partial_path));
    }
    SECTION("a clean end renames and leaves no partial file") {
        const Built built = Build(dir, "clean", LibraryMode::None);
        REQUIRE(fs::exists(built.path));
        REQUIRE_FALSE(fs::exists(fs::path{built.path.string() + ".partial"}));
    }
    SECTION("an incomplete capture stays partial and is not readable as a capture") {
        const Built built = Build(dir, "broken", LibraryMode::None, false);
        const fs::path partial = fs::path{built.path.string() + ".partial"};
        REQUIRE_FALSE(fs::exists(built.path));
        REQUIRE(fs::exists(partial));
        R::CaptureReader reader;
        std::string detail;
        REQUIRE(reader.Open(partial, detail) == R::ReadError::NotComplete);
    }
    SECTION("a path without the capture extension is refused") {
        R::CaptureWriter writer;
        REQUIRE_FALSE(writer.Open(dir.File("race.bin"), header, error));
        REQUIRE_FALSE(error.empty());
    }
}

TEST_CASE("Replay codec: the writer enforces its limits and the frame order",
          "[video_core][replay][codec]") {
    TempDir dir;
    const R::FileHeader header = MakeHeader(false);
    std::string error;

    SECTION("a payload over the chunk limit fails the writer") {
        R::CaptureWriter writer;
        REQUIRE(writer.Open(dir.File("big.egcap"), header, error));
        Add(writer, R::ChunkType::Session, 0, Enc(MakeSession()));
        const Bytes oversize(R::MaxChunkBytes + 1, 0);
        REQUIRE_FALSE(writer.Append(R::ChunkType::MemoryBlob, 10, oversize, error));
        REQUIRE(writer.Failed());
        REQUIRE(writer.ChunkCount() == 1);
        REQUIRE_FALSE(writer.Append(R::ChunkType::Unmap, 10, Enc(MakeSession()), error));
        REQUIRE_FALSE(writer.Finalize(header, MakeComplete(), error));
    }
    SECTION("INDEX and COMPLETE come only from Finalize") {
        R::CaptureWriter writer;
        REQUIRE(writer.Open(dir.File("typed.egcap"), header, error));
        REQUIRE_FALSE(writer.Append(R::ChunkType::Index, 0, Enc(MakeIndex()), error));
        REQUIRE(writer.Failed());
    }
    SECTION("frame ids are gapless and a frame must end before the next starts") {
        R::CaptureWriter gap;
        REQUIRE(gap.Open(dir.File("gap.egcap"), header, error));
        Add(gap, R::ChunkType::Session, 0, Enc(MakeSession()));
        Add(gap, R::ChunkType::MemoryBlob, 10, Enc(MakeBlob()));
        REQUIRE_FALSE(gap.Append(R::ChunkType::MemoryBlob, 11, Enc(MakeBlob()), error));
        REQUIRE(gap.Failed());

        R::CaptureWriter backwards;
        REQUIRE(backwards.Open(dir.File("back.egcap"), header, error));
        Add(backwards, R::ChunkType::Session, 0, Enc(MakeSession()));
        Add(backwards, R::ChunkType::MemoryBlob, 10, Enc(MakeBlob()));
        Add(backwards, R::ChunkType::FrameEnd, 10, Enc(MakeFrameEnd(10, 1, 2)));
        REQUIRE_FALSE(backwards.Append(R::ChunkType::MemoryBlob, 10, Enc(MakeBlob()), error));
        REQUIRE(backwards.Failed());
    }
    SECTION("a frameless chunk with a frame id and a framed chunk without one are refused") {
        R::CaptureWriter frameless;
        REQUIRE(frameless.Open(dir.File("frameless.egcap"), header, error));
        REQUIRE_FALSE(frameless.Append(R::ChunkType::Session, 10, Enc(MakeSession()), error));

        R::CaptureWriter framed;
        REQUIRE(framed.Open(dir.File("framed.egcap"), header, error));
        REQUIRE_FALSE(framed.Append(R::ChunkType::MemoryBlob, 0, Enc(MakeBlob()), error));

        R::CaptureWriter no_frame;
        REQUIRE(no_frame.Open(dir.File("noframe.egcap"), header, error));
        REQUIRE_FALSE(no_frame.Append(R::ChunkType::GpuCheckpoint, 0, Enc(MakeCheckpoint()), error));
    }
    SECTION("an unfinished frame or wrong frame counts stop Finalize") {
        R::CaptureWriter open_frame;
        REQUIRE(open_frame.Open(dir.File("open.egcap"), header, error));
        Add(open_frame, R::ChunkType::Session, 0, Enc(MakeSession()));
        Add(open_frame, R::ChunkType::MemoryBlob, 10, Enc(MakeBlob()));
        REQUIRE_FALSE(open_frame.Finalize(header, MakeComplete(), error));
        REQUIRE_FALSE(fs::exists(dir.File("open.egcap")));

        R::CaptureWriter counts;
        REQUIRE(counts.Open(dir.File("counts.egcap"), header, error));
        Add(counts, R::ChunkType::Session, 0, Enc(MakeSession()));
        Add(counts, R::ChunkType::MemoryBlob, 10, Enc(MakeBlob()));
        Add(counts, R::ChunkType::FrameEnd, 10, Enc(MakeFrameEnd(10, 1, 2)));
        // The header claims two frames, one was written.
        REQUIRE_FALSE(counts.Finalize(header, MakeComplete(), error));
        REQUIRE_FALSE(fs::exists(dir.File("counts.egcap")));
    }
}

TEST_CASE("Replay codec: the writer's counters follow the file", "[video_core][replay][codec]") {
    TempDir dir;
    const Built built = Build(dir, "counters", LibraryMode::None);
    R::CaptureReader reader;
    std::string detail;
    REQUIRE(OpenAndValidate(reader, built.path, detail) == R::ReadError::None);

    // Replaying the same chunks through a second writer gives the same chain digest and size.
    R::CaptureWriter writer;
    std::string error;
    REQUIRE(writer.Open(dir.File("again.egcap"), built.header, error));
    REQUIRE(writer.NextSequence() == 0);
    REQUIRE(writer.BytesWritten() == R::FileHeaderBytes);
    reader.Rewind();
    for (;;) {
        R::ChunkView view;
        bool at_end = false;
        REQUIRE(reader.Next(view, at_end, detail) == R::ReadError::None);
        if (at_end) {
            break;
        }
        if (view.type == R::ChunkType::Index || view.type == R::ChunkType::Complete) {
            continue;
        }
        REQUIRE(writer.Append(view.type, view.header.frame_id, view.payload, error));
    }
    REQUIRE(writer.ChunkCount() + 2 == reader.Header().chunk_count);
    REQUIRE(writer.ChainDigest() == reader.Complete().chain_digest);
    REQUIRE_FALSE(writer.Failed());
    writer.Abandon();
}

TEST_CASE("Uncaptured mapped bytes refuse an otherwise complete stream before replay",
          "[video_core][replay][memory]") {
    TempDir dir;
    const auto built = Build(dir, "absent", LibraryMode::None, true, 33);
    R::CaptureReader reader;
    std::string detail;
    REQUIRE(reader.Open(built.path, detail) == R::ReadError::None);
    R::ChunkView chunk;
    bool end = false;
    REQUIRE(reader.Next(chunk, end, detail) == R::ReadError::NotComplete);
    REQUIRE(reader.Validate(detail) == R::ReadError::BadMapping);
    REQUIRE(reader.Next(chunk, end, detail) == R::ReadError::NotComplete);
}

TEST_CASE("PRESENT proves the tiled framebuffer span before exposing any chunk", "[replay][memory]") {
    CHECK(R::PresentBackingBytes(128, 65, 128, 1) == 65536);
    CHECK(R::PresentBackingBytes(128, 65, 128, 4) == 32768);
    CHECK_FALSE(R::PresentBackingBytes(128, 65, 127, 1));
    CHECK_FALSE(R::PresentBackingBytes(128, 65, 128, 0));
    TempDir dir;
    const auto built = Build(dir, "short-present", LibraryMode::None, true, 32, 36 * 1024);
    R::CaptureReader reader;
    std::string detail;
    REQUIRE(reader.Open(built.path, detail) == R::ReadError::None);
    REQUIRE(reader.Validate(detail) == R::ReadError::BadMapping);
    CHECK(detail.find("PRESENT") != std::string::npos);
    R::ChunkView chunk;
    bool end = false;
    CHECK(reader.Next(chunk, end, detail) == R::ReadError::NotComplete);
}
