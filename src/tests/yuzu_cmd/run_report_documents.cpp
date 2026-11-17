// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/baseline_settings.h"
#include "common/run_outcome.h"
#include "yuzu_cmd/run_report.h"

// The two documents of --report, whole: the normal report and the crash report. Both read the
// process environment and the baseline object, so what they must not carry is checked on the text
// they produce and not on the pieces. The report keeps process-wide state: one run per process,
// so the fixture below prepares it once for every test case of this file.

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::string_view kSentinel = "sentinel-dump-path-9f3a";

void SetVariable(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

struct Fixture {
    Fixture() {
        std::error_code ec;
        directory = fs::temp_directory_path(ec) / "eglantine-run-report-test";
        fs::remove_all(directory, ec);
        fs::create_directories(directory, ec);
        // A relative path can look like a number or a word, which is the trap.
        SetVariable("EDEN_NX2_DIAG_DIR", "123");
        SetVariable("EDEN_NX2_FRAME_CAPTURE", "on");
        SetVariable("EDEN_NX2_SHADER_DUMP", kSentinel.data());
        SetVariable("EDEN_NX2_ABLATE_FILE", "123");
        SetVariable("EDEN_NX2_SPLIT", "2");
        std::string message;
        prepared = RunReport::Prepare((directory / "report.json").string(), message) ==
                   RunReport::PrepareResult::Ready;
        RunReport::SetBaselineRequest(false, {});
        RunReport::CaptureStart();
    }

    ~Fixture() {
        // Writes the normal report, which removes the crash report's file.
        (void)RunReport::Publish(Common::RunOutcome::Reason::GuestExit, 0, true, "");
        std::error_code ec;
        fs::remove_all(directory, ec);
    }

    fs::path directory;
    bool prepared = false;
};

Fixture& GetFixture() {
    static Fixture fixture;
    return fixture;
}

/// Walks the document: every member that is a path variable, and every deviation that names one,
/// carries "set" and nothing else.
void CheckPathValuesHidden(const Json& node) {
    if (node.is_object()) {
        for (const auto& [key, member] : node.items()) {
            if (Settings::Baseline::IsPathSwitch(key) && member.is_object()) {
                CHECK(member.value("raw", "") == "set");
            }
            CheckPathValuesHidden(member);
        }
        if (node.contains("key") && node["key"].is_string() &&
            Settings::Baseline::IsPathSwitch(node["key"].get<std::string>())) {
            CHECK(node.value("effective", "") == "set");
        }
    } else if (node.is_array()) {
        for (const auto& member : node) {
            CheckPathValuesHidden(member);
        }
    }
}

void CheckDocument(const std::string& text) {
    REQUIRE_FALSE(text.empty());
    CHECK(text.find(kSentinel) == std::string::npos);
    const Json document = Json::parse(text);
    const Json& tunables = document["tunables"];
    for (const char* name : {"EDEN_NX2_DIAG_DIR", "EDEN_NX2_FRAME_CAPTURE", "EDEN_NX2_SHADER_DUMP",
                             "EDEN_NX2_ABLATE_FILE"}) {
        REQUIRE(tunables.contains(name));
        CHECK(tunables[name]["raw"] == "set");
    }
    // A plain number of an ordinary variable is still reported.
    REQUIRE(tunables.contains("EDEN_NX2_SPLIT"));
    CHECK(tunables["EDEN_NX2_SPLIT"]["raw"] == "2");
    CheckPathValuesHidden(document);

    // The deviation of an ordinary switch is listed, with its value hidden when it is a path.
    bool ablate_listed = false;
    for (const auto& deviation : document["baseline"]["deviations"]) {
        if (deviation["key"] == "EDEN_NX2_ABLATE_FILE") {
            ablate_listed = true;
            CHECK(deviation["effective"] == "set");
        }
    }
    CHECK(ablate_listed);
}

bool ListsAblation(const std::string& text) {
    const Json document = Json::parse(text);
    for (const auto& deviation : document["baseline"]["deviations"]) {
        if (deviation["key"] == "shader_ablation") {
            return true;
        }
    }
    return false;
}

/// Makes shader ablation active from another thread, in the window between the snapshot having
/// evaluated the baseline and having published its text. The other thread cannot take the report's
/// lock then: the request must not be lost.
void AblationFromRendererThread() {
    RunReport::SetSnapshotTestHook(nullptr);
    std::thread renderer{[] { Settings::Baseline::NoteShaderAblation(); }};
    renderer.join();
}

} // Anonymous namespace

TEST_CASE("The normal report names no value of a path variable", "[run_report]") {
    REQUIRE(GetFixture().prepared);
    CheckDocument(RunReport::NormalDocumentText(Common::RunOutcome::Reason::GuestExit, 0, true, ""));
}

TEST_CASE("The crash report names no value of a path variable", "[run_report]") {
    REQUIRE(GetFixture().prepared);
    const std::string text = RunReport::CrashDocumentText("test");
    CheckDocument(text);
    CHECK(Json::parse(text)["exit"]["reason"] == "host_crash");
}

TEST_CASE("The crash snapshot shows shader ablation that began during a rebuild", "[run_report]") {
    REQUIRE(GetFixture().prepared);
    Settings::Baseline::ClearNotedSources();
    RunReport::RefreshCrashSnapshot();
    REQUIRE_FALSE(ListsAblation(RunReport::CrashDocumentText("test")));

    // The hook runs after the snapshot evaluated the baseline and before it is published: the
    // text it publishes was formatted without the ablation, and the rebuild that follows the
    // unlock must put it right.
    RunReport::SetSnapshotTestHook(&AblationFromRendererThread);
    RunReport::RefreshCrashSnapshot();
    RunReport::SetSnapshotTestHook(nullptr);
    CHECK(Settings::Baseline::ShaderAblationActive());
    CHECK(ListsAblation(RunReport::CrashDocumentText("test")));

    // The normal report agrees.
    CHECK(ListsAblation(
        RunReport::NormalDocumentText(Common::RunOutcome::Reason::GuestExit, 0, true, "")));
    Settings::Baseline::ClearNotedSources();
}

TEST_CASE("Shader ablation that begins while nothing is rebuilding reaches the crash snapshot",
          "[run_report]") {
    REQUIRE(GetFixture().prepared);
    Settings::Baseline::ClearNotedSources();
    RunReport::RefreshCrashSnapshot();
    REQUIRE_FALSE(ListsAblation(RunReport::CrashDocumentText("test")));
    Settings::Baseline::NoteShaderAblation();
    CHECK(ListsAblation(RunReport::CrashDocumentText("test")));
    Settings::Baseline::ClearNotedSources();
    RunReport::RefreshCrashSnapshot();
}
