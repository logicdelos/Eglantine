// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen contract of the library section of the capture file: the call stream of the graphics
// library the guest links, recorded next to the GPU events of the same frames. The section is
// optional in the file (every chunk of it carries the Optional flag, so the GPU-only replay of
// 0.6 skips it) and mandatory for the later native-executor milestones. Slice A freezes the
// layouts and the sink and source interfaces here; slice D (call census, library capture) is the
// producer, slice B writes and reads the chunks, nothing in 0.6 replays the calls.
//
// Why paired. The call stream answers "what did the guest ask for" and the GPU events answer
// "what did the driver path do"; both share allocation ids, frame ids and submit/present links,
// so a later backend can be validated call by call against the GPU events of the same capture
// (the paired existing-path oracle of the native-path plan).
//
// Everything here is generic. A function is identified by a dense function_id of the schema the
// producer discovered from the library's own call surface; no function, type or module name is
// built into this header or into any consumer, and no per-title behaviour exists.
//
// LIBRARY_SCHEMA payload (frameless, before the first library record)
//   u64 abi_hash, u32 api_word0, u32 api_word1, u32 pointer_bits (64), u32 reserved (0),
//   u32 module_count (<= 256), module_count * {u32 module_id, u32 reserved, u64 identity_hash},
//   u32 type_count (<= 4096), type_count * {u32 type_tag, name},
//   u32 function_count (<= MaxFunctions), function_count * FunctionSchema
//   name        = u32 length (<= MaxNameBytes) then that many ASCII bytes
//   FunctionSchema = u32 function_id (dense from 1), u32 flags (SchemaFlag), u64 signature_hash,
//     name, ArgDesc result, u32 arg_count (<= MaxArgs), arg_count * ArgDesc
//   ArgDesc (12 bytes) = u8 kind (ArgKind, 0 = none), u8 width, u16 flags (ArgDescFlag),
//     u32 type_tag, u32 reserved (0)
//
// CALL RECORD (inside LIBRARY_EVENT and LIBRARY_CHECKPOINT payloads, which are u32 record_count
// followed by records), 80-byte header then variable parts
//    0   4  record_length        whole record including this field, <= MaxRecordBytes
//    4   2  kind                 CallKind
//    6   2  flags                RecordFlag
//    8   8  call_seq             dense from 1 across the capture, assigned in call order
//   16   4  function_id          must exist in the schema
//   20   4  thread_id            dense logical thread index, never an OS id
//   24   8  frame_id             the accepted-present frame the call belongs to
//   32   4  link_kind            LinkKind
//   36   4  arg_count            <= MaxArgs
//   40   8  link_id              submit_id / present_id / fence id of the link, 0 for None
//   48   4  extent_count         <= MaxExtentsPerCall
//   52   4  inline_bytes         bytes of inline data at the end of the record
//   56  24  result               ArgValue (kind 0 when the call returns nothing or is a Call)
//   80      arg_count * ArgValue (24 bytes each)
//           extent_count * Extent (48 bytes each)
//           inline_bytes bytes
// A Call record lists every argument at call time. The matching Return record (same call_seq)
// lists only the output and in/out arguments of the schema with their values after the call, the
// result, and the extents whose role is OutputBytes. A call and its return may be separated by
// callback records and by records of other threads. A call that never returns before the capture
// ends is an open call; it counts as unresolved and makes library_replay_ready false.
//
// ArgValue (24 bytes): u8 kind, u8 width, u16 flags, u32 aux, u64 value, u64 value2
//   Scalar          value = the integer (width 1/2/4/8, ArgFlag signed)
//   Float           value = IEEE bit pattern (width 4 or 8)
//   ObjectRef       value = object id (dense from 1, assigned by the producer), value2 =
//                   generation, aux = type_tag
//   ExtentRef       value = captured pointer identity (never dereferenced, never used to find
//                   data), aux = index into the record's extents
//   PointerIdentity value = captured pointer identity only; no data is captured
//   CallbackRef     value = offset inside the module, value2 = module_id (schema module table)
//
// Extent (48 bytes): u32 role (ExtentRole), u32 arg_index, u64 length, u64 alloc_id,
//   u64 offset, u32 generation, u32 inline_offset, u32 flags (ExtentFlag), u32 reserved
//   InBytes    bytes the function reads, copied at call time: alloc_id 0, data at inline_offset
//   OutBytes   bytes the function writes; inline data only when ExtentExternalProducer (the value
//              came from outside the graphics path); otherwise length only (inline_offset
//              NoInlineData)
//   PoolRange  a range of a captured allocation (alloc_id, generation, offset, length); no data
//
// Object lifetime. A function schema flagged CreatesObject defines the ObjectRef values of its
// output arguments at its Return; one flagged DestroysObject ends the ObjectRef values of its
// input arguments at its Call. A generation increases when a guest address is reused after the
// producer saw the earlier object destroyed. The closure check (below) requires every ObjectRef
// and PoolRange to resolve to an earlier definition that is alive with the same generation.
//
// LIBRARY_CHECKPOINT. A capture that starts mid-run holds, instead of the whole prehistory, a
// compact list of records in the same encoding (flag RecordFromCheckpoint) that re-create every
// object alive at the first recorded frame, in dependency order, equivalent to the startup calls
// that built them. The producer supplies them through LibraryCallSource::EmitCheckpoint. A
// checkpoint the producer cannot make complete is reported (CheckpointComplete == false) and
// keeps library_replay_ready false; it never invalidates the GPU replay.

#pragma once

#include <array>
#include <atomic>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/replay/format.h"

namespace VideoCore::Replay {

inline constexpr u32 MaxFunctions = 4096;
inline constexpr u32 MaxArgs = 32;
inline constexpr u32 MaxExtentsPerCall = 32;
inline constexpr u32 MaxModules = 256;
inline constexpr u32 MaxTypes = 4096;
inline constexpr u32 MaxRecordBytes = 16U << 20;
inline constexpr u32 MaxRecordsPerChunk = 65536;
inline constexpr u32 CallHeaderBytes = 80;
inline constexpr u32 ArgValueBytes = 24;
inline constexpr u32 ExtentBytes = 48;
inline constexpr u32 ArgDescBytes = 12;
inline constexpr u32 NoInlineData = 0xFFFFFFFFu;

enum class ArgKind : u8 {
    None = 0,
    Scalar = 1,
    Float = 2,
    ObjectRef = 3,
    ExtentRef = 4,
    PointerIdentity = 5,
    CallbackRef = 6,
};

enum ArgDescFlag : u16 {
    ArgDescSigned = 1U << 0,
    ArgDescOut = 1U << 1,    // the function writes it
    ArgDescIn = 1U << 2,     // the function reads it; both bits = in/out
};

enum SchemaFlag : u32 {
    SchemaComplete = 1U << 0,       // every argument is described; required for replay readiness
    SchemaReturnsValue = 1U << 1,
    SchemaMayBlock = 1U << 2,
    SchemaCallback = 1U << 3,       // the guest provides the function, the library calls it
    SchemaCreatesObject = 1U << 4,
    SchemaDestroysObject = 1U << 5,
};

enum class CallKind : u16 { Call = 1, Return = 2, CallbackEnter = 3, CallbackReturn = 4 };

enum RecordFlag : u16 {
    RecordFromCheckpoint = 1U << 0,
    RecordBlocking = 1U << 1,
    RecordExternalResult = 1U << 2,  // the result came from outside the graphics path
};

enum class LinkKind : u32 { None = 0, Submit = 1, Present = 2, Fence = 3 };

enum class ExtentRole : u32 { InBytes = 1, OutBytes = 2, PoolRange = 3 };

enum ExtentFlag : u32 {
    ExtentExternalProducer = 1U << 0,
    ExtentDataOmitted = 1U << 1,  // the bound forbade the copy: makes the call unsupported
};

enum ArgFlag : u16 { ArgSigned = 1U << 0 };

struct ArgDesc {
    ArgKind kind = ArgKind::None;
    u8 width = 0;
    u16 flags = 0;
    u32 type_tag = 0;
};

struct FunctionSchema {
    u32 function_id = 0;
    u32 flags = 0;
    u64 signature_hash = 0;
    std::string name;
    ArgDesc result;
    std::vector<ArgDesc> args;
};

struct ModuleEntry {
    u32 module_id = 0;
    u64 identity_hash = 0;
};

struct TypeEntry {
    u32 type_tag = 0;
    std::string name;
};

struct LibrarySchema {
    u64 abi_hash = 0;
    u32 api_word0 = 0;
    u32 api_word1 = 0;
    u32 pointer_bits = 64;
    std::vector<ModuleEntry> modules;
    std::vector<TypeEntry> types;
    std::vector<FunctionSchema> functions;
};

struct ArgValue {
    ArgKind kind = ArgKind::None;
    u8 width = 0;
    u16 flags = 0;
    u32 aux = 0;
    u64 value = 0;
    u64 value2 = 0;
};

struct Extent {
    ExtentRole role = ExtentRole::InBytes;
    u32 arg_index = 0;
    u64 length = 0;
    u64 alloc_id = 0;
    u64 offset = 0;
    u32 generation = 0;
    u32 inline_offset = NoInlineData;
    u32 flags = 0;
};

// What a producer hands to the sink. Spans are valid only for the duration of the call; the sink
// copies what it keeps.
struct CallRecordView {
    CallKind kind = CallKind::Call;
    u16 flags = 0;
    u64 call_seq = 0;      // 0: the sink assigns it from the ticket
    u32 function_id = 0;
    u32 thread_id = 0;
    u64 frame_id = 0;      // 0: the sink uses the frame of the ticket
    LinkKind link_kind = LinkKind::None;
    u64 link_id = 0;
    ArgValue result;
    std::span<const ArgValue> args;
    std::span<const Extent> extents;
    std::span<const u8> inline_data;
};

// A call record as decoded from a file (owning).
struct CallRecord {
    CallKind kind = CallKind::Call;
    u16 flags = 0;
    u64 call_seq = 0;
    u32 function_id = 0;
    u32 thread_id = 0;
    u64 frame_id = 0;
    LinkKind link_kind = LinkKind::None;
    u64 link_id = 0;
    ArgValue result;
    std::vector<ArgValue> args;
    std::vector<Extent> extents;
    std::vector<u8> inline_data;
};

void Encode(const LibrarySchema& value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, LibrarySchema& out);
// Appends one record in the encoding above; returns false when it would exceed MaxRecordBytes
// (the sink then counts the call as dropped, which makes the capture incomplete).
[[nodiscard]] bool Encode(const CallRecordView& value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, std::vector<CallRecord>& out);

// Closure check run by the reader on a whole capture: every record's function_id exists, every
// schema entry the records use is SchemaComplete, no extent has ExtentDataOmitted, every
// ObjectRef and PoolRange resolves to an earlier definition alive with the same generation,
// every Call has its Return, and every record's link resolves to a Submit or Present event of the
// GPU section of the same frame. Result is the library_replay_ready flag.
struct ClosureResult {
    bool ready = false;
    u32 unsupported_schemas = 0;
    u32 unresolved_calls = 0;
    u32 dangling_references = 0;
    u32 unresolved_links = 0;
};

// ---- producer interface (implemented by slice D, consumed by slice B) ---------------------------------

struct CheckpointStatus {
    bool complete = false;
    u64 records = 0;
};

// Receives checkpoint records in dependency order. Implemented by the capture writer.
class RecordSink {
public:
    virtual ~RecordSink() = default;
    [[nodiscard]] virtual bool Append(const CallRecordView& record) = 0;
};

struct LibraryCoverage {
    u32 unsupported_schemas = 0;   // functions used by the capture whose schema is not complete
    u32 unresolved_calls = 0;      // calls with no return at the end of the capture
    u64 dropped_records = 0;       // records the producer could not hand to the sink
    bool checkpoint_complete = false;
};

class LibraryCallSource {
public:
    virtual ~LibraryCallSource() = default;
    // Called once when a capture begins: the schema of the functions the producer instruments.
    [[nodiscard]] virtual bool BuildSchema(LibrarySchema& out, std::string& error) = 0;
    // Called once at the first recorded frame: the object-creating records alive at that moment.
    // It runs on the GPU thread while the GPU is held quiescent, with no capture lock held. It
    // must not wait for the GPU (a fence, an idle wait, a flush) and must not take a lock that a
    // thread holds while it waits for the GPU; it may take its own locks and call the sink.
    [[nodiscard]] virtual CheckpointStatus EmitCheckpoint(RecordSink& sink) = 0;
    // Called when the capture is finalized.
    [[nodiscard]] virtual LibraryCoverage Coverage() = 0;
};

// The producer registers itself when its census starts and clears the registration (nullptr) when
// it stops. At most one source exists; the capture holds no ownership. Implemented by slice B.
void RegisterLibrarySource(LibraryCallSource* source) noexcept;
[[nodiscard]] LibraryCallSource* RegisteredLibrarySource() noexcept;

// ---- consumer interface (implemented by slice B, called from slice D's probes) -----------------------------

namespace detail {
// Set by BeginCapture / EndCapture of the capture (capture.h). Header-inline so a probe tests it
// with one relaxed load and no link dependency.
inline std::atomic<bool> g_library_capture_active{false};
} // namespace detail

[[nodiscard]] inline bool LibraryCaptureActive() noexcept {
    return detail::g_library_capture_active.load(std::memory_order_relaxed);
}

struct CallTicket {
    u64 call_seq = 0;
    u64 frame_id = 0;  // the frame the call belongs to at this instant (open frame id)
};

// Both functions are wait-free for the calling guest thread up to a bounded copy into a
// per-thread staging area; they never block on I/O and never allocate on the hot path past the
// staging area's first use. Hand over a ticket at the Call and pass it to the Return's record.
[[nodiscard]] CallTicket NextCallTicket() noexcept;
// Returns false when the record was dropped (bound hit); the capture is then incomplete.
bool RecordLibraryCall(const CallTicket& ticket, const CallRecordView& record) noexcept;

} // namespace VideoCore::Replay
