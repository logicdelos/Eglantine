// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// What the capture recorder needs from the running system, as one seam. capture.cpp implements it
// over Core::System; the tests implement it with a fake, so the recorder itself (state machine,
// ordering, staging, fences, semantic outputs, the file) runs without a GPU device or a guest.

#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/replay/capture.h"
#include "video_core/replay/checkpoint.h"
#include "video_core/replay/events.h"
#include "video_core/replay/memory_capture.h"

namespace VideoCore::Replay {

// One run of the GPU address space as the memory manager holds it at the cut.
struct CutMapping {
    u32 space = 0;        // MemoryManager::GetID()
    u64 gpu_addr = 0;
    u64 size = 0;
    u64 device = 0;       // device address of the first byte, unused when sparse
    u32 kind = 0;         // PTE kind
    bool big_pages = false;
    bool sparse = false;
};

// What one run of a cut body found.
enum class CutOutcome { Done, Retry, Failed };

// Everything the checkpoint cut gathers from the system. `pins` are the settings values of the
// SESSION chunk. A pin the host could not read is not in `pins`: its key is in `unknown_pins`, and
// the SESSION chunk carries it with the value SessionPinUnknown, which a replay refuses.
// `title_version` is the version of the running title when the host can supply it.
struct CutState {
    GpuCheckpoint checkpoint;
    std::vector<CutMapping> mappings;
    DeviceIdentity device;
    std::vector<SessionPin> pins;
    std::vector<u32> unknown_pins;
    bool title_version_known = false;
    u32 title_version = 0;
    // Set by a refused Cut when what it found may clear by itself (a channel waiting on a
    // semaphore, a command list not yet finished, a macro half way); the recorder then lets the
    // host cut again at the next quiescent point instead of giving up.
    bool retry_later = false;
};

class CaptureHost {
public:
    virtual ~CaptureHost() = default;

    // Guest and device memory, read at the moment of the call.
    [[nodiscard]] virtual MemorySource& Memory() = 0;

    // Gathers the state at the cut. Called only from inside RunQuiescent's body. False with a
    // message when the state cannot be captured cleanly; `out.retry_later` says whether a later
    // quiescent point may succeed.
    [[nodiscard]] virtual bool Cut(u64 present_id, CutState& out, std::string& error) = 0;

    // Runs `body` while the GPU consumes nothing: between two command lists of the GPU thread (every
    // list submitted before the call has run, none submitted after it has started), with every
    // engine idle and the GPU thread held until `body` returns. A GPU still busy at the call is
    // waited for, not refused. `body` returns Retry when a precondition of a clean cut is not met
    // yet; the host then runs it again at the next quiescent point until its time is up. With
    // `present_id` 0 nothing has run yet and `body` runs on the calling thread. False with a
    // message (the last Retry reason and how long was waited, or the Failed message) when the
    // outcome is not Done.
    [[nodiscard]] virtual bool RunQuiescent(
        u64 present_id, const std::function<CutOutcome(std::string&)>& body,
        std::string& error) = 0;

    // Pins (delta +1) or releases (delta -1) the cached state of device pages so that guest CPU
    // writes to them reach NoteGuestWrite.
    virtual void PinPages(std::span<const std::pair<u64, size_t>> runs, s32 delta) = 0;

    // The picture the emulator displayed for accepted present `present_id`, after its composite
    // finished. False when it is unavailable.
    [[nodiscard]] virtual bool ReadComposedPicture(u64 present_id, EndPicture& out) = 0;
};

// BeginCapture over a host other than the running system (the tests). Same contract as
// BeginCapture; the recorder owns the host until EndCapture.
[[nodiscard]] bool BeginCaptureWithHost(std::unique_ptr<CaptureHost> host, const CaptureConfig& config,
                                        std::string& error);

} // namespace VideoCore::Replay
