// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Decisions the replay session and the runner share, inline so a test can call them without
// pulling replay_session.cpp (that file references the capture reader, which another slice
// provides). The session calls these functions; a bug here fails a test and fails the replay.

#pragma once

#include <array>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "common/run_outcome.h"
#include "common/settings_enums.h"
#include "video_core/replay/memory_restore.h"
#include "video_core/replay/result.h"
#include "video_core/replay/format.h"
#include "video_core/replay/options.h"
#include "video_core/replay/replay_session.h"

namespace VideoCore::Replay {

inline constexpr u32 IdleBoundMs = 30000;
inline constexpr u32 SubmitHeaderSizeBits = 21;
inline constexpr u32 SubmitHeaderMaxWords = (1U << SubmitHeaderSizeBits) - 1U;

// ---- capture prefix ------------------------------------------------------------------------

struct PrefixVerdict {
    ReadError error = ReadError::None;
    std::string detail;
};

// Refuses a provisional file, a short head, a wrong magic and anything DecodeFileHeader refuses
// (wrong version, bad checksum, unknown required feature, limit). Open calls this on the bytes
// it read itself, before the reader, so a hostile file never reaches a decoder that allocates.
[[nodiscard]] inline PrefixVerdict ClassifyCapturePrefix(std::string_view path,
                                                         std::span<const u8> head) {
    PrefixVerdict verdict;
    constexpr std::string_view partial = ".partial";
    if (path.size() >= partial.size() &&
        path.substr(path.size() - partial.size()) == partial) {
        verdict.error = ReadError::NotComplete;
        verdict.detail = "refusing a .partial capture";
        return verdict;
    }
    if (path.empty()) {
        verdict.error = ReadError::Truncated;
        verdict.detail = "capture path is empty";
        return verdict;
    }
    if (head.size() < FileHeaderBytes) {
        verdict.error = ReadError::Truncated;
        verdict.detail = "capture is shorter than the file header";
        return verdict;
    }
    if (std::memcmp(head.data(), Magic.data(), Magic.size()) != 0) {
        verdict.error = ReadError::BadMagic;
        verdict.detail = "capture magic does not match";
        return verdict;
    }
    FileHeader header;
    verdict.error = DecodeFileHeader(head, header);
    if (verdict.error != ReadError::None) {
        verdict.detail = std::string(ReadErrorName(verdict.error));
    }
    return verdict;
}

// A finalized capture is replayable only when the COMPLETE record is clean. Open calls this
// after the reader validates, and Run calls it again when the COMPLETE chunk is reached.
[[nodiscard]] inline ReadError RejectUnclean(const CompleteRecord& record, std::string& detail) {
    if (!CompleteIsClean(record)) {
        detail = "capture is not a clean complete recording";
        return ReadError::NotComplete;
    }
    detail.clear();
    return ReadError::None;
}

// ---- chunk sequence ------------------------------------------------------------------------

struct SequenceCheck {
    u64 previous = 0;
    bool started = false;
};

// Payload checksum, then a contiguous sequence. Run calls this on every chunk the reader yields.
[[nodiscard]] inline ReadError RejectDecodedChunk(SequenceCheck& state, const ChunkHeader& header,
                                                  std::span<const u8> payload,
                                                  std::string& detail) {
    const ReadError payload_error = VerifyPayload(header, payload);
    if (payload_error != ReadError::None) {
        detail = "chunk payload checksum does not match";
        return payload_error;
    }
    if (state.started && header.sequence != state.previous + 1) {
        detail = "chunk sequence is not contiguous";
        return ReadError::BadOrdering;
    }
    state.started = true;
    state.previous = header.sequence;
    return ReadError::None;
}

// The raw framing check tests use, and that a caller can apply to bytes that never reached the
// reader. Truncation, a bad header checksum, version 0 and a compressed flag are refused.
[[nodiscard]] inline ReadError RejectChunkBytes(std::span<const u8> header_bytes,
                                                std::span<const u8> payload, std::string& detail) {
    ChunkHeader header;
    const ReadError framed = DecodeChunkHeader(header_bytes, header);
    if (framed != ReadError::None) {
        detail = std::string(ReadErrorName(framed));
        return framed;
    }
    SequenceCheck state;
    return RejectDecodedChunk(state, header, payload, detail);
}

// ---- pins ----------------------------------------------------------------------------------

struct PinDecision {
    bool ok = false;
    std::string error;
    bool has_width = false;
    u32 width = 0;
    bool has_height = false;
    u32 height = 0;
    bool has_docked = false;
    u32 docked = 0;
    bool has_accuracy = false;
    u32 accuracy = 0;
    bool has_resolution = false;
    u32 resolution = 0;
    bool has_anti_aliasing = false;
    u32 anti_aliasing = 0;
    bool has_scaling = false;
    u32 scaling = 0;
    bool has_astc = false;
    u32 astc = 0;
    bool has_layer_mask = false;
    u32 layer_mask = 0;
};

// A pin the build cannot honour refuses the capture. Absent pins are not an error: the session
// forces the deterministic defaults (Vulkan, vsync off, async off) either way. AsyncGpu other
// than 0 cannot be honoured, because replay disables asynchronous GPU emulation.
[[nodiscard]] inline PinDecision InterpretPins(std::span<const SessionPin> pins) {
    PinDecision decision;
    decision.ok = true;
    std::array<bool, 16> seen{};
    for (const SessionPin& pin : pins) {
        if (pin.key == 0 || pin.key >= seen.size()) {
            decision.ok = false;
            decision.error = "unknown settings pin";
            return decision;
        }
        if (seen[pin.key]) {
            decision.ok = false;
            decision.error = "duplicate settings pin";
            return decision;
        }
        seen[pin.key] = true;
        const u64 value = pin.value;
        if (value == SessionPinUnknown) {
            decision.ok = false;
            decision.error = "the capture could not read the setting of pin " +
                             std::to_string(pin.key) + ", so the replay cannot honour it";
            return decision;
        }
        switch (static_cast<SettingKey>(pin.key)) {
        case SettingKey::Backend:
            if (value != 1) {
                decision.ok = false;
                decision.error = "only the Vulkan backend is honoured";
            }
            break;
        case SettingKey::FrameWidth:
            if (value == 0 || value > MaxPictureDimension) {
                decision.ok = false;
                decision.error = "frame width is outside 1..8192";
            } else {
                decision.has_width = true;
                decision.width = static_cast<u32>(value);
            }
            break;
        case SettingKey::FrameHeight:
            if (value == 0 || value > MaxPictureDimension) {
                decision.ok = false;
                decision.error = "frame height is outside 1..8192";
            } else {
                decision.has_height = true;
                decision.height = static_cast<u32>(value);
            }
            break;
        case SettingKey::Docked:
            if (value > 1) {
                decision.ok = false;
                decision.error = "docked pin is not 0 or 1";
            } else {
                decision.has_docked = true;
                decision.docked = static_cast<u32>(value);
            }
            break;
        case SettingKey::GpuAccuracy:
            if (value > 1) {
                decision.ok = false;
                decision.error = "gpu accuracy pin is outside the build's range";
            } else {
                decision.has_accuracy = true;
                decision.accuracy = static_cast<u32>(value);
            }
            break;
        case SettingKey::ResolutionSetup:
            if (value > 12) {
                decision.ok = false;
                decision.error = "resolution setup pin is outside 0..12";
            } else {
                decision.has_resolution = true;
                decision.resolution = static_cast<u32>(value);
            }
            break;
        case SettingKey::AntiAliasing:
            if (value > 2) {
                decision.ok = false;
                decision.error = "anti-aliasing pin is outside 0..2";
            } else {
                decision.has_anti_aliasing = true;
                decision.anti_aliasing = static_cast<u32>(value);
            }
            break;
        case SettingKey::ScalingFilter:
            if (value > 16) {
                decision.ok = false;
                decision.error = "scaling filter pin is outside 0..16";
            } else {
                decision.has_scaling = true;
                decision.scaling = static_cast<u32>(value);
            }
            break;
        case SettingKey::AsyncGpu:
            if (value != 0) {
                decision.ok = false;
                decision.error = "asynchronous gpu emulation cannot be honoured";
            }
            break;
        case SettingKey::ShaderBackend:
            if (value != 0) {
                decision.ok = false;
                decision.error = "only the default Vulkan shader backend is honoured";
            }
            break;
        case SettingKey::NativeTiling:
            if (value != 1) {
                decision.ok = false;
                decision.error = "native tiling off is not honoured";
            }
            break;
        case SettingKey::TuringMacros:
            if (value != 1) {
                decision.ok = false;
                decision.error = "Turing macros off is not honoured";
            }
            break;
        case SettingKey::VSync:
            if (value != 0) {
                decision.ok = false;
                decision.error = "vsync other than 0 is not honoured";
            }
            break;
        case SettingKey::AstcRecompression:
            if (value > 2) {
                decision.ok = false;
                decision.error = "astc recompression pin is outside 0..2";
            } else {
                decision.has_astc = true;
                decision.astc = static_cast<u32>(value);
            }
            break;
        case SettingKey::OutputLayerMask:
            if (value != 0) {
                decision.ok = false;
                decision.error = "output layer mask is not honoured";
            }
            break;
        }
        if (!decision.ok) {
            return decision;
        }
    }
    return decision;
}

// ---- phase ---------------------------------------------------------------------------------

enum class Phase : u32 { Closed = 0, Opened = 1, PinsApplied = 2, Running = 3, Done = 4, Failed = 5 };

enum class PhaseEvent : u32 { Opened = 1, PinsApplied = 2, Running = 3, Done = 4, Failed = 5 };

struct PhaseStep {
    bool allowed = false;
    Phase next = Phase::Failed;
};

// Closed -> Opened -> PinsApplied -> Running -> Done. Failed is reachable from any live phase
// and is terminal, as is Done. Run before pins is refused.
[[nodiscard]] inline PhaseStep AdvancePhase(Phase current, PhaseEvent event) {
    PhaseStep step;
    if (current == Phase::Done || current == Phase::Failed) {
        return step;
    }
    if (event == PhaseEvent::Failed) {
        step.allowed = true;
        step.next = Phase::Failed;
        return step;
    }
    const bool ok = (current == Phase::Closed && event == PhaseEvent::Opened) ||
                    (current == Phase::Opened && event == PhaseEvent::PinsApplied) ||
                    (current == Phase::PinsApplied && event == PhaseEvent::Running) ||
                    (current == Phase::Running && event == PhaseEvent::Done);
    if (!ok) {
        return step;
    }
    step.allowed = true;
    step.next = static_cast<Phase>(static_cast<u32>(event));
    return step;
}

// ---- pictures ------------------------------------------------------------------------------

struct ChecksumResult {
    bool ok = false;
    u64 hash = 0;
};

// FNV-1a over tightly packed B8G8R8A8 pixels. Stride padding is not hashed, so two captures of
// the same picture with different row padding compare equal.
[[nodiscard]] inline ChecksumResult PictureChecksum(std::span<const u8> bgra, u32 width,
                                                    u32 height, u32 stride) {
    ChecksumResult result;
    if (width == 0 || height == 0 || width > MaxPictureDimension || height > MaxPictureDimension) {
        return result;
    }
    const u64 row_bytes = u64{width} * 4U;
    if (u64{stride} < row_bytes) {
        return result;
    }
    const u64 need = u64{stride} * (u64{height} - 1U) + row_bytes;
    if (bgra.size() < need) {
        return result;
    }
    u64 hash = Fnv1aOffset;
    for (u32 y = 0; y < height; ++y) {
        const u8* row = bgra.data() + u64{stride} * y;
        for (u64 i = 0; i < row_bytes; ++i) {
            hash = (hash ^ row[i]) * Fnv1aPrime;
        }
    }
    result.ok = true;
    result.hash = hash;
    return result;
}

[[nodiscard]] inline u64 ChainDigest(u64 digest, u64 checksum) noexcept {
    u64 mixed = digest;
    for (u32 byte = 0; byte < 8; ++byte) {
        mixed = (mixed ^ ((checksum >> (byte * 8U)) & 0xFFU)) * Fnv1aPrime;
    }
    return mixed;
}

// B8G8R8A8 to RGBA, tightly packed. The PNG writer takes the result.
[[nodiscard]] inline bool SwizzleBgraToRgba(std::span<const u8> bgra, u32 width, u32 height,
                                            u32 stride, std::vector<u8>& rgba) {
    if (width == 0 || height == 0 || width > MaxPictureDimension || height > MaxPictureDimension) {
        return false;
    }
    const u64 row_bytes = u64{width} * 4U;
    if (u64{stride} < row_bytes) {
        return false;
    }
    const u64 need = u64{stride} * (u64{height} - 1U) + row_bytes;
    if (bgra.size() < need) {
        return false;
    }
    const u64 packed = row_bytes * height;
    rgba.resize(static_cast<size_t>(packed));
    for (u32 y = 0; y < height; ++y) {
        const u8* src = bgra.data() + u64{stride} * y;
        u8* dst = rgba.data() + row_bytes * y;
        for (u32 x = 0; x < width; ++x) {
            const u32 i = x * 4U;
            dst[i] = src[i + 2U];
            dst[i + 1U] = src[i + 1U];
            dst[i + 2U] = src[i];
            dst[i + 3U] = src[i + 3U];
        }
    }
    return true;
}

// ---- semantics -----------------------------------------------------------------------------

// Structural differences only: kind, address space, id, value_lo and value_hi. Order does not
// matter. An
// expected output with no match, a match whose high word differs, and an extra observed output
// each count as one mismatch.
[[nodiscard]] inline u64 CompareSemantics(std::span<const SemanticOutput> expected,
                                          std::span<const SemanticOutput> observed) {
    std::vector<bool> used(observed.size(), false);
    u64 mismatches = 0;
    for (const SemanticOutput& want : expected) {
        bool found = false;
        for (size_t i = 0; i < observed.size(); ++i) {
            if (used[i]) {
                continue;
            }
            const SemanticOutput& got = observed[i];
            if (got.kind == want.kind && got.space_tag == want.space_tag && got.id == want.id &&
                got.value_lo == want.value_lo && got.value_hi == want.value_hi) {
                used[i] = true;
                found = true;
                break;
            }
        }
        if (!found) {
            ++mismatches;
        }
    }
    for (const bool taken : used) {
        if (!taken) {
            ++mismatches;
        }
    }
    return mismatches;
}

// The address space a memory output is read from. `mapping_spaces` lists the space of every
// channel that maps the whole range (duplicates allowed). An output that names its space is read
// from exactly that space, and from none other when it does not map the range. An output that does
// not name one (a file that did not say) is read only when the range belongs to one space: with
// two candidates the replay cannot know which memory the guest saw and does not guess.
[[nodiscard]] inline bool ResolveOutputSpace(u32 space_tag, std::span<const u32> mapping_spaces,
                                             u32& space) noexcept {
    if (space_tag != 0) {
        const u32 wanted = space_tag - 1U;
        for (const u32 candidate : mapping_spaces) {
            if (candidate == wanted) {
                space = wanted;
                return true;
            }
        }
        return false;
    }
    if (mapping_spaces.empty()) {
        return false;
    }
    for (const u32 candidate : mapping_spaces) {
        if (candidate != mapping_spaces.front()) {
            return false;
        }
    }
    space = mapping_spaces.front();
    return true;
}

// ---- command header ------------------------------------------------------------------------

// CommandListHeader layout the pusher already uses: bits 0-39 address, 40 allow_flush, 41
// is_push_buffer, 42-62 size in words, 63 sync. header_flags bit 0 is allow_flush, bit 1 is
// is_push_buffer, bit 2 is sync. The capture writer must use this packing.
[[nodiscard]] inline bool SubmitHeaderFits(u32 word_count) noexcept {
    return word_count <= SubmitHeaderMaxWords;
}

[[nodiscard]] inline u64 SubmitHeaderRaw(u64 gpu_va, u32 word_count, u32 header_flags) noexcept {
    const u64 address = gpu_va & 0xFFFFFFFFFFULL;
    const u64 allow_flush = (header_flags & 1U) != 0 ? 1ULL : 0ULL;
    const u64 push = (header_flags & 2U) != 0 ? 1ULL : 0ULL;
    const u64 sync = (header_flags & 4U) != 0 ? 1ULL : 0ULL;
    const u64 size = u64{word_count} & u64{SubmitHeaderMaxWords};
    return address | (allow_flush << 40U) | (push << 41U) | (size << 42U) | (sync << 63U);
}

// ---- how many frames, and how the run ends -------------------------------------------------

// max_frames 0 means every selected frame. A prefix shorter than the capture is success when
// that many selected frames were replayed.
[[nodiscard]] inline u64 FramesAsked(u64 selected, u64 max_frames) noexcept {
    if (selected == 0) {
        return 0;
    }
    if (max_frames == 0 || max_frames > selected) {
        return selected;
    }
    return max_frames;
}

// The id stored before the first present, so the first CountAcceptedPresent() returns the first
// executed frame's captured id (the first selected frame minus the preroll).
[[nodiscard]] inline u64 PresentBaseBeforeFirst(u64 first_frame, u32 preroll_frames) noexcept {
    const u64 before = u64{preroll_frames} + 1U;
    if (first_frame < before) {
        return 0;
    }
    return first_frame - before;
}

struct OutcomeInput {
    ReadError open_error = ReadError::None;
    bool pins_ok = true;
    bool prefix_ok = true;
    bool interrupted = false;
    bool device_lost = false;
    bool shader_failure = false;
    bool report_io = false;
    bool startup_failed = false;  // the seams or the GPU could not be set up
    bool failed = false;          // the capture could not be applied (any other stop short)
    bool ran = false;
    u64 frames_replayed = 0;
    u64 frames_asked = 0;
};

// Prefixes of ReplayReport::error. The session writes one, the runner reads it back; an error
// without a prefix is a capture or replay failure.
inline constexpr std::string_view ReportIoPrefix = "report write failed: ";
inline constexpr std::string_view StartupPrefix = "startup failed: ";
inline constexpr std::string_view InterruptedText = "interrupted";

[[nodiscard]] inline bool StartsWith(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

// A bad capture or a pin the build cannot honour is BadArguments. Device, shader and I/O
// reasons win over a short count; an interrupt wins over everything that follows it. A run that
// replayed every frame it was asked for, and nothing went wrong, is FrameLimit, which the
// frontend maps to exit 0. Anything else that stopped short is BadArguments.
[[nodiscard]] inline Common::RunOutcome::Reason JudgeReplay(const OutcomeInput& in) noexcept {
    using Reason = Common::RunOutcome::Reason;
    if (!in.prefix_ok || in.open_error != ReadError::None || !in.pins_ok) {
        return Reason::BadArguments;
    }
    if (in.device_lost) {
        return Reason::DeviceLost;
    }
    if (in.shader_failure) {
        return Reason::ShaderFailure;
    }
    if (in.report_io) {
        return Reason::ReportIoError;
    }
    if (in.interrupted) {
        return Reason::Interrupted;
    }
    if (in.startup_failed) {
        return Reason::StartupError;
    }
    if (in.ran && !in.failed && in.frames_asked != 0 && in.frames_replayed >= in.frames_asked) {
        return Reason::FrameLimit;
    }
    return Reason::BadArguments;
}

// How a capture that has ended counts for the run: only a complete one is a clean exit; anything
// else is CaptureFailed. Requested through RunOutcome it replaces a clean exit and nothing else
// (every crash, device, shader, report, timeout and interrupt reason outranks it).
[[nodiscard]] inline Common::RunOutcome::Reason JudgeCapture(const CaptureReport& report) noexcept {
    return report.status == SectionStatus::Complete ? Common::RunOutcome::Reason::FrameLimit
                                                    : Common::RunOutcome::Reason::CaptureFailed;
}

// The first stop of a replay wins: its text and, when the stop is a refusal the format names (for
// instance UnsupportedState), that ReadError. Later stops change neither.
inline void RecordStop(std::string& message, ReadError& kind, std::string text,
                       ReadError text_kind) {
    if (message.empty()) {
        message = std::move(text);
        kind = text_kind;
    }
}

// The report's name of a ReadError; empty for None so an unnamed stop adds no field.
[[nodiscard]] inline std::string ErrorKindText(ReadError kind) {
    return kind == ReadError::None ? std::string{} : std::string(ReadErrorName(kind));
}

// The outcome text of a stop: the message, followed by the error name when there is one.
[[nodiscard]] inline std::string DescribeStop(const std::string& message, ReadError kind) {
    return kind == ReadError::None ? message : message + " (" + ErrorKindText(kind) + ")";
}

// ---- PNG, stored zlib, 8-bit RGBA ----------------------------------------------------------

[[nodiscard]] inline u32 Crc32(std::span<const u8> data) noexcept {
    u32 crc = 0xFFFFFFFFU;
    for (const u8 byte : data) {
        crc ^= byte;
        for (u32 bit = 0; bit < 8; ++bit) {
            const u32 mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] inline u32 Adler32(std::span<const u8> data) noexcept {
    u32 sum_a = 1;
    u32 sum_b = 0;
    for (const u8 byte : data) {
        sum_a = (sum_a + byte) % 65521U;
        sum_b = (sum_b + sum_a) % 65521U;
    }
    return (sum_b << 16U) | sum_a;
}

namespace detail {

inline void Put32Be(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<u8>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<u8>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<u8>(value & 0xFFU));
}

inline void PutChunk(std::vector<u8>& out, std::string_view type, std::span<const u8> data) {
    Put32Be(out, static_cast<u32>(data.size()));
    const size_t type_at = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    const u32 crc = Crc32(std::span<const u8>(out.data() + type_at, data.size() + type.size()));
    Put32Be(out, crc);
}

[[nodiscard]] inline u32 Read32Be(std::span<const u8> bytes, size_t at) {
    return (u32{bytes[at]} << 24U) | (u32{bytes[at + 1U]} << 16U) | (u32{bytes[at + 2U]} << 8U) |
           u32{bytes[at + 3U]};
}

} // namespace detail

// Writes an 8-bit RGBA sRGB PNG with stored (uncompressed) zlib blocks. False on a size the
// encoder refuses; the caller treats that as a report I/O failure.
[[nodiscard]] inline bool EncodeRgbaPng(std::span<const u8> rgba, u32 width, u32 height,
                                        std::vector<u8>& png, std::string& error) {
    png.clear();
    if (width == 0 || height == 0 || width > MaxPictureDimension || height > MaxPictureDimension) {
        error = "png size is outside 1..8192";
        return false;
    }
    const u64 row_bytes = u64{width} * 4U;
    const u64 packed = row_bytes * height;
    if (rgba.size() < packed) {
        error = "png source is shorter than width times height";
        return false;
    }
    std::vector<u8> filtered(static_cast<size_t>(packed + height));
    for (u32 y = 0; y < height; ++y) {
        const size_t dst = static_cast<size_t>(u64{y} * (row_bytes + 1U));
        filtered[dst] = 0;
        std::memcpy(filtered.data() + dst + 1U, rgba.data() + row_bytes * y,
                    static_cast<size_t>(row_bytes));
    }
    std::vector<u8> zlib;
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    const std::span<const u8> raw(filtered.data(), filtered.size());
    size_t offset = 0;
    while (offset < raw.size() || raw.empty()) {
        const size_t remain = raw.size() - offset;
        const size_t block = remain > 65535U ? 65535U : remain;
        const bool last = offset + block >= raw.size();
        zlib.push_back(last ? 0x01 : 0x00);
        const u16 len = static_cast<u16>(block);
        const u16 nlen = static_cast<u16>(~len);
        zlib.push_back(static_cast<u8>(len & 0xFFU));
        zlib.push_back(static_cast<u8>((len >> 8U) & 0xFFU));
        zlib.push_back(static_cast<u8>(nlen & 0xFFU));
        zlib.push_back(static_cast<u8>((nlen >> 8U) & 0xFFU));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + block));
        offset += block;
        if (last) {
            break;
        }
    }
    const u32 adler = Adler32(raw);
    detail::Put32Be(zlib, adler);

    static constexpr std::array<u8, 8> signature{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), signature.begin(), signature.end());
    std::array<u8, 13> ihdr{};
    ihdr[0] = static_cast<u8>((width >> 24U) & 0xFFU);
    ihdr[1] = static_cast<u8>((width >> 16U) & 0xFFU);
    ihdr[2] = static_cast<u8>((width >> 8U) & 0xFFU);
    ihdr[3] = static_cast<u8>(width & 0xFFU);
    ihdr[4] = static_cast<u8>((height >> 24U) & 0xFFU);
    ihdr[5] = static_cast<u8>((height >> 16U) & 0xFFU);
    ihdr[6] = static_cast<u8>((height >> 8U) & 0xFFU);
    ihdr[7] = static_cast<u8>(height & 0xFFU);
    ihdr[8] = 8;
    ihdr[9] = 6;
    detail::PutChunk(png, "IHDR", ihdr);
    detail::PutChunk(png, "IDAT", zlib);
    detail::PutChunk(png, "IEND", {});
    error.clear();
    return true;
}

[[nodiscard]] inline bool DecodeRgbaPng(std::span<const u8> png, std::vector<u8>& rgba, u32& width,
                                        u32& height, std::string& error) {
    rgba.clear();
    width = 0;
    height = 0;
    static constexpr std::array<u8, 8> signature{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < signature.size() + 8U ||
        std::memcmp(png.data(), signature.data(), signature.size()) != 0) {
        error = "png signature does not match";
        return false;
    }
    bool saw_ihdr = false;
    bool saw_iend = false;
    std::vector<u8> idat;
    size_t at = signature.size();
    while (at + 12U <= png.size()) {
        const u32 length = detail::Read32Be(png, at);
        if (u64{at} + 12U + length > png.size()) {
            error = "png chunk overruns the file";
            return false;
        }
        const std::span<const u8> type_and_data(png.data() + at + 4U, size_t{length} + 4U);
        const u32 stored = detail::Read32Be(png, at + 8U + length);
        if (Crc32(type_and_data) != stored) {
            error = "png chunk checksum does not match";
            return false;
        }
        const std::string_view type(reinterpret_cast<const char*>(png.data() + at + 4U), 4);
        const std::span<const u8> data(png.data() + at + 8U, length);
        if (type == "IHDR") {
            if (length != 13 || saw_ihdr) {
                error = "png header chunk is invalid";
                return false;
            }
            width = detail::Read32Be(data, 0);
            height = detail::Read32Be(data, 4);
            if (data[8] != 8 || data[9] != 6 || data[10] != 0 || data[11] != 0 || data[12] != 0) {
                error = "png is not 8-bit RGBA stored";
                return false;
            }
            if (width == 0 || height == 0 || width > MaxPictureDimension ||
                height > MaxPictureDimension) {
                error = "png size is outside 1..8192";
                return false;
            }
            saw_ihdr = true;
        } else if (type == "IDAT") {
            idat.insert(idat.end(), data.begin(), data.end());
        } else if (type == "IEND") {
            saw_iend = true;
            break;
        }
        at += 12U + length;
    }
    if (!saw_ihdr || !saw_iend || idat.size() < 6U) {
        error = "png is missing a required chunk";
        return false;
    }
    if (idat[0] != 0x78 || idat[1] != 0x01) {
        error = "png zlib header is not a stored block";
        return false;
    }
    const u64 row_bytes = u64{width} * 4U;
    const u64 filtered_size = (row_bytes + 1U) * height;
    std::vector<u8> filtered;
    filtered.reserve(static_cast<size_t>(filtered_size));
    size_t z = 2;
    bool last = false;
    while (!last) {
        if (z + 5U > idat.size()) {
            error = "png stored block is truncated";
            return false;
        }
        const u8 header = idat[z];
        if ((header & 0xFEU) != 0) {
            error = "png block is not stored";
            return false;
        }
        last = (header & 1U) != 0;
        const u16 len = static_cast<u16>(idat[z + 1U] | (u16{idat[z + 2U]} << 8U));
        const u16 nlen = static_cast<u16>(idat[z + 3U] | (u16{idat[z + 4U]} << 8U));
        if (static_cast<u16>(~len) != nlen) {
            error = "png stored block length is inconsistent";
            return false;
        }
        z += 5U;
        if (z + len > idat.size()) {
            error = "png stored block overruns the stream";
            return false;
        }
        filtered.insert(filtered.end(), idat.begin() + static_cast<std::ptrdiff_t>(z),
                        idat.begin() + static_cast<std::ptrdiff_t>(z + len));
        z += len;
    }
    if (z + 4U != idat.size()) {
        error = "png adler trailer is the wrong length";
        return false;
    }
    const u32 stored_adler = detail::Read32Be(idat, z);
    if (Adler32(filtered) != stored_adler) {
        error = "png adler checksum does not match";
        return false;
    }
    if (filtered.size() != filtered_size) {
        error = "png inflated size does not match the header";
        return false;
    }
    rgba.resize(static_cast<size_t>(row_bytes * height));
    for (u32 y = 0; y < height; ++y) {
        const size_t src = static_cast<size_t>(u64{y} * (row_bytes + 1U));
        if (filtered[src] != 0) {
            error = "png row filter is not none";
            return false;
        }
        std::memcpy(rgba.data() + row_bytes * y, filtered.data() + src + 1U,
                    static_cast<size_t>(row_bytes));
    }
    error.clear();
    return true;
}

[[nodiscard]] inline ReplayConfig ReplayConfigFromOptions(const Options& options,
                                                          u64 max_frames) {
    ReplayConfig config;
    config.capture_path = options.replay_path;
    config.images_dir = options.replay_images_dir;
    config.benchmark = options.replay_benchmark;
    config.max_frames = max_frames;
    config.frame_times_path = options.frame_times_path;
    return config;
}


// ---- map flags -----------------------------------------------------------------------------

// A MAP entry the replayer can apply: a reserved mapping has no representation to re-create.
[[nodiscard]] inline bool MapFlagsReplayable(u32 flags) noexcept {
    return (flags & MapReserved) == 0;
}

// ---- frame selection -----------------------------------------------------------------------

struct FrameSelection {
    u64 selected = 0;
    u64 preroll = 0;
};

[[nodiscard]] inline bool FrameIsPreroll(u32 frame_flags) noexcept {
    return (frame_flags & FramePreroll) != 0;
}

// The INDEX against the header: ids ascend without a gap from the first recorded frame, the
// pre-roll frames come first and match the header, the selected frames match finalized_frames
// and every frame is complete. A capture with no selected frame has nothing to replay.
[[nodiscard]] inline ReadError SelectFrames(const FileHeader& header, const IndexRecord& index,
                                            FrameSelection& out, std::string& detail) {
    out = {};
    if (header.finalized_frames == 0) {
        detail = "capture holds no finalized frame";
        return ReadError::NotComplete;
    }
    if (header.first_frame < header.preroll_frames) {
        detail = "first frame is earlier than its pre-roll";
        return ReadError::LimitExceeded;
    }
    u64 expect = header.first_frame - header.preroll_frames;
    bool in_preroll = true;
    for (const FrameIndexEntry& frame : index.frames) {
        if (frame.frame_id != expect) {
            detail = "frame ids are not consecutive from the first recorded frame";
            return ReadError::BadIndex;
        }
        ++expect;
        if ((frame.flags & FrameComplete) == 0) {
            detail = "index lists a frame that is not complete";
            return ReadError::NotComplete;
        }
        if (FrameIsPreroll(frame.flags)) {
            if (!in_preroll) {
                detail = "pre-roll frame after a selected frame";
                return ReadError::BadIndex;
            }
            ++out.preroll;
        } else {
            in_preroll = false;
            ++out.selected;
        }
    }
    if (out.preroll != header.preroll_frames || out.selected != header.finalized_frames) {
        detail = "index frame counts differ from the header";
        return ReadError::BadIndex;
    }
    detail.clear();
    return ReadError::None;
}

// ---- submits -------------------------------------------------------------------------------

// A SUBMIT event only announces how many words each entry will carry; the words arrive in the
// CONSUME events, in the order the pusher consumed them. Each consume reaches the pusher where it
// was recorded (see FeedConsumeEvent): the pusher never gets a word the original had not consumed
// yet, and never gets one later than the original did. Pushbuffer memory is never consulted.
struct PendingSubmit {
    SubmitEvent event;
    std::vector<u32> received;  // words consumed so far, one count per entry
};

inline constexpr u64 GpuAddressLimit = 1ULL << 40;

[[nodiscard]] inline bool BeginSubmit(const SubmitEvent& event, PendingSubmit& out,
                                      std::string& error) {
    if (event.entries.empty() || event.entries.size() > MaxEntriesPerSubmit) {
        error = "submit has no entry or too many";
        return false;
    }
    for (const SubmitEntry& entry : event.entries) {
        if (!SubmitHeaderFits(entry.word_count) || entry.header_flags > 7U ||
            entry.gpu_va >= GpuAddressLimit) {
            error = "submit entry does not fit a command list header";
            return false;
        }
    }
    out.event = event;
    out.received.assign(event.entries.size(), 0U);
    error.clear();
    return true;
}

// Entries are consumed one after the other, so an entry may only continue once every entry
// before it is full.
[[nodiscard]] inline bool AddConsume(PendingSubmit& submit, const ConsumeEvent& consume,
                                     std::string& error) {
    if (consume.submit_id != submit.event.submit_id ||
        consume.channel_id != submit.event.channel_id) {
        error = "consume names another submit or channel";
        return false;
    }
    if (consume.entry_index >= submit.event.entries.size()) {
        error = "consume names an entry the submit does not have";
        return false;
    }
    for (u32 i = 0; i < consume.entry_index; ++i) {
        if (submit.received[i] != submit.event.entries[i].word_count) {
            error = "consume skips an entry that is not full";
            return false;
        }
    }
    const SubmitEntry& entry = submit.event.entries[consume.entry_index];
    u32& have = submit.received[consume.entry_index];
    if (consume.word_offset != have) {
        error = "consume does not continue the entry's words";
        return false;
    }
    if (consume.words.size() > entry.word_count - have) {
        error = "consume carries more words than the entry holds";
        return false;
    }
    if (consume.gpu_va != entry.gpu_va + u64{consume.word_offset} * 4U) {
        error = "consume address does not match its entry";
        return false;
    }
    const u32 after = have + static_cast<u32>(consume.words.size());
    if ((consume.flags & ConsumeEntryEnd) != 0 && after != entry.word_count) {
        error = "consume ends an entry that is not full";
        return false;
    }
    have = after;
    error.clear();
    return true;
}

[[nodiscard]] inline bool SubmitComplete(const PendingSubmit& submit) noexcept {
    for (size_t i = 0; i < submit.event.entries.size(); ++i) {
        if (submit.received[i] != submit.event.entries[i].word_count) {
            return false;
        }
    }
    return true;
}

// What one CONSUME hands the pusher: the words it carries as a command list of one entry that
// starts at their own address. The pusher keeps its method state between lists, as it does between
// the entries of a real submit, so a method that the consume cut short continues in the next one
// and a suspension on a semaphore acquire holds the rest back exactly where the original held it.
// Only the first piece of an entry carries the entry's header flags (the sync wait is on its fetch).
struct SubmitPiece {
    u64 gpu_va = 0;
    u32 header_flags = 0;
    std::span<const u32> words;  // the consume's own words; none: nothing to push
};

inline void PopCompleteSubmits(std::deque<PendingSubmit>& queue) {
    while (!queue.empty() && SubmitComplete(queue.front())) {
        queue.pop_front();
    }
}

// Queues a SUBMIT behind the channel's earlier ones. A submit that carries no word at all has
// nothing to push and is dropped with the complete ones ahead of it.
[[nodiscard]] inline bool QueueSubmitEvent(std::deque<PendingSubmit>& queue,
                                           const SubmitEvent& submit, std::string& error) {
    PendingSubmit entry;
    if (!BeginSubmit(submit, entry, error)) {
        return false;
    }
    queue.push_back(std::move(entry));
    PopCompleteSubmits(queue);
    return true;
}

// Applies a CONSUME to the channel's queue and says what to push now: the consumed words
// themselves, at once, never the words of the submit that came after them. The consumed submit
// must be the oldest one, as the pusher consumes in order.
[[nodiscard]] inline bool FeedConsumeEvent(std::deque<PendingSubmit>& queue,
                                           const ConsumeEvent& consume, SubmitPiece& piece,
                                           std::string& error) {
    if (queue.empty()) {
        error = "consume names a channel with no submit";
        return false;
    }
    PendingSubmit& head = queue.front();
    if (consume.submit_id != head.event.submit_id) {
        bool queued = false;
        for (const PendingSubmit& later : queue) {
            queued = queued || later.event.submit_id == consume.submit_id;
        }
        error = queued ? "consume names a submit that is not the oldest one"
                       : "consume names a submit that was never queued";
        return false;
    }
    if (!AddConsume(head, consume, error)) {
        return false;
    }
    piece.gpu_va = consume.gpu_va;
    piece.header_flags =
        consume.word_offset == 0 ? head.event.entries[consume.entry_index].header_flags : 0U;
    piece.words = consume.words;
    PopCompleteSubmits(queue);
    return true;
}

// ---- present, picture, fence ---------------------------------------------------------------

[[nodiscard]] inline bool ValidatePresent(const PresentRecord& present, u64 frame_id,
                                          std::string& error) {
    if (present.present_id != frame_id) {
        error = "present id differs from its frame id";
        return false;
    }
    if (present.layers.empty() || present.layers.size() > MaxLayersPerPresent) {
        error = "present has no layer or too many";
        return false;
    }
    for (const PresentLayer& layer : present.layers) {
        if (layer.alloc_id == 0 || layer.width == 0 || layer.height == 0 ||
            layer.width > MaxPictureDimension || layer.height > MaxPictureDimension) {
            error = "present layer names no allocation or has a size outside 1..8192";
            return false;
        }
        if (layer.blending > 2U || layer.crop_right < layer.crop_left ||
            layer.crop_bottom < layer.crop_top) {
            error = "present layer blending or crop rectangle is invalid";
            return false;
        }
    }
    error.clear();
    return true;
}

// The composed live picture of a selected frame: the only kind the check compares.
[[nodiscard]] inline bool ValidateLivePicture(const EndPicture& picture, u64 frame_id,
                                              std::string& error) {
    if (picture.frame_id != frame_id || picture.kind != PictureKind::Composed ||
        picture.format != PictureFormat::B8G8R8A8Unorm || picture.color != ColorEncoding::SdrSrgb) {
        error = "live picture is not a composed B8G8R8A8 sRGB picture of its frame";
        return false;
    }
    if (picture.width == 0 || picture.height == 0 || picture.width > MaxPictureDimension ||
        picture.height > MaxPictureDimension ||
        u64{picture.stride_bytes} < u64{picture.width} * 4U) {
        error = "live picture size or stride is invalid";
        return false;
    }
    if (picture.pixels.size() != u64{picture.stride_bytes} * picture.height) {
        error = "live picture holds the wrong number of bytes";
        return false;
    }
    if (Fnv1a64(picture.pixels) != picture.pixel_checksum) {
        error = "live picture checksum does not match its pixels";
        return false;
    }
    error.clear();
    return true;
}

inline constexpr u32 SyncpointLimit = 192;
// No recorded raise means an unsatisfied display dependency, even if the command queue is idle.
// Check bounds before reading the guest syncpoint table. A zero threshold is already satisfied.
[[nodiscard]] inline bool PresentFenceReady(const PresentRecord& present, auto&& guest_value) {
    return present.fence_id < SyncpointLimit && present.fence_value <= UINT32_MAX &&
           (present.fence_value == 0 || guest_value(static_cast<u32>(present.fence_id)) >= present.fence_value);
}

inline constexpr u32 SemaphoreBytes = 4;
inline constexpr u32 QueryBytes = 8;

// Bytes an externally produced fence value occupies in memory: a semaphore word is 32 bits, a
// query result or a timestamp is 64 bits; `aux` of 4 or 8 overrides that. 0 for a syncpoint.
[[nodiscard]] inline u32 FenceWriteBytes(const FenceEntry& entry) noexcept {
    if (entry.kind == FenceKind::SyncpointRaised) {
        return 0;
    }
    if (entry.aux == 4 || entry.aux == 8) {
        return static_cast<u32>(entry.aux);
    }
    return entry.kind == FenceKind::SemaphoreWrite ? SemaphoreBytes : QueryBytes;
}

[[nodiscard]] inline bool ValidateFence(const FenceEntry& entry, std::string& error) {
    if (entry.kind == FenceKind::SyncpointRaised) {
        if (entry.id >= SyncpointLimit || entry.value > 0xFFFFFFFFULL) {
            error = "syncpoint fence is outside the syncpoint table or 32 bits";
            return false;
        }
    } else if (entry.kind == FenceKind::SemaphoreWrite || entry.kind == FenceKind::QueryValue ||
               entry.kind == FenceKind::Timestamp) {
        if (entry.id >= GpuAddressLimit ||
            (FenceWriteBytes(entry) == 4 && entry.value > 0xFFFFFFFFULL)) {
            error = "memory fence address is outside the GPU address space or its value is too wide";
            return false;
        }
    } else {
        error = "fence kind is unknown";
        return false;
    }
    error.clear();
    return true;
}

// The backend of the window a run opens. A replay composes through Vulkan whatever the local
// setting says (a capture's pins refuse anything else), so its window is a Vulkan one from the
// start; the session applies the pins only after the window exists.
[[nodiscard]] inline Settings::RendererBackend WindowBackendFor(
    bool replay_requested, Settings::RendererBackend configured) noexcept {
    return replay_requested ? Settings::RendererBackend::Vulkan : configured;
}

} // namespace VideoCore::Replay
