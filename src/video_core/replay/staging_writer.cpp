// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <utility>

#include "video_core/replay/memory_capture.h"

namespace VideoCore::Replay {

namespace {
// Bytes kept free in the file for INDEX and COMPLETE when a chunk is accepted: the chunk headers,
// one index entry per chunk, one frame entry per possible frame and slack.
[[nodiscard]] u64 FinalReserveBytes(u64 chunks) noexcept {
    return 2 * ChunkHeaderBytes + 4096 + (chunks + 1) * IndexEntryBytes +
           u64{MaxTotalFrames} * FrameEntryBytes;
}

constexpr auto StageWaitTimeout = std::chrono::seconds(120);
} // namespace

StagingWriter::StagingWriter(const SessionLimits& limits_) : limits{limits_} {}

StagingWriter::~StagingWriter() {
    Stop();
    if (opened && !finished) {
        writer.Abandon();
    }
}

bool StagingWriter::Open(const std::string& final_path, const FileHeader& header,
                         std::string& error) {
    if (opened) {
        error = "capture sink already open";
        return false;
    }
    if (!writer.Open(std::filesystem::path{final_path}, header, error)) {
        return false;
    }
    next_sequence = writer.NextSequence();
    opened = true;
    thread = std::thread([this] { Run(); });
    return true;
}

void StagingWriter::Run() {
    for (;;) {
        Item item;
        bool skip = false;
        {
            std::unique_lock lock{mutex};
            work.wait(lock, [this] { return !queue.empty() || stopping; });
            if (queue.empty()) {
                return;
            }
            item = std::move(queue.front());
            queue.pop_front();
            skip = stats.write_failed;
        }
        std::string error;
        bool ok = true;
        if (!skip) {
            ok = writer.Append(item.type, item.frame_id, item.payload, error);
        }
        {
            std::lock_guard lock{mutex};
            queued_bytes -= item.payload.size();
            if (!skip && ok) {
                ++stats.chunks_written;
                written_chunks.store(stats.chunks_written, std::memory_order_relaxed);
                written_bytes.store(writer.BytesWritten(), std::memory_order_relaxed);
            } else if (!skip) {
                stats.write_failed = true;
                if (stats.error.empty()) {
                    stats.error = error.empty() ? std::string{"write failed"} : error;
                }
            }
        }
        room.notify_all();
    }
}

void StagingWriter::Stop() {
    {
        std::lock_guard lock{mutex};
        stopping = true;
    }
    work.notify_all();
    room.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
}

bool StagingWriter::Stage(ChunkType type, u64 frame_id, std::vector<u8>&& payload, bool may_wait) {
    std::unique_lock lock{mutex};
    const u64 size = payload.size();
    const auto drop = [&](const char* why) {
        ++stats.dropped_chunks;
        ++stats.overflow_events;
        if (stats.error.empty()) {
            stats.error = why;
        }
        return false;
    };
    if (!opened || stopping || stats.write_failed) {
        ++stats.dropped_chunks;
        return false;
    }
    if (size > limits.max_chunk_bytes || size > MaxChunkBytes) {
        return drop("a chunk exceeds max_chunk_bytes");
    }
    if (stats.chunks_staged + 2 >= MaxChunkCount) {
        return drop("the chunk count limit was reached");
    }
    if (reserved_bytes + ChunkHeaderBytes + size + FinalReserveBytes(stats.chunks_staged) >
        limits.max_file_bytes) {
        return drop("max_file_bytes was reached");
    }
    if (size > limits.max_staging_bytes) {
        return drop("a chunk exceeds max_staging_bytes");
    }
    if (queued_bytes + size > limits.max_staging_bytes) {
        if (!may_wait) {
            return drop("max_staging_bytes was reached");
        }
        const bool room_made = room.wait_for(lock, StageWaitTimeout, [&] {
            return queued_bytes + size <= limits.max_staging_bytes || stats.write_failed || stopping;
        });
        if (!room_made || stats.write_failed || stopping) {
            return drop("the writer did not make room in the staging queue");
        }
    }
    queued_bytes += size;
    stats.peak_queued_bytes = (std::max)(stats.peak_queued_bytes, queued_bytes);
    stats.bytes_staged += size;
    ++stats.chunks_staged;
    reserved_bytes += ChunkHeaderBytes + size;
    ++next_sequence;
    queue.push_back(Item{type, frame_id, std::move(payload)});
    lock.unlock();
    work.notify_one();
    return true;
}

u64 StagingWriter::NextSequence() const {
    std::lock_guard lock{mutex};
    return next_sequence;
}

bool StagingWriter::Healthy() const {
    std::lock_guard lock{mutex};
    return opened && stats.dropped_chunks == 0 && !stats.write_failed;
}

StagingStats StagingWriter::Stats() const {
    std::lock_guard lock{mutex};
    return stats;
}

u64 StagingWriter::BytesWritten() const {
    return written_bytes.load(std::memory_order_relaxed);
}

u64 StagingWriter::ChunkCount() const {
    return written_chunks.load(std::memory_order_relaxed);
}

bool StagingWriter::Finish(bool finalize, const FileHeader& header, CompleteRecord& complete,
                           std::string& error) {
    Stop();
    if (!opened || finished) {
        return false;
    }
    finished = true;
    StagingStats snapshot;
    {
        std::lock_guard lock{mutex};
        snapshot = stats;
    }
    const auto add_saturated = [](u32& counter, u64 amount) {
        const u64 sum = u64{counter} + amount;
        counter = static_cast<u32>((std::min)(sum, u64{std::numeric_limits<u32>::max()}));
    };
    if (snapshot.dropped_chunks != 0) {
        add_saturated(complete.dropped_events, snapshot.dropped_chunks);
        complete.incomplete_reasons |= IncompleteDroppedEvents;
    }
    if (snapshot.overflow_events != 0) {
        add_saturated(complete.overflow_events, snapshot.overflow_events);
        complete.incomplete_reasons |= IncompleteLimitExceeded;
    }
    if (snapshot.write_failed || writer.Failed()) {
        complete.incomplete_reasons |= IncompleteWriteFailed;
    }
    if (complete.incomplete_reasons != 0) {
        complete.status = CompleteStatus::Incomplete;
    }
    if (finalize && complete.incomplete_reasons == 0 && !writer.Failed()) {
        std::string finalize_error;
        const bool finalized = writer.Finalize(header, complete, finalize_error);
        // INDEX and COMPLETE are written here, not by the worker: the counts of the file are the
        // writer's, including whatever tail a failed finalization did write.
        written_chunks.store(writer.ChunkCount(), std::memory_order_relaxed);
        written_bytes.store(writer.BytesWritten(), std::memory_order_relaxed);
        if (finalized) {
            return true;
        }
        if (writer.Failed()) {
            // The queued chunks all landed; the tail (INDEX, COMPLETE, header flush or rename)
            // did not. That is a write failure like any other.
            complete.incomplete_reasons |= IncompleteWriteFailed;
            complete.status = CompleteStatus::Incomplete;
        }
        error = finalize_error;
        return false;
    }
    writer.Abandon();
    if (error.empty()) {
        error = snapshot.error;
    }
    return false;
}

} // namespace VideoCore::Replay
