// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen: the file-level writer and reader of the capture format (format.h, events.h,
// library_call.h). Slice B implements format_io.cpp; the capture (slice B) uses the writer, the
// replay session (slice C) and the lab-facing validator use the reader.
//
// Writer. Streams chunks to "<final>.partial" and never reallocates the whole file in memory.
//   Open      creates the .partial file exclusively (an existing .partial or final file is an
//             error), writes the provisional header (flags 0, counts 0).
//   Append    frames one payload: assigns the sequence (strictly +1), the Optional flag of known
//             optional types, the frame id check (non-decreasing, frameless types 0), the chunk and
//             payload checksums; rejects a payload over MaxChunkBytes and any append that would
//             exceed MaxFileBytes or MaxChunkCount, leaving the writer failed and the file
//             incomplete. Chunks other than INDEX and COMPLETE extend the chain digest.
//   Finalize  appends INDEX and COMPLETE, rewrites the header (flags including Complete, counts,
//             index_offset, file_length, header checksum), flushes to disk, closes, and renames
//             "<final>.partial" to "<final>" without replacing an existing file. A failure
//             anywhere leaves the .partial file in place and returns false.
//   Abandon   closes without finalizing; the file stays ".partial" and incomplete.
//
// Reader. Treats the file as hostile (format.h REJECTION list).
//   Open      reads and validates the header, then (for a finalized file) INDEX and COMPLETE,
//             before anything else; a header without the Complete flag, or a COMPLETE that is not
//             CompleteIsClean, returns NotComplete.
//   Validate  a full sweep: every chunk header and payload checksum, sequence, frame ordering and
//             the grammar of format.h, every id resolving to an earlier definition, the INDEX
//             against the chunks found, the chain digest, and the library closure when the
//             library section exists. It bounds its memory by MaxStagingBytes and decodes each
//             payload once.
//   Next      yields chunks in sequence order for a replayer; the payload checksum is verified,
//             unknown optional chunks are skipped, a decode of the payload is the caller's job.

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/replay/events.h"
#include "video_core/replay/format.h"
#include "video_core/replay/library_call.h"

namespace VideoCore::Replay {

class CaptureWriter {
public:
    CaptureWriter();
    ~CaptureWriter();
    CaptureWriter(const CaptureWriter&) = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;

    [[nodiscard]] bool Open(const std::filesystem::path& final_path, const FileHeader& header,
                            std::string& error);
    [[nodiscard]] bool Append(ChunkType type, u64 frame_id, std::span<const u8> payload,
                              std::string& error);
    // The writer owns what it alone knows and writes it over the caller's values: in `complete`
    // chain_digest, total_frames, total_chunks and total_bytes; in the header index_offset,
    // chunk_count, file_length, and the flags Complete, DependencyComplete and LibraryReplayReady
    // (taken from `complete`) and HasLibrarySection (set when a LIBRARY_SCHEMA chunk was
    // appended). The caller supplies the rest, and the frame counts, which are checked.
    [[nodiscard]] bool Finalize(const FileHeader& header, const CompleteRecord& complete,
                                std::string& error);
    void Abandon() noexcept;

    [[nodiscard]] u64 NextSequence() const noexcept;
    [[nodiscard]] u64 BytesWritten() const noexcept;
    [[nodiscard]] u64 ChunkCount() const noexcept;
    [[nodiscard]] u64 ChainDigest() const noexcept;
    [[nodiscard]] bool Failed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct ChunkView {
    ChunkHeader header;
    ChunkType type = ChunkType::Session;
    std::vector<u8> payload;
};

class CaptureReader {
public:
    CaptureReader();
    ~CaptureReader();
    CaptureReader(const CaptureReader&) = delete;
    CaptureReader& operator=(const CaptureReader&) = delete;

    [[nodiscard]] ReadError Open(const std::filesystem::path& path, std::string& detail);
    [[nodiscard]] ReadError Validate(std::string& detail);
    // Restarts Next at sequence 0.
    void Rewind();
    // Refuses to deliver any chunks until Validate succeeds. Sets `at_end` instead of returning a chunk after COMPLETE.
    [[nodiscard]] ReadError Next(ChunkView& out, bool& at_end, std::string& detail);

    [[nodiscard]] const FileHeader& Header() const noexcept;
    [[nodiscard]] const IndexRecord& Index() const noexcept;
    [[nodiscard]] const CompleteRecord& Complete() const noexcept;
    // Valid after a successful Validate.
    [[nodiscard]] const ClosureResult& LibraryClosure() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore::Replay
