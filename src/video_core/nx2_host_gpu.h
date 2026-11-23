// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"

// The host GPU time of a frame by pass, from GPU timestamps (EDEN_NX2_GPU_PROFILE, full or lite).
// This is the part of that without Vulkan: what labels a timestamp, how a profiled frame's
// timestamps turn into spans, and how the spans of a report window add up and print. The Vulkan
// side (the timestamp writes and their readback) is in the scheduler.
//
// A timestamp is written where the work changes (a render pass begins or ends, a dispatch, the
// present blit) and labelled for the span that starts there; a span lasts until the next
// timestamp. The time between two timestamps is all the GPU needed to get from one to the other,
// so a span also holds the time the GPU waited with nothing to do (the host submitted late) and
// work of the span before that overlapped it.
namespace VideoCommon::Nx2HostGpu {

/// One frame in this many is profiled.
constexpr u32 FrameInterval = 16;
/// Timestamps of one profiled frame; past it the timestamps that would start a span are only
/// counted (the last one is kept for the end of the frame).
constexpr u32 MaxQueriesPerFrame = 4096;
/// Groups a report line prints, by time.
constexpr size_t MaxReportedGroups = 12;

enum class Kind : u8 {
    Pass,        // a render pass
    Compute,     // a dispatch
    PresentBlit, // the blit of the frame to the present image
    Other,       // everything else: copies, uploads, barriers, the GPU waiting for the host
    End,         // the end of the frame, no span of its own
};

/// What a span is. The size, samples and attachments tell render passes apart.
struct Label {
    Kind kind{Kind::Other};
    bool depth{};
    u8 colors{};
    u8 samples{1};
    u32 width{};
    u32 height{};

    friend bool operator==(const Label&, const Label&) = default;
};

[[nodiscard]] inline Label PassLabel(u32 width, u32 height, u32 samples, u32 colors, bool depth) {
    return {
        .kind = Kind::Pass,
        .depth = depth,
        .colors = static_cast<u8>((std::min)(colors, u32{255})),
        .samples = static_cast<u8>((std::min)(samples, u32{255})),
        .width = width,
        .height = height,
    };
}

/// A label that the passes of one size, sample count and attachment set share: only a render pass
/// has more than its kind.
[[nodiscard]] inline Label GroupOf(const Label& label) {
    return label.kind == Kind::Pass ? label : Label{.kind = label.kind};
}

[[nodiscard]] inline std::string Describe(const Label& label) {
    switch (label.kind) {
    case Kind::Pass:
        return label.samples > 1
                   ? fmt::format("pass {}x{}x{} samples c{}{}", label.width, label.height,
                                 label.samples, label.colors, label.depth ? "+d" : "")
                   : fmt::format("pass {}x{} c{}{}", label.width, label.height, label.colors,
                                 label.depth ? "+d" : "");
    case Kind::Compute:
        return "compute";
    case Kind::PresentBlit:
        return "present blit";
    case Kind::End:
    case Kind::Other:
        break;
    }
    return "other (copies, uploads, barriers)";
}

[[nodiscard]] inline const char* UnitOf(Kind kind) {
    switch (kind) {
    case Kind::Pass:
        return "passes";
    case Kind::Compute:
        return "dispatches";
    case Kind::PresentBlit:
        return "blits";
    case Kind::End:
    case Kind::Other:
        break;
    }
    return "spans";
}

/// The labels of the timestamps of one profiled frame, in the order they are written. A
/// timestamp's index in the frame is its query's.
class FrameLog {
public:
    static constexpr u32 NoQuery = ~u32{0};

    FrameLog() : FrameLog(MaxQueriesPerFrame) {}

    explicit FrameLog(u32 limit) : max_queries{limit} {
        labels.reserve(limit);
    }

    /// The index of the timestamp that starts a span with `label`, or NoQuery when the frame is
    /// out of timestamps (counted in Overflow). The last timestamp is kept for End.
    [[nodiscard]] u32 Begin(const Label& label) {
        if (labels.size() + 1 >= max_queries) {
            ++overflow;
            return NoQuery;
        }
        labels.push_back(label);
        return static_cast<u32>(labels.size() - 1);
    }

    /// The index of the timestamp that ends the frame (NoQuery for a frame that has ended).
    [[nodiscard]] u32 End() {
        if (ended || labels.size() >= max_queries) {
            return NoQuery;
        }
        ended = true;
        labels.push_back({.kind = Kind::End});
        return static_cast<u32>(labels.size() - 1);
    }

    void Clear() {
        labels.clear();
        overflow = 0;
        ended = false;
    }

    [[nodiscard]] std::span<const Label> Labels() const noexcept {
        return labels;
    }
    [[nodiscard]] u32 Overflow() const noexcept {
        return overflow;
    }
    [[nodiscard]] bool Ended() const noexcept {
        return ended;
    }
    [[nodiscard]] u32 Used() const noexcept {
        return static_cast<u32>(labels.size());
    }

private:
    u32 max_queries;
    std::vector<Label> labels;
    u32 overflow{};
    bool ended{};
};

/// The spans of the profiled frames of one report window.
class Window {
public:
    struct Group {
        Label label;
        double ns{};
        u64 spans{};
    };

    /// Adds a profiled frame: timestamps[i] (device ticks) starts the span labels[i]; the
    /// timestamp of the End label, the last, ends the last one. A frame without a span is not
    /// added. `overflow` is what FrameLog counted.
    void AddFrame(std::span<const u64> timestamps, std::span<const Label> labels, u32 overflow,
                  double ns_per_tick, u32 valid_bits) {
        const size_t count = (std::min)(timestamps.size(), labels.size());
        if (count < 2) {
            return;
        }
        const u64 mask = valid_bits >= 64 ? ~u64{0} : (u64{1} << valid_bits) - 1;
        for (size_t i = 0; i + 1 < count; ++i) {
            // The counter wraps at its valid bits.
            const u64 ticks = (timestamps[i + 1] - timestamps[i]) & mask;
            const double ns = static_cast<double>(ticks) * ns_per_tick;
            AddSpan(labels[i], ns);
            total_ns += ns;
            if (labels[i].kind == Kind::Pass) {
                ++passes;
            }
        }
        first_to_last_ns +=
            static_cast<double>((timestamps[count - 1] - timestamps[0]) & mask) * ns_per_tick;
        dropped += overflow;
        ++frames;
    }

    /// A frame that was due for profiling and was not (every slot was still in use, or the
    /// readback never came).
    void NoteSkipped() {
        ++skipped;
    }

    [[nodiscard]] u32 Frames() const noexcept {
        return frames;
    }
    [[nodiscard]] bool Empty() const noexcept {
        return frames == 0 && skipped == 0;
    }
    [[nodiscard]] const std::vector<Group>& Groups() const noexcept {
        return groups;
    }
    [[nodiscard]] double TotalNs() const noexcept {
        return total_ns;
    }
    [[nodiscard]] double FirstToLastNs() const noexcept {
        return first_to_last_ns;
    }
    [[nodiscard]] u64 Dropped() const noexcept {
        return dropped;
    }

    /// The report line, per profiled frame: the top MaxReportedGroups groups by time, the rest as
    /// one. Empty when no frame was profiled.
    [[nodiscard]] std::string Format() const {
        if (frames == 0) {
            return {};
        }
        const double per_frame = 1.0 / static_cast<double>(frames);
        std::vector<const Group*> order;
        order.reserve(groups.size());
        for (const Group& group : groups) {
            order.push_back(&group);
        }
        std::stable_sort(order.begin(), order.end(),
                         [](const Group* a, const Group* b) { return a->ns > b->ns; });
        std::string line = fmt::format("Host GPU per profiled frame ({} frames sampled): total "
                                       "{:.2f} ms",
                                       frames, total_ns * per_frame / 1e6);
        const size_t shown = (std::min)(order.size(), MaxReportedGroups);
        for (size_t i = 0; i < shown; ++i) {
            const Group& group = *order[i];
            const std::string name = Describe(group.label);
            const char* const unit = UnitOf(group.label.kind);
            if (group.label.kind == Kind::Other) {
                line += fmt::format(" | {}: {:.2f} ms", name, group.ns * per_frame / 1e6);
            } else {
                line += fmt::format(" | {}: {:.2f} ms in {:.1f} {}", name,
                                    group.ns * per_frame / 1e6,
                                    static_cast<double>(group.spans) * per_frame, unit);
            }
        }
        if (order.size() > shown) {
            double rest_ns = 0.0;
            for (size_t i = shown; i < order.size(); ++i) {
                rest_ns += order[i]->ns;
            }
            line += fmt::format(" | {} more groups: {:.2f} ms", order.size() - shown,
                                rest_ns * per_frame / 1e6);
        }
        line += fmt::format(" | passes {:.1f} | first-to-last timestamp {:.2f} ms",
                            static_cast<double>(passes) * per_frame,
                            first_to_last_ns * per_frame / 1e6);
        if (dropped != 0) {
            line += fmt::format(" | {} timestamps over the {} a frame keep", dropped,
                                MaxQueriesPerFrame);
        }
        if (skipped != 0) {
            line += fmt::format(" | {} frames not profiled (no free query slot)", skipped);
        }
        return line;
    }

    void Clear() {
        *this = Window{};
    }

private:
    void AddSpan(const Label& label, double ns) {
        const Label key = GroupOf(label);
        for (Group& group : groups) {
            if (group.label == key) {
                group.ns += ns;
                ++group.spans;
                return;
            }
        }
        groups.push_back({.label = key, .ns = ns, .spans = 1});
    }

    std::vector<Group> groups;
    double total_ns{};
    double first_to_last_ns{};
    u64 dropped{};
    u64 passes{};
    u32 frames{};
    u32 skipped{};
};

} // namespace VideoCommon::Nx2HostGpu
