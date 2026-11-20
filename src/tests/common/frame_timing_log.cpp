// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/frame_timing_log.h"

namespace {

namespace FT = Common::FrameTiming;
namespace fs = std::filesystem;

// A private directory per test, removed afterwards. Public tests never touch a fixed path.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("eglantine_frame_timing_" + std::to_string(stamp) + "_" +
                 std::to_string(counter.fetch_add(1)));
        fs::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        // A failed check must not leave a running log for the next test.
        if (FT::detail::Active()) {
            std::string ignored;
            (void)FT::Stop(false, ignored);
        }
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] std::string File(const std::string& name) const {
        return (path_ / name).string();
    }

private:
    fs::path path_;
};

std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct ParsedLog {
    bool valid = false;
    std::string why;
    std::string source;
    u64 capacity = 0;
    std::vector<std::pair<u64, u64>> samples;
    u64 recorded = 0;
    u64 lost = 0;
    bool complete = false;
};

// The reader a consumer must implement: a file is valid only with a correct header, well formed
// sample lines, an end line that is the last line and counts that agree with the samples.
ParsedLog Parse(const std::string& text) {
    ParsedLog log;
    if (text.empty() || text.back() != '\n') {
        log.why = "does not end in a newline";
        return log;
    }
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t end = text.find('\n', begin);
        lines.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    if (lines.size() < 2) {
        log.why = "too short";
        return log;
    }
    char source[32] = {};
    unsigned long long capacity = 0;
    if (std::sscanf(lines.front().c_str(),
                    "{\"schema\":1,\"kind\":\"frame_times\",\"source\":\"%31[a-z_]\","
                    "\"clock\":\"steady_ns\",\"capacity\":%llu}",
                    source, &capacity) != 2) {
        log.why = "bad header";
        return log;
    }
    log.source = source;
    log.capacity = capacity;
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
        unsigned long long id = 0;
        unsigned long long time = 0;
        if (std::sscanf(lines[i].c_str(), "{\"id\":%llu,\"t\":%llu}", &id, &time) != 2) {
            log.why = "bad sample line";
            return log;
        }
        log.samples.emplace_back(id, time);
    }
    char flag[8] = {};
    unsigned long long recorded = 0;
    unsigned long long lost = 0;
    if (std::sscanf(lines.back().c_str(),
                    "{\"end\":true,\"recorded\":%llu,\"lost\":%llu,\"complete\":%7[a-z]}",
                    &recorded, &lost, flag) != 3) {
        log.why = "no end line";
        return log;
    }
    if (recorded != log.samples.size()) {
        log.why = "recorded count does not match the samples";
        return log;
    }
    log.recorded = recorded;
    log.lost = lost;
    log.complete = std::string(flag) == "true";
    log.valid = true;
    return log;
}

FT::Config MakeConfig(const TempDir& dir, u64 capacity, FT::Source source = FT::Source::HostPresent) {
    FT::Config config;
    config.path = dir.File("frames.jsonl");
    config.capacity = capacity;
    config.source = source;
    return config;
}

} // Anonymous namespace

TEST_CASE("Frame timing: a bad configuration fails and leaves nothing behind",
          "[common][frame_timing]") {
    const TempDir dir;
    std::string error;

    FT::Config config = MakeConfig(dir, 8);
    config.path = dir.File("frames.txt");
    REQUIRE_FALSE(FT::Start(config, error));
    REQUIRE_FALSE(error.empty());

    config = MakeConfig(dir, 0);
    REQUIRE_FALSE(FT::Start(config, error));
    REQUIRE_FALSE(error.empty());

    config = MakeConfig(dir, FT::MaxCapacity + 1);
    REQUIRE_FALSE(FT::Start(config, error));
    REQUIRE_FALSE(error.empty());

    config = MakeConfig(dir, 8);
    config.source = static_cast<FT::Source>(7);
    REQUIRE_FALSE(FT::Start(config, error));
    REQUIRE_FALSE(error.empty());

    config = MakeConfig(dir, 8);
    config.path = dir.File("missing_dir/frames.jsonl");
    REQUIRE_FALSE(FT::Start(config, error));
    REQUIRE_FALSE(error.empty());

    // An existing target is never replaced, and Start does not touch it.
    config = MakeConfig(dir, 8);
    {
        std::ofstream existing(config.path, std::ios::binary);
        existing << "keep";
    }
    REQUIRE_FALSE(FT::Start(config, error));
    REQUIRE(ReadAll(config.path) == "keep");

    REQUIRE_FALSE(fs::exists(config.path + ".partial"));
    REQUIRE_FALSE(FT::detail::Active());
}

TEST_CASE("Frame timing: a normal run writes header, samples and a complete trailer",
          "[common][frame_timing]") {
    const TempDir dir;
    std::string error;
    const FT::Config config = MakeConfig(dir, 8);
    REQUIRE(FT::Start(config, error));
    REQUIRE(error.empty());
    REQUIRE(FT::detail::Active());
    REQUIRE(fs::exists(config.path + ".partial"));
    REQUIRE_FALSE(fs::exists(config.path));

    // A second log while one runs is refused and does not disturb the first.
    FT::Config other = MakeConfig(dir, 4);
    other.path = dir.File("other.jsonl");
    REQUIRE_FALSE(FT::Start(other, error));
    REQUIRE_FALSE(error.empty());

    FT::Record(100);
    FT::Record(101);
    FT::Record(105);
    const FT::Summary live = FT::GetSummary();
    REQUIRE(live.started);
    REQUIRE_FALSE(live.stopped);
    REQUIRE(live.recorded == 3U);
    REQUIRE(live.lost == 0U);

    const FT::Summary summary = FT::Stop(true, error);
    REQUIRE(error.empty());
    REQUIRE_FALSE(FT::detail::Active());
    REQUIRE(summary.started);
    REQUIRE(summary.stopped);
    REQUIRE(summary.source == FT::Source::HostPresent);
    REQUIRE(summary.recorded == 3U);
    REQUIRE(summary.lost == 0U);
    REQUIRE(summary.file_written);
    REQUIRE(summary.complete);
    REQUIRE(summary.first_ns != 0U);
    REQUIRE(summary.last_ns >= summary.first_ns);

    REQUIRE(fs::exists(config.path));
    REQUIRE_FALSE(fs::exists(config.path + ".partial"));
    const std::string text = ReadAll(config.path);
    const ParsedLog parsed = Parse(text);
    INFO(parsed.why);
    REQUIRE(parsed.valid);
    REQUIRE(parsed.source == "host_present");
    REQUIRE(parsed.capacity == 8U);
    REQUIRE(parsed.samples.size() == 3U);
    REQUIRE(parsed.samples[0].first == 100U);
    REQUIRE(parsed.samples[1].first == 101U);
    REQUIRE(parsed.samples[2].first == 105U);
    REQUIRE(parsed.samples[1].second >= parsed.samples[0].second);
    REQUIRE(parsed.samples[2].second >= parsed.samples[1].second);
    REQUIRE(parsed.samples.front().second == summary.first_ns);
    REQUIRE(parsed.samples.back().second == summary.last_ns);
    REQUIRE(parsed.lost == 0U);
    REQUIRE(parsed.complete);
    // The documented text, byte for byte, for the fixed parts.
    REQUIRE(text.rfind("{\"schema\":1,\"kind\":\"frame_times\",\"source\":\"host_present\","
                       "\"clock\":\"steady_ns\",\"capacity\":8}\n",
                       0) == 0U);
    const std::string trailer = "{\"end\":true,\"recorded\":3,\"lost\":0,\"complete\":true}\n";
    REQUIRE(text.size() > trailer.size());
    REQUIRE(text.compare(text.size() - trailer.size(), trailer.size(), trailer) == 0);

    // A Record after Stop is ignored, and Stop twice reports instead of rewriting.
    FT::Record(999);
    const FT::Summary again = FT::Stop(true, error);
    REQUIRE_FALSE(error.empty());
    REQUIRE(again.recorded == 3U);
    REQUIRE(ReadAll(config.path) == text);
    REQUIRE(FT::GetSummary().recorded == 3U);
}

TEST_CASE("Frame timing: loss is counted and marks the log incomplete", "[common][frame_timing]") {
    const TempDir dir;
    std::string error;
    const FT::Config config = MakeConfig(dir, 4, FT::Source::ReplayFrame);
    REQUIRE(FT::Start(config, error));
    for (u64 id = 0; id < 7; ++id) {
        FT::Record(id);
    }
    const FT::Summary live = FT::GetSummary();
    REQUIRE(live.recorded == 4U);
    REQUIRE(live.lost == 3U);

    const FT::Summary summary = FT::Stop(true, error);
    REQUIRE(error.empty());
    REQUIRE(summary.source == FT::Source::ReplayFrame);
    REQUIRE(summary.recorded == 4U);
    REQUIRE(summary.lost == 3U);
    REQUIRE(summary.file_written);
    REQUIRE_FALSE(summary.complete);

    const ParsedLog parsed = Parse(ReadAll(config.path));
    INFO(parsed.why);
    REQUIRE(parsed.valid);
    REQUIRE(parsed.source == "replay_frame");
    REQUIRE(parsed.samples.size() == 4U);
    REQUIRE(parsed.samples.back().first == 3U);
    REQUIRE(parsed.recorded == 4U);
    REQUIRE(parsed.lost == 3U);
    REQUIRE_FALSE(parsed.complete);
}

TEST_CASE("Frame timing: a run that did not end normally is written but incomplete",
          "[common][frame_timing]") {
    const TempDir dir;
    std::string error;
    const FT::Config config = MakeConfig(dir, 4);
    REQUIRE(FT::Start(config, error));
    FT::Record(1);
    FT::Record(2);
    const FT::Summary summary = FT::Stop(false, error);
    REQUIRE(error.empty());
    REQUIRE(summary.recorded == 2U);
    REQUIRE(summary.lost == 0U);
    REQUIRE(summary.file_written);
    REQUIRE_FALSE(summary.complete);

    const ParsedLog parsed = Parse(ReadAll(config.path));
    REQUIRE(parsed.valid);
    REQUIRE(parsed.lost == 0U);
    REQUIRE_FALSE(parsed.complete);
}

TEST_CASE("Frame timing: an empty run is a valid file", "[common][frame_timing]") {
    const TempDir dir;
    std::string error;
    const FT::Config config = MakeConfig(dir, 4);
    REQUIRE(FT::Start(config, error));
    const FT::Summary summary = FT::Stop(true, error);
    REQUIRE(summary.recorded == 0U);
    REQUIRE(summary.first_ns == 0U);
    REQUIRE(summary.last_ns == 0U);
    const ParsedLog parsed = Parse(ReadAll(config.path));
    REQUIRE(parsed.valid);
    REQUIRE(parsed.samples.empty());
}

TEST_CASE("Frame timing: a truncated or damaged file is always detectable",
          "[common][frame_timing]") {
    const TempDir dir;
    std::string error;
    const FT::Config config = MakeConfig(dir, 16);
    REQUIRE(FT::Start(config, error));
    for (u64 id = 0; id < 5; ++id) {
        FT::Record(id);
    }
    REQUIRE(FT::Stop(true, error).file_written);
    const std::string text = ReadAll(config.path);
    REQUIRE(Parse(text).valid);

    // Every proper prefix, down to one byte, is rejected.
    for (std::size_t length = 0; length < text.size(); ++length) {
        const ParsedLog cut = Parse(text.substr(0, length));
        INFO(length);
        REQUIRE_FALSE(cut.valid);
    }

    // A dropped sample line leaves a count that disagrees with the trailer.
    const std::size_t first_end = text.find('\n');
    const std::size_t second_end = text.find('\n', first_end + 1);
    std::string dropped = text;
    dropped.erase(first_end + 1, second_end - first_end);
    REQUIRE_FALSE(Parse(dropped).valid);

    // A damaged line is rejected.
    std::string damaged = text;
    damaged.replace(first_end + 1, 6, "{\"id\":x");
    REQUIRE_FALSE(Parse(damaged).valid);

    // A trailer in the middle (extra data after it) is not accepted as the end.
    REQUIRE_FALSE(Parse(text + "{\"id\":9,\"t\":9}\n").valid);
}

TEST_CASE("Frame timing: the final name is never replaced", "[common][frame_timing]") {
    const TempDir dir;
    std::string error;
    const FT::Config config = MakeConfig(dir, 4);
    REQUIRE(FT::Start(config, error));
    FT::Record(1);
    // Something else claims the name while the log runs.
    {
        std::ofstream intruder(config.path, std::ios::binary);
        intruder << "intruder";
    }
    const FT::Summary summary = FT::Stop(true, error);
    REQUIRE_FALSE(error.empty());
    REQUIRE_FALSE(summary.file_written);
    REQUIRE_FALSE(summary.complete);
    REQUIRE(ReadAll(config.path) == "intruder");
    REQUIRE(fs::exists(config.path + ".partial"));
    REQUIRE(Parse(ReadAll(config.path + ".partial")).valid);
}

TEST_CASE("Frame timing: publishing never replaces a file and says so", "[common][frame_timing]") {
    const TempDir dir;
    const std::string partial = dir.File("a.jsonl.partial");
    const std::string target = dir.File("a.jsonl");
    {
        std::ofstream out(partial, std::ios::binary);
        out << "new";
    }
    // The name is taken at the moment of the rename (the window the old exists()-then-rename left).
    {
        std::ofstream out(target, std::ios::binary);
        out << "old";
    }
    REQUIRE(FT::detail::PublishNoReplace(partial, target) == FT::detail::PublishResult::Exists);
    REQUIRE(ReadAll(target) == "old");
    REQUIRE(ReadAll(partial) == "new");

    // A free name is published and the partial name is gone.
    const std::string free_name = dir.File("b.jsonl");
    REQUIRE(FT::detail::PublishNoReplace(partial, free_name) ==
            FT::detail::PublishResult::Published);
    REQUIRE(ReadAll(free_name) == "new");
    REQUIRE_FALSE(fs::exists(partial));

    // A missing source is a failure, not "exists".
    REQUIRE(FT::detail::PublishNoReplace(partial, dir.File("c.jsonl")) ==
            FT::detail::PublishResult::Failed);
}

TEST_CASE("Frame timing: a forced finish leaves a readable incomplete log, once",
          "[common][frame_timing]") {
    const TempDir dir;
    std::string error;
    // Nothing started: nothing written.
    REQUIRE_FALSE(FT::StopIncomplete(error));
    REQUIRE_FALSE(error.empty());

    const FT::Config config = MakeConfig(dir, 4);
    REQUIRE(FT::Start(config, error));
    FT::Record(1);
    FT::Record(2);
    REQUIRE(FT::StopIncomplete(error));
    REQUIRE(error.empty());
    const ParsedLog parsed = Parse(ReadAll(config.path));
    REQUIRE(parsed.valid);
    REQUIRE(parsed.recorded == 2U);
    REQUIRE_FALSE(parsed.complete);
    const std::string written = ReadAll(config.path);

    // A second call, and a normal Stop after it, do nothing and leave the file as it was.
    REQUIRE_FALSE(FT::StopIncomplete(error));
    REQUIRE_FALSE(error.empty());
    (void)FT::Stop(true, error);
    REQUIRE_FALSE(error.empty());
    REQUIRE(ReadAll(config.path) == written);
    const FT::Summary summary = FT::GetSummary();
    REQUIRE(summary.file_written);
    REQUIRE_FALSE(summary.complete);
}

TEST_CASE("Frame timing: concurrent recorders lose no sample inside the capacity",
          "[common][frame_timing]") {
    const TempDir dir;
    std::string error;
    constexpr u64 threads = 4;
    constexpr u64 per_thread = 500;
    const FT::Config config = MakeConfig(dir, threads * per_thread);
    REQUIRE(FT::Start(config, error));
    std::vector<std::thread> workers;
    for (u64 t = 0; t < threads; ++t) {
        workers.emplace_back([t] {
            for (u64 i = 0; i < per_thread; ++i) {
                FT::Record(t * per_thread + i);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const FT::Summary summary = FT::Stop(true, error);
    REQUIRE(error.empty());
    REQUIRE(summary.recorded == threads * per_thread);
    REQUIRE(summary.lost == 0U);
    REQUIRE(summary.complete);

    const ParsedLog parsed = Parse(ReadAll(config.path));
    REQUIRE(parsed.valid);
    std::vector<char> seen(threads * per_thread, 0);
    for (const auto& sample : parsed.samples) {
        REQUIRE(sample.first < threads * per_thread);
        REQUIRE(seen[static_cast<std::size_t>(sample.first)] == 0);
        seen[static_cast<std::size_t>(sample.first)] = 1;
    }
}
