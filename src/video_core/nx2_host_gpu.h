// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
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
// A timestamp is written where the work changes (a render pass begins or ends, a dispatch, a copy
// outside a pass, the present blit) and labelled for the span that starts there; a span lasts
// until the next timestamp. The time between two timestamps is all the GPU needed to get from one
// to the other, so a span also holds work of the span before that overlapped it.
//
// The GPU waiting for the host is not in a work span: the first and the last command of every
// command buffer the scheduler submits is a timestamp, and the span from a submission's last one
// to the next one's first is Kind::Idle (the GPU had nothing to run, or waited for a semaphore of
// the submission). The labels of the work spans are GPU work only.
namespace VideoCommon::Nx2HostGpu {

/// One frame in this many is profiled.
constexpr u32 FrameInterval = 16;
/// Timestamps of one profiled frame; past it the timestamps that would start a span are only
/// counted (the last one is kept for the end of the frame).
constexpr u32 MaxQueriesPerFrame = 4096;
/// Groups a report line prints, by time.
constexpr size_t MaxReportedGroups = 14;
/// Pairs of neighbours the line of the "other" spans prints, by time.
constexpr size_t MaxReportedPairs = 8;
/// Length buckets of the "other" spans.
constexpr size_t OtherBucketCount = 6;

enum class Kind : u8 {
    Pass,            // a render pass
    Compute,         // a dispatch of the guest
    PresentBlit,     // the blit of the frame to the present image
    Other,           // everything else: barriers, work nothing labels
    End,             // the end of the frame, no span of its own
    FrameStart,      // from the start of the frame to the first thing that is labelled
    Idle,            // between the last command of a submission and the first of the next
    ImageCopy,       // an image copy outside a pass
    ImageBlit,       // an image blit (also its helper passes)
    ImageConvert,    // a format conversion of an image
    ImageUpload,     // buffer to image
    ImageDownload,   // image to buffer
    BufferCopy,      // a buffer copy or clear
    BufferUpload,    // staging to buffer
    EmulatorCompute, // a compute pass of the emulator (index conversion, unswizzle, ...)
    MipGen,          // mip chain generation
    Rescale,         // an image scaled up or down for resolution scaling
    ImageClear,      // a clear of an image outside a pass (sparse tiles)
    Transition,      // the first layout transition of an image
    UpscaleChain,    // the game's DLSS, FSR, Streamline, temporal and camera motion work
};

/// What a span is. The size, samples and attachments tell render passes apart.
struct Label {
    Kind kind{Kind::Other};
    bool depth{};
    u8 colors{};
    u8 samples{1};
    /// The span goes on with work that was counted when it started (the end of a pass, after a
    /// submission): its time counts, it is not one more of its kind.
    bool continued{};
    u32 width{};
    u32 height{};
    /// The pixels the work moves (image copies and blits), 0 when unknown.
    u64 pixels{};

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

/// The label of the work after `label` goes on: the same, not counted again.
[[nodiscard]] inline Label Continued(Label label) {
    label.continued = true;
    label.pixels = 0;
    return label;
}

/// A label that the passes of one size, sample count and attachment set share: only a render pass
/// has more than its kind.
[[nodiscard]] inline Label GroupOf(const Label& label) {
    if (label.kind != Kind::Pass) {
        return Label{.kind = label.kind};
    }
    return {
        .kind = Kind::Pass,
        .depth = label.depth,
        .colors = label.colors,
        .samples = label.samples,
        .width = label.width,
        .height = label.height,
    };
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
    case Kind::FrameStart:
        return "frame start";
    case Kind::Idle:
        return "idle waiting for a submit";
    case Kind::ImageCopy:
        return "image copy";
    case Kind::ImageBlit:
        return "image blit";
    case Kind::ImageConvert:
        return "image convert";
    case Kind::ImageUpload:
        return "image upload";
    case Kind::ImageDownload:
        return "image download";
    case Kind::BufferCopy:
        return "buffer copy";
    case Kind::BufferUpload:
        return "buffer upload";
    case Kind::EmulatorCompute:
        return "compute (emulator)";
    case Kind::MipGen:
        return "mip gen";
    case Kind::Rescale:
        return "rescale";
    case Kind::ImageClear:
        return "image clear";
    case Kind::Transition:
        return "layout transition";
    case Kind::UpscaleChain:
        return "upscale chain";
    case Kind::End:
    case Kind::Other:
        break;
    }
    return "other (barriers, unlabelled work)";
}

/// How a neighbour of an "other" span is called in the pairs: the kind only, every pass is one.
[[nodiscard]] inline std::string KindName(Kind kind) {
    if (kind == Kind::Pass) {
        return "pass";
    }
    if (kind == Kind::Idle) {
        return "idle";
    }
    if (kind == Kind::End) {
        return "frame end";
    }
    if (kind == Kind::Other) {
        return "other";
    }
    return Describe(Label{.kind = kind});
}

/// What the spans of a kind count, nullptr for the kinds that are not counted.
[[nodiscard]] inline const char* UnitOf(Kind kind) {
    switch (kind) {
    case Kind::Pass:
        return "passes";
    case Kind::Compute:
    case Kind::EmulatorCompute:
        return "dispatches";
    case Kind::PresentBlit:
    case Kind::ImageBlit:
        return "blits";
    case Kind::ImageCopy:
    case Kind::BufferCopy:
        return "copies";
    case Kind::ImageConvert:
        return "conversions";
    case Kind::ImageUpload:
    case Kind::BufferUpload:
        return "uploads";
    case Kind::ImageDownload:
        return "downloads";
    case Kind::MipGen:
        return "chains";
    case Kind::Rescale:
        return "rescales";
    case Kind::ImageClear:
        return "clears";
    case Kind::Transition:
        return "transitions";
    case Kind::UpscaleChain:
        return "runs";
    case Kind::End:
    case Kind::Other:
    case Kind::FrameStart:
    case Kind::Idle:
        break;
    }
    return nullptr;
}

/// The length bucket of an "other" span: <5 us, 5-20 us, 20-100 us, 100-500 us, 0.5-2 ms, more.
[[nodiscard]] inline size_t OtherBucketOf(double ns) {
    static constexpr std::array<double, OtherBucketCount - 1> limits{5e3, 20e3, 100e3, 500e3, 2e6};
    size_t bucket = 0;
    while (bucket < limits.size() && ns >= limits[bucket]) {
        ++bucket;
    }
    return bucket;
}

[[nodiscard]] inline const char* OtherBucketName(size_t bucket) {
    static constexpr std::array<const char*, OtherBucketCount> names{
        "<5 us", "5-20 us", "20-100 us", "100-500 us", "0.5-2 ms", ">2 ms"};
    return names[(std::min)(bucket, names.size() - 1)];
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
    /// out of timestamps (counted in Overflow). The last timestamp is kept for End, and
    /// `reserve` more for what the caller writes right after (the first timestamp of the next
    /// submission, after the last of this one).
    [[nodiscard]] u32 Begin(const Label& label, u32 reserve = 0) {
        if (labels.size() + 1 + reserve >= max_queries) {
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
        u64 pixels{};
    };

    /// What an "other" span sat between: the kinds of the labels before and after it.
    struct Pair {
        Kind before{};
        Kind after{};
        double ns{};
        u64 spans{};
    };

    /// Adds a profiled frame: timestamps[i] (device ticks) starts the span labels[i]; the
    /// timestamp of the End label, the last, ends the last one. A frame without a span is not
    /// added. `overflow` is what FrameLog counted. A span of Kind::Idle is the GPU waiting: it
    /// is in the idle numbers, not in a group. A timestamp that is before the one it follows
    /// makes a span of 0.
    void AddFrame(std::span<const u64> timestamps, std::span<const Label> labels, u32 overflow,
                  double ns_per_tick, u32 valid_bits) {
        const size_t count = (std::min)(timestamps.size(), labels.size());
        if (count < 2) {
            return;
        }
        const u64 mask = valid_bits >= 64 ? ~u64{0} : (u64{1} << valid_bits) - 1;
        const auto span_ns = [&](u64 from, u64 to) {
            // The counter wraps at its valid bits; a difference past half of that went backwards.
            u64 ticks = (to - from) & mask;
            if (ticks > mask / 2) {
                ticks = 0;
            }
            return static_cast<double>(ticks) * ns_per_tick;
        };
        for (size_t i = 0; i + 1 < count; ++i) {
            const double ns = span_ns(timestamps[i], timestamps[i + 1]);
            total_ns += ns;
            const Label& label = labels[i];
            if (label.kind == Kind::Idle) {
                idle_ns += ns;
                ++submits;
                if (ns > 0.0) {
                    ++gaps;
                }
                largest_gap_ns = (std::max)(largest_gap_ns, ns);
                if (i + 2 == count) {
                    // The gap that ends the frame: from its last submission to the first command
                    // of the next one, so the host's time between frames.
                    frame_gap_ns += ns;
                }
                continue;
            }
            if (label.kind == Kind::Other) {
                AddOther(i == 0 ? Kind::FrameStart : labels[i - 1].kind, labels[i + 1].kind, ns);
            }
            AddSpan(label, ns);
            if (label.kind == Kind::Pass && !label.continued) {
                ++passes;
            }
        }
        first_to_last_ns += span_ns(timestamps[0], timestamps[count - 1]);
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
    [[nodiscard]] double IdleNs() const noexcept {
        return idle_ns;
    }
    [[nodiscard]] double LargestGapNs() const noexcept {
        return largest_gap_ns;
    }
    [[nodiscard]] double FrameGapNs() const noexcept {
        return frame_gap_ns;
    }
    [[nodiscard]] u64 Gaps() const noexcept {
        return gaps;
    }
    [[nodiscard]] u64 Submits() const noexcept {
        return submits;
    }
    [[nodiscard]] double FirstToLastNs() const noexcept {
        return first_to_last_ns;
    }
    [[nodiscard]] u64 Dropped() const noexcept {
        return dropped;
    }
    [[nodiscard]] double OtherNs() const noexcept {
        return other_ns;
    }
    [[nodiscard]] u64 OtherSpans() const noexcept {
        return other_spans;
    }
    [[nodiscard]] const std::vector<Pair>& Pairs() const noexcept {
        return pairs;
    }
    [[nodiscard]] u64 BucketSpans(size_t bucket) const noexcept {
        return bucket_spans[bucket];
    }
    [[nodiscard]] double BucketNs(size_t bucket) const noexcept {
        return bucket_ns[bucket];
    }
    [[nodiscard]] double LargestOtherNs() const noexcept {
        return largest_other_ns;
    }

    /// The report line, per profiled frame (the largest gap is the largest of the window): total
    /// and busy time, the idle time, then the top MaxReportedGroups groups by time, the rest as
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
        std::string line = fmt::format(
            "Host GPU per profiled frame ({} sampled): total {:.2f} ms | busy {:.2f} ms | idle "
            "waiting for a submit {:.2f} ms in {:.1f} gaps (largest {:.2f} ms) | submits {:.1f} | "
            "frame-start gap {:.2f} ms",
            frames, total_ns * per_frame / 1e6, (total_ns - idle_ns) * per_frame / 1e6,
            idle_ns * per_frame / 1e6, static_cast<double>(gaps) * per_frame,
            largest_gap_ns / 1e6, static_cast<double>(submits) * per_frame,
            frame_gap_ns * per_frame / 1e6);
        const size_t shown = (std::min)(order.size(), MaxReportedGroups);
        for (size_t i = 0; i < shown; ++i) {
            const Group& group = *order[i];
            std::string detail;
            if (const char* const unit = UnitOf(group.label.kind); unit != nullptr) {
                detail = fmt::format(" in {:.1f} {}", static_cast<double>(group.spans) * per_frame,
                                     unit);
            }
            if (group.pixels != 0) {
                detail += fmt::format(", {:.1f} MP",
                                      static_cast<double>(group.pixels) * per_frame / 1e6);
            }
            line += fmt::format(" | {}: {:.2f} ms{}", Describe(group.label),
                                group.ns * per_frame / 1e6, detail);
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

    /// The second report line, per profiled frame: what the "other" spans sat between (the top
    /// MaxReportedPairs pairs of neighbour kinds by time), how long they are, and the largest
    /// one (of the window). Empty when no frame was profiled or none had an "other" span.
    [[nodiscard]] std::string FormatOther() const {
        if (frames == 0 || other_spans == 0) {
            return {};
        }
        const double per_frame = 1.0 / static_cast<double>(frames);
        std::vector<const Pair*> order;
        order.reserve(pairs.size());
        for (const Pair& pair : pairs) {
            order.push_back(&pair);
        }
        std::stable_sort(order.begin(), order.end(),
                         [](const Pair* a, const Pair* b) { return a->ns > b->ns; });
        std::string line = fmt::format(
            "Host GPU other, per profiled frame ({} sampled): {:.2f} ms in {:.1f} spans", frames,
            other_ns * per_frame / 1e6, static_cast<double>(other_spans) * per_frame);
        const size_t shown = (std::min)(order.size(), MaxReportedPairs);
        for (size_t i = 0; i < shown; ++i) {
            const Pair& pair = *order[i];
            line += fmt::format("{}between {} and {}: {:.2f} ms in {:.1f} spans",
                                i == 0 ? " | " : "; ", KindName(pair.before), KindName(pair.after),
                                pair.ns * per_frame / 1e6,
                                static_cast<double>(pair.spans) * per_frame);
        }
        if (order.size() > shown) {
            double rest_ns = 0.0;
            for (size_t i = shown; i < order.size(); ++i) {
                rest_ns += order[i]->ns;
            }
            line += fmt::format("; {} more pairs: {:.2f} ms", order.size() - shown,
                                rest_ns * per_frame / 1e6);
        }
        for (size_t bucket = 0; bucket < OtherBucketCount; ++bucket) {
            line += fmt::format("{}{}: {:.1f} spans, {:.2f} ms", bucket == 0 ? " | sizes " : "; ",
                                OtherBucketName(bucket),
                                static_cast<double>(bucket_spans[bucket]) * per_frame,
                                bucket_ns[bucket] * per_frame / 1e6);
        }
        line += fmt::format(" | largest {:.3f} ms between {} and {}", largest_other_ns / 1e6,
                            KindName(largest_before), KindName(largest_after));
        return line;
    }

    void Clear() {
        *this = Window{};
    }

private:
    void AddSpan(const Label& label, double ns) {
        const Label key = GroupOf(label);
        const u64 counted = label.continued ? 0 : 1;
        for (Group& group : groups) {
            if (group.label == key) {
                group.ns += ns;
                group.spans += counted;
                group.pixels += label.pixels;
                return;
            }
        }
        groups.push_back({.label = key, .ns = ns, .spans = counted, .pixels = label.pixels});
    }

    void AddOther(Kind before, Kind after, double ns) {
        ++other_spans;
        other_ns += ns;
        const size_t bucket = OtherBucketOf(ns);
        ++bucket_spans[bucket];
        bucket_ns[bucket] += ns;
        if (ns > largest_other_ns) {
            largest_other_ns = ns;
            largest_before = before;
            largest_after = after;
        }
        for (Pair& pair : pairs) {
            if (pair.before == before && pair.after == after) {
                pair.ns += ns;
                ++pair.spans;
                return;
            }
        }
        pairs.push_back({.before = before, .after = after, .ns = ns, .spans = 1});
    }

    std::vector<Group> groups;
    double total_ns{};
    double idle_ns{};
    double largest_gap_ns{};
    double frame_gap_ns{};
    double first_to_last_ns{};
    u64 gaps{};
    u64 submits{};
    u64 dropped{};
    u64 passes{};
    u32 frames{};
    u32 skipped{};
    std::vector<Pair> pairs;
    std::array<u64, OtherBucketCount> bucket_spans{};
    std::array<double, OtherBucketCount> bucket_ns{};
    double other_ns{};
    double largest_other_ns{};
    Kind largest_before{};
    Kind largest_after{};
    u64 other_spans{};
};

} // namespace VideoCommon::Nx2HostGpu
