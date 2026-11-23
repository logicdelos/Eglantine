// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/nx2_host_gpu.h"

namespace {

namespace Host = VideoCommon::Nx2HostGpu;

constexpr double MicrosecondPerTick = 1000.0;

/// Other, 3840x2160 twice, a dispatch, 2560x1440 with depth; ticks of 1 us.
struct Frame {
    std::array<Host::Label, 6> labels{
        Host::Label{},
        Host::PassLabel(3840, 2160, 1, 1, false),
        Host::PassLabel(3840, 2160, 1, 1, false),
        Host::Label{.kind = Host::Kind::Compute},
        Host::PassLabel(2560, 1440, 1, 1, true),
        Host::Label{.kind = Host::Kind::End},
    };
    std::array<u64, 6> timestamps{0, 1000, 4000, 5000, 5500, 7500};
};

/// Two submissions, as the scheduler writes them (nx2_host_gpu.h): a pass that the first one ends
/// (its last timestamp is Idle), the second one's upload command buffer, its main command buffer
/// going on with the pass, two copies of 2 and 3 megapixels with the work after the first, and
/// the last submission's end, which the frame waits out until its End timestamp. Ticks of 1 us.
struct SubmitFrame {
    static Host::Label Copy(u64 pixels) {
        return {.kind = Host::Kind::ImageCopy, .pixels = pixels};
    }

    std::array<Host::Label, 10> labels{
        Host::Label{.kind = Host::Kind::FrameStart},
        Host::PassLabel(1280, 720, 1, 1, false),
        Host::Label{.kind = Host::Kind::Idle},
        Host::Label{.kind = Host::Kind::BufferUpload},
        Host::Continued(Host::PassLabel(1280, 720, 1, 1, false)),
        Copy(2'000'000),
        Host::Continued(Host::Label{}),
        Copy(3'000'000),
        Host::Label{.kind = Host::Kind::Idle},
        Host::Label{.kind = Host::Kind::End},
    };
    std::array<u64, 10> timestamps{0, 1000, 3000, 3400, 3500, 4500, 5000, 5200, 5700, 6700};
};

} // Anonymous namespace

TEST_CASE("Nx2HostGpu groups spans by pass size, samples and attachments", "[video_core]") {
    const Frame frame;
    Host::Window window;
    window.AddFrame(frame.timestamps, frame.labels, 0, MicrosecondPerTick, 64);
    REQUIRE(window.Frames() == 1);
    REQUIRE(window.Groups().size() == 4);
    CHECK(window.TotalNs() == 7'500'000.0);
    CHECK(window.FirstToLastNs() == 7'500'000.0);
    CHECK(window.IdleNs() == 0.0);
    CHECK(window.Submits() == 0);
    CHECK(window.Format() ==
          "Host GPU per profiled frame (1 sampled): total 7.50 ms | busy 7.50 ms | idle waiting "
          "for a submit 0.00 ms in 0.0 gaps (largest 0.00 ms) | submits 0.0 | frame-start gap "
          "0.00 ms | pass 3840x2160 c1: 4.00 ms in 2.0 passes | pass 2560x1440 c1+d: 2.00 ms in "
          "1.0 passes | other (barriers, unlabelled work): 1.00 ms | compute: 0.50 ms in 1.0 "
          "dispatches | passes 3.0 | first-to-last timestamp 7.50 ms");

    // The same frame again: the line is per profiled frame, so only the count changes.
    window.AddFrame(frame.timestamps, frame.labels, 0, MicrosecondPerTick, 64);
    const std::string line = window.Format();
    CHECK(line.starts_with("Host GPU per profiled frame (2 sampled): total 7.50 ms"));
    CHECK(line.find("pass 3840x2160 c1: 4.00 ms in 2.0 passes") != std::string::npos);
}

TEST_CASE("Nx2HostGpu takes the GPU's idle time between submissions out of the labels",
          "[video_core]") {
    const SubmitFrame frame;
    Host::Window window;
    window.AddFrame(frame.timestamps, frame.labels, 0, MicrosecondPerTick, 64);
    REQUIRE(window.Frames() == 1);

    // Two Idle spans: 3000 to 3400 between the submissions, 5700 to 6700 after the last one.
    CHECK(window.IdleNs() == 1'400'000.0);
    CHECK(window.Gaps() == 2);
    CHECK(window.Submits() == 2);
    CHECK(window.LargestGapNs() == 1'000'000.0);
    // The gap that ends the frame is the one before its End timestamp.
    CHECK(window.FrameGapNs() == 1'000'000.0);
    CHECK(window.TotalNs() == 6'700'000.0);
    CHECK(window.FirstToLastNs() == 6'700'000.0);
    // The pass goes on after the submission: its time adds up, it is one pass.
    REQUIRE(window.Groups().size() == 5);
    CHECK(window.Format() ==
          "Host GPU per profiled frame (1 sampled): total 6.70 ms | busy 5.30 ms | idle waiting "
          "for a submit 1.40 ms in 2.0 gaps (largest 1.00 ms) | submits 2.0 | frame-start gap "
          "1.00 ms | pass 1280x720 c1: 3.00 ms in 1.0 passes | frame start: 1.00 ms | image "
          "copy: 1.00 ms in 2.0 copies, 5.0 MP | other (barriers, unlabelled work): 0.20 ms | "
          "buffer upload: 0.10 ms in 1.0 uploads | passes 1.0 | first-to-last timestamp 6.70 ms");

    // Per profiled frame: a second frame of the same spans changes the count only, and the
    // largest gap is the largest of the window.
    window.AddFrame(frame.timestamps, frame.labels, 0, MicrosecondPerTick, 64);
    const std::string line = window.Format();
    CHECK(line.starts_with("Host GPU per profiled frame (2 sampled): total 6.70 ms | busy 5.30 ms"));
    CHECK(line.find("in 2.0 gaps (largest 1.00 ms)") != std::string::npos);
    CHECK(line.find("image copy: 1.00 ms in 2.0 copies, 5.0 MP") != std::string::npos);
}

TEST_CASE("Nx2HostGpu clamps an idle span that went backwards to nothing", "[video_core]") {
    // The upload command buffer's timestamp reads earlier than the submission's last one.
    const std::array<Host::Label, 4> labels{
        Host::Label{.kind = Host::Kind::FrameStart},
        Host::Label{.kind = Host::Kind::Idle},
        Host::Continued(Host::Label{.kind = Host::Kind::FrameStart}),
        Host::Label{.kind = Host::Kind::End},
    };
    const std::array<u64, 4> timestamps{0, 1000, 900, 2000};
    Host::Window window;
    window.AddFrame(timestamps, labels, 0, MicrosecondPerTick, 64);
    REQUIRE(window.Frames() == 1);
    CHECK(window.IdleNs() == 0.0);
    // A submission without a gap is still a submission.
    CHECK(window.Gaps() == 0);
    CHECK(window.Submits() == 1);
    CHECK(window.FrameGapNs() == 0.0);
    // 1000 of the first span and 1100 of the one after it.
    CHECK(window.TotalNs() == 2'100'000.0);
    REQUIRE(window.Groups().size() == 1);
    CHECK(window.Groups()[0].ns == 2'100'000.0);
    // A new, then a continued span of the same kind: one.
    CHECK(window.Groups()[0].spans == 1);
}

TEST_CASE("Nx2HostGpu names samples, depth and the spans that are not passes", "[video_core]") {
    CHECK(Host::Describe(Host::PassLabel(1920, 1080, 4, 2, true)) == "pass 1920x1080x4 samples c2+d");
    CHECK(Host::Describe(Host::PassLabel(1280, 720, 1, 1, false)) == "pass 1280x720 c1");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::Compute}) == "compute");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::PresentBlit}) == "present blit");
    CHECK(Host::Describe(Host::Label{}) == "other (barriers, unlabelled work)");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::FrameStart}) == "frame start");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::Idle}) == "idle waiting for a submit");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::ImageCopy}) == "image copy");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::ImageBlit}) == "image blit");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::ImageConvert}) == "image convert");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::ImageUpload}) == "image upload");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::ImageDownload}) == "image download");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::BufferCopy}) == "buffer copy");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::BufferUpload}) == "buffer upload");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::EmulatorCompute}) ==
          "compute (emulator)");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::MipGen}) == "mip gen");
    CHECK(Host::Describe(Host::Label{.kind = Host::Kind::Rescale}) == "rescale");
    // Only passes are told apart by size; a dispatch is a dispatch, whatever it moves.
    CHECK(Host::GroupOf({.kind = Host::Kind::Compute, .width = 8, .height = 8}) ==
          Host::Label{.kind = Host::Kind::Compute});
    CHECK(Host::GroupOf({.kind = Host::Kind::ImageCopy, .continued = true, .pixels = 9}) ==
          Host::Label{.kind = Host::Kind::ImageCopy});
    CHECK(Host::GroupOf(Host::Continued(Host::PassLabel(8, 8, 1, 1, false))) ==
          Host::PassLabel(8, 8, 1, 1, false));
    // What is not counted has no unit.
    CHECK(Host::UnitOf(Host::Kind::Other) == nullptr);
    CHECK(Host::UnitOf(Host::Kind::FrameStart) == nullptr);
    CHECK(Host::UnitOf(Host::Kind::Idle) == nullptr);
    CHECK(std::string{Host::UnitOf(Host::Kind::ImageBlit)} == "blits");
}

TEST_CASE("Nx2HostGpu prints the top 14 groups by time and counts the rest", "[video_core]") {
    constexpr size_t group_count = 16;
    std::vector<Host::Label> labels;
    std::vector<u64> timestamps{0};
    for (size_t i = 0; i < group_count; ++i) {
        labels.push_back(Host::PassLabel(static_cast<u32>(100 + i), 1, 1, 1, false));
        // Pass i lasts i + 1 ms.
        timestamps.push_back(timestamps.back() + (i + 1) * 1000);
    }
    // The End label's own timestamp is the last one pushed.
    labels.push_back({.kind = Host::Kind::End});
    REQUIRE(timestamps.size() == labels.size());

    Host::Window window;
    window.AddFrame(timestamps, labels, 0, MicrosecondPerTick, 64);
    REQUIRE(window.Groups().size() == group_count);
    const std::string line = window.Format();
    // The longest passes are in, the two shortest are not.
    CHECK(line.find("pass 115x1 c1: 16.00 ms") != std::string::npos);
    CHECK(line.find("pass 102x1 c1: 3.00 ms") != std::string::npos);
    CHECK(line.find("pass 101x1 c1") == std::string::npos);
    CHECK(line.find("pass 100x1 c1") == std::string::npos);
    CHECK(line.find("| 2 more groups: 3.00 ms") != std::string::npos);
    // In order of time: the longest first.
    CHECK(line.find("pass 115x1") < line.find("pass 114x1"));
    CHECK(line.find("pass 103x1") < line.find("pass 102x1"));
}

TEST_CASE("Nx2HostGpu keeps one timestamp for the end of a frame past the cap", "[video_core]") {
    Host::FrameLog log{4};
    CHECK(log.Begin(Host::Label{}) == 0);
    CHECK(log.Begin(Host::Label{.kind = Host::Kind::Compute}) == 1);
    CHECK(log.Begin(Host::Label{.kind = Host::Kind::Compute}) == 2);
    CHECK(log.Begin(Host::Label{}) == Host::FrameLog::NoQuery);
    CHECK(log.Begin(Host::Label{}) == Host::FrameLog::NoQuery);
    CHECK(log.Overflow() == 2);
    CHECK(log.End() == 3);
    CHECK(log.Ended());
    CHECK(log.Used() == 4);
    CHECK(log.End() == Host::FrameLog::NoQuery);
    REQUIRE(log.Labels().size() == 4);
    CHECK(log.Labels()[3].kind == Host::Kind::End);

    log.Clear();
    CHECK(log.Used() == 0);
    CHECK(log.Overflow() == 0);
    CHECK_FALSE(log.Ended());
    CHECK(log.Begin(Host::Label{}) == 0);
}

TEST_CASE("Nx2HostGpu keeps room for the timestamps that close a submission's last one",
          "[video_core]") {
    // A submission's last timestamp is only written if the two that start the next one fit too,
    // otherwise the idle span it opens would never close.
    Host::FrameLog log{6};
    CHECK(log.Begin(Host::Label{.kind = Host::Kind::Idle}, 2) == 0);
    CHECK(log.Begin(Host::Label{.kind = Host::Kind::Idle}, 2) == 1);
    CHECK(log.Begin(Host::Label{.kind = Host::Kind::Idle}, 2) == 2);
    CHECK(log.Begin(Host::Label{.kind = Host::Kind::Idle}, 2) == Host::FrameLog::NoQuery);
    CHECK(log.Overflow() == 1);
    // What closes them still fits, and so does the end of the frame.
    CHECK(log.Begin(Host::Label{.kind = Host::Kind::BufferUpload}) == 3);
    CHECK(log.Begin(Host::Label{}) == 4);
    CHECK(log.Begin(Host::Label{}) == Host::FrameLog::NoQuery);
    CHECK(log.End() == 5);
}

TEST_CASE("Nx2HostGpu reports what a window dropped or skipped", "[video_core]") {
    const Frame frame;
    Host::Window window;
    window.NoteSkipped();
    // Nothing profiled yet: no line, but the window is not empty.
    CHECK(window.Format().empty());
    CHECK_FALSE(window.Empty());

    window.AddFrame(frame.timestamps, frame.labels, 5, MicrosecondPerTick, 64);
    window.NoteSkipped();
    const std::string line = window.Format();
    CHECK(line.find("| 5 timestamps over the 4096 a frame keep") != std::string::npos);
    CHECK(line.find("| 2 frames not profiled") != std::string::npos);

    window.Clear();
    CHECK(window.Empty());
    CHECK(window.Frames() == 0);
    CHECK(window.Submits() == 0);
}

TEST_CASE("Nx2HostGpu ignores a frame without a span and follows the counter's wrap",
          "[video_core]") {
    Host::Window window;
    const std::array<u64, 1> one_timestamp{5};
    const std::array<Host::Label, 1> one_label{Host::Label{.kind = Host::Kind::End}};
    window.AddFrame(one_timestamp, one_label, 0, MicrosecondPerTick, 64);
    CHECK(window.Frames() == 0);

    // An 8-bit counter that wrapped between the two: 250 to 4 is 10 ticks.
    const std::array<u64, 2> wrapped{250, 4};
    const std::array<Host::Label, 2> labels{Host::Label{}, Host::Label{.kind = Host::Kind::End}};
    window.AddFrame(wrapped, labels, 0, MicrosecondPerTick, 8);
    REQUIRE(window.Frames() == 1);
    CHECK(window.TotalNs() == 10'000.0);
    CHECK(window.FirstToLastNs() == 10'000.0);
}
