# Replay stream contract

C wire decision for D/F/G: EGLCAP major 2, FRAME_END adds u64 call_count and u32 effect_count (independent live totals, not gpu_words or the final-value outputs vector). Sidecar ordinal is zero-based in recorded order; (frame_id, ordinal, kind) identities must be unique and reproduced in order. COMPLETE.status=Complete with zero reasons means requested window completed; incomplete bits carry final refusal reasons. D retains ordered sidecars, not donor sorting/coalescing. F include new replay/memory_restore.h instead of donor RangeWriteLog/ApplyBlobToBytes definitions in replay_internal.h. C requires AGY #5 for clean-cut predicates; validation with #5 applied in a test-only commit.

All integers are little endian. `format.h` specifies header fields, feature bits, chunk grammar and hard limits. No native structs, pointers or inferred sizes on disk. The 256-byte file header leads to sequenced, checksummed chunks (64-byte headers). Session and GPU checkpoint precede events; backed allocation generations and their defined ranges precede each MAP and PRESENT. Undefined bytes are not zero. F must open **and validate** the complete file before setting up the GPU; use the checked memory restore before installing mappings. Failure is not a shorter successful replay.

Each recorded present closes exactly one logical frame (including marked preroll). FRAME_END: u64 frame, u32 flags, u32 semantic output count, u64 GPU words, u64 first/last chunk sequence, u64 output id, u64 call_count, u32 effect_count, then 32-byte semantic output entries. The latter are end-state values, not an ordered event log. Do not convert counts to floating point. D takes both counts from the live frame, independently of F. G reconciles each frame and rejects gaps, extra, duplicate, reordered or mismatched payload/outcome, including deletion from both sidecars. A genuinely unobservable outcome needs an explicit reason, not a fabricated match.

INDEX and COMPLETE close the file. Never qualify a partial file, an abnormal stop or a nonzero incompleteness reason/counter. Only publish the final name after an intact, flushed tail; never replace an existing capture. Limits and unsupported required features fail closed. Format 1 frames lack independent counts and are rejected, not silently upgraded. Baseline settings hashes are independent of this file version.

The staging sink is in `staging_writer.cpp`; slice D must **not** port its original definitions or anonymous staging helpers from `capture.cpp`.

A live cut holds the GPU between lists and excludes guest channel allocation, binding,
initialization and mapping publication until the initial snapshot has been staged. The engine
and mapping walks use the same ID-ordered, owning channel snapshot; each initialized channel
keeps its address space alive. Arm capture before GPU start; late attachment is refused.

PRESENT backing is the block-linear source the raw renderer would read, not the composed
picture or a tightly packed `stride * height` image. `PresentBackingBytes` checks dimensions
and format, then aligns row bytes to a 64-byte GOB and height to 128 rows. RGBA8888, RGBX8888,
BGRA8888 and RGBA1010102 use four bytes per pixel; RGB565 uses two. Recorder, validator and
replayer must require the entire range from `alloc_offset + framebuffer_offset`.

PRESENT carries at most one guest syncpoint threshold. The recorder refuses a fence list
or threshold it cannot reproduce. It records a guest raise before publishing the mirrored
page or waking a compositor. Ready compositions drain in submission order; the selected
picture belongs to that composition's present ID. Preroll effects remain in the ordered
live ledger and count toward their own FRAME_END, but require no live picture. Capture
requires the observed VSync mode to be Immediate; it does not change the user's profile.
