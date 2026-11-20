// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "core/arm/arm_interface.h"
#include "core/hle/guest_call/nvn_census.h"
#include "core/hle/guest_call/nvn_census_verdict.h"

namespace {
using namespace Core::NvnCensus;

/// Inputs of a census that is complete: every condition holds with a margin of none.
CoverageInputs Complete() {
    CoverageInputs in;
    in.final_report = true;
    in.run_complete = true;
    in.audit_enabled = true;
    in.audit_region_known = true;
    in.bootstrap_sites = 1;
    in.bootstrap_calls = 1;
    in.named_targets = 700;
    in.audit_targets = 12;
    in.audit_targets_entry_counted = 12;
    in.instrumented_targets = 712;
    in.audited_entries = 500;
    in.accounting_exact = true;
    in.executed = 900;
    in.startup = 100;
    in.frames = 800;
    in.paths_used = 1;
    in.streams = 1;
    in.stream_tokens = 1;
    in.identity_ok = true;
    in.presents = 201;
    in.frames_closed = 200;
    in.min_frames = 120;
    return in;
}

bool Failed(const Verdict& verdict, const std::string& name) {
    return std::find(verdict.failed.begin(), verdict.failed.end(), name) != verdict.failed.end();
}
} // namespace

TEST_CASE("NVN census verdict: a call instruction is BL, BLR or an authenticated BLR", "[core][nvn_census]") {
    CHECK(IsCallInstruction(0x94000000U));   // BL #0
    CHECK(IsCallInstruction(0x97FFFFFFU));   // BL #-4
    CHECK(IsCallInstruction(0xD63F0100U));   // BLR x8
    CHECK(IsCallInstruction(0xD63F0200U));   // BLR x16
    CHECK(IsCallInstruction(0xD63F091FU));   // BLRAAZ x8
    CHECK(IsCallInstruction(0xD63F0D1FU));   // BLRABZ x8
    CHECK(IsCallInstruction(0xD73F0909U));   // BLRAA x8, x9
    CHECK(IsCallInstruction(0xD73F0D09U));   // BLRAB x8, x9
    CHECK_FALSE(IsCallInstruction(0x14000000U)); // B #0: a tail branch is not a call
    CHECK_FALSE(IsCallInstruction(0xD61F0100U)); // BR x8
    CHECK_FALSE(IsCallInstruction(0xD65F03C0U)); // RET
    CHECK_FALSE(IsCallInstruction(0xD503201FU)); // NOP
    CHECK_FALSE(IsCallInstruction(0x54000000U)); // B.EQ
    CHECK_FALSE(IsCallInstruction(0xD63F0101U)); // BLR with a stray low bit set is not that encoding
}

TEST_CASE("NVN census verdict: a RET landing after a call is a return site, any other word is an entry",
          "[core][nvn_census]") {
    CHECK(ClassifyReturn(0x94001234U) == EntryClass::ReturnSite);
    CHECK(ClassifyReturn(0xD63F0120U) == EntryClass::ReturnSite);
    CHECK(ClassifyReturn(0xD65F03C0U) == EntryClass::Entry); // the word before is itself a RET: a function start
    CHECK(ClassifyReturn(0x14000010U) == EntryClass::Entry);
    CHECK(ClassifyReturn(0U) == EntryClass::Entry);
}

TEST_CASE("NVN census verdict: a census whose conditions all hold is complete and gives no reasons",
          "[core][nvn_census]") {
    const Verdict verdict = EvaluateCoverage(Complete());
    CHECK(verdict.complete);
    CHECK(verdict.failed.empty());
    CHECK(verdict.reasons.empty());
    CHECK(verdict.conditions.size() == 11);
    CHECK(verdict.resolved_share == 1.0);
    CHECK(verdict.instrumented_share == 1.0);
    for (const Condition& condition : verdict.conditions) {
        INFO(condition.name);
        CHECK(condition.ok);
    }
}

TEST_CASE("NVN census verdict: entries nobody could attribute make it incomplete, with the numbers as the reason",
          "[core][nvn_census]") {
    CoverageInputs in = Complete();
    in.unknown_targets = 82;
    in.unknown_calls = 2194378;
    const Verdict verdict = EvaluateCoverage(in);
    CHECK_FALSE(verdict.complete);
    REQUIRE(verdict.failed.size() == 3);
    CHECK(verdict.failed[0] == "every_target_resolved");
    CHECK(verdict.failed[1] == "every_target_instrumented");
    CHECK(verdict.failed[2] == "no_unknown_dispatch");
    REQUIRE(verdict.reasons.size() == 3);
    CHECK(verdict.reasons[0] ==
          "every_target_resolved: audit=true resolved_share=0.896725 (700 named, 12 found by the audit, 82 unknown)");
    CHECK(verdict.reasons[2] ==
          "no_unknown_dispatch: audit=true unknown_targets=82 unknown_calls=2194378 overflow=0 unaudited=0");
}

TEST_CASE("NVN census verdict: entries the audit found and gave a live entry probe do not fail it",
          "[core][nvn_census]") {
    CoverageInputs in = Complete();
    in.named_targets = 0;
    in.audit_targets = 40;
    in.audit_targets_entry_counted = 40;
    in.instrumented_targets = 40;
    const Verdict verdict = EvaluateCoverage(in);
    CHECK(verdict.complete);
    CHECK(verdict.resolved_share == 1.0);
}

TEST_CASE("NVN census verdict: an audit-found function counted only at the branch is never complete",
          "[core][nvn_census]") {
    // Counted at the branch it sees the calls from outside the library, not the library's own entries
    // into it. Every other figure is perfect, so only this class can make the census incomplete.
    CoverageInputs in = Complete();
    in.audit_targets = 40;
    in.audit_targets_entry_counted = 39;
    in.instrumented_targets = 740; // acknowledged, and still not counted at its entry
    const Verdict verdict = EvaluateCoverage(in);
    CHECK_FALSE(verdict.complete);
    REQUIRE(verdict.failed.size() == 1);
    CHECK(verdict.failed[0] == "audit_found_entries_counted_at_entry");
    REQUIRE(verdict.reasons.size() == 1);
    CHECK(verdict.reasons[0] == "audit_found_entries_counted_at_entry: audit_found=40 counted_at_entry=39");

    in.audit_targets_entry_counted = 0; // the old census: found at the branch, never probed
    const Verdict none = EvaluateCoverage(in);
    CHECK_FALSE(none.complete);
    CHECK(Failed(none, "audit_found_entries_counted_at_entry"));
}

TEST_CASE("NVN census keeps sparse rows across a late capture window and reports overflow",
          "[core][nvn_census]") {
    u64 last{};
    SECTION("late selected frames") { last = 7900; }
    SECTION("first dropped row") { last = 8193; }
    testing::Begin(1);
    for (u64 frame = 1; frame <= last; ++frame) {
        testing::Call(0);
        NotePresent(PresentPath::BufferQueue, 1, 0, frame);
        NoteAcceptedPresent(frame);
    }
    const auto report = nlohmann::json::parse(testing::EndReport());
    const auto& rows = report.at("frame_rows");
    const auto& frames = report.at("frames");
    const auto dropped = last == 8193 ? 1 : 0;
    REQUIRE(rows.size() == last - dropped);
    CHECK(frames.at("rows_recorded") == rows.size());
    CHECK(frames.at("rows_dropped") == dropped);
    CHECK(report.at("errors").at("frame_rows_dropped") == dropped);
    CHECK(frames.at("rows_joinable") == (dropped == 0));
    const auto& conditions = report.at("coverage").at("conditions");
    CHECK(conditions.at("no_dropped_or_lost_events").at("ok") == (dropped == 0));
    CHECK(report.at("accounting").at("executed_total") == last);
    if (dropped != 0) {
        CHECK(report.at("coverage_complete") == false);
    }
    for (u64 id = 7800; id <= 7819; ++id) {
        CHECK(rows.at(id - 1) == nlohmann::json::array({id, {{0, 1}}}));
    }
}

TEST_CASE("NVN census dispatch shortfall cannot cancel against calls to another function",
          "[core][nvn_census]") {
    bool missing{};
    SECTION("matched dispatch") { missing = false; }
    SECTION("one shortfall despite matching totals") { missing = true; }
    testing::Begin(2);
    Core::CoreRunState state;
    Core::ScopedRun run{state, 7};
    testing::Call(0);
    testing::Call(1);
    testing::Call(1);
    testing::AuditBranch(0);
    testing::AuditBranch(1);
    testing::AuditBranch(missing ? 0 : 1);
    const auto report = nlohmann::json::parse(testing::EndReport());
    const auto& audit = report.at("audit");
    REQUIRE(report.at("guest_threads") == nlohmann::json::array({{{"id", 7}, {"calls", 3}}}));
    u64 calls = 0, entries = 0, short_calls = 0, short_functions = 0;
    for (const auto& function : report.at("functions")) {
        const auto count = function.at("calls").get<u64>();
        const auto audited = function.at("audited_entries").get<u64>();
        calls += count;
        entries += audited;
        if (audited > count) {
            short_calls += audited - count;
            ++short_functions;
        }
    }
    REQUIRE(calls == 3);
    REQUIRE(entries == calls);
    CHECK(report.at("accounting").at("executed_total") == calls);
    CHECK(audit.at("known_entries") == entries);
    CHECK(short_calls == (missing ? 1 : 0));
    CHECK(audit.at("mismatched_calls") == short_calls);
    CHECK(audit.at("mismatched_functions") == short_functions);
    CHECK(report.at("coverage").at("conditions").at("audit_matches_counts").at("ok") == !missing);
}

TEST_CASE("NVN census verdict: a single audited entry missing at dispatch fails closed", "[core][nvn_census]") {
    CoverageInputs in = Complete();
    in.audit_shortfall_calls = 1;
    in.audit_shortfall_functions = 1;
    const Verdict verdict = EvaluateCoverage(in);
    CHECK_FALSE(verdict.complete);
    REQUIRE(verdict.failed.size() == 1);
    CHECK(verdict.failed.front() == "audit_matches_counts");
}

TEST_CASE("NVN census verdict: one fault at a time", "[core][nvn_census]") {
    CoverageInputs in = Complete();
    const char* gate = nullptr;
    SECTION("late library") { ++in.late_modules; gate = "bootstrap_found_before_first_request"; }
    SECTION("no bootstrap call") { --in.bootstrap_calls; gate = "bootstrap_found_before_first_request"; }
    SECTION("unknown resolver call") { ++in.unknown_calls; gate = "no_unknown_dispatch"; }
    SECTION("probe not installed") { --in.instrumented_targets; gate = "every_target_instrumented"; }
    SECTION("late frame call") { ++in.accounting_late; gate = "frame_accounting_exact"; }
    SECTION("missing startup call") { --in.startup; gate = "frame_accounting_exact"; }
    SECTION("duplicated present") {
        ObservePresentId(8, 8, in.id_duplicates, in.id_gaps);
        CHECK(in.id_duplicates == 1);
        gate = "presents_consistent";
    }
    SECTION("out of order present") {
        ObservePresentId(8, 7, in.id_duplicates, in.id_gaps);
        CHECK(in.id_gaps == 1);
        gate = "presents_consistent";
    }
    const Verdict verdict = EvaluateCoverage(in);
    CHECK_FALSE(verdict.complete);
    REQUIRE(verdict.failed.size() == 1);
    CHECK(verdict.failed.front() == gate);
}

TEST_CASE("NVN census verdict: a function entered more often than the audit saw is not a mismatch",
          "[core][nvn_census]") {
    // The audit counts branches from outside, the counter counts every entry: a function that the
    // library also tail-branches into has more counted calls than audited ones. That is shortfall 0.
    CoverageInputs in = Complete();
    in.executed = 9000;
    in.frames = 8900;
    in.audited_entries = 10;
    CHECK(EvaluateCoverage(in).complete);
}

TEST_CASE("NVN census verdict: without the audit the census cannot be complete", "[core][nvn_census]") {
    CoverageInputs in = Complete();
    in.audit_enabled = false;
    in.audit_region_known = false;
    in.audited_entries = 0;
    const Verdict verdict = EvaluateCoverage(in);
    CHECK_FALSE(verdict.complete);
    CHECK(Failed(verdict, "every_target_resolved"));
    CHECK(Failed(verdict, "no_unknown_dispatch"));
    CHECK(Failed(verdict, "audit_matches_counts"));
    CHECK_FALSE(Failed(verdict, "every_target_instrumented"));
}

TEST_CASE("NVN census verdict: a report written while the game runs, or after an unclean stop, is never complete",
          "[core][nvn_census]") {
    CoverageInputs running = Complete();
    running.final_report = false;
    const Verdict a = EvaluateCoverage(running);
    CHECK_FALSE(a.complete);
    REQUIRE(a.failed.size() == 1);
    CHECK(a.failed[0] == "not_a_final_report");
    CHECK(a.reasons[0] == "not_a_final_report: a report written while the game runs is never complete");

    CoverageInputs crashed = Complete();
    crashed.run_complete = false;
    const Verdict b = EvaluateCoverage(crashed);
    CHECK_FALSE(b.complete);
    REQUIRE(b.failed.size() == 1);
    CHECK(b.failed[0] == "run_not_complete");
}

TEST_CASE("NVN census verdict: frames, present identity and lost events each fail their own condition",
          "[core][nvn_census]") {
    CoverageInputs few = Complete();
    few.frames_closed = 119;
    few.presents = 120;
    const Verdict a = EvaluateCoverage(few);
    REQUIRE(a.failed.size() == 1);
    CHECK(a.failed[0] == "enough_frames");

    CoverageInputs two_paths = Complete();
    two_paths.paths_used = 2;
    const Verdict b = EvaluateCoverage(two_paths);
    REQUIRE(b.failed.size() == 1);
    CHECK(b.failed[0] == "one_present_stream");

    CoverageInputs lost = Complete();
    lost.lost_events = 2;
    lost.lost_detail = "rendezvous_timeouts=2 ";
    const Verdict c = EvaluateCoverage(lost);
    REQUIRE(c.failed.size() == 1);
    CHECK(c.failed[0] == "no_dropped_or_lost_events");
    CHECK(c.reasons[0] == "no_dropped_or_lost_events: rendezvous_timeouts=2 ");

    CoverageInputs inexact = Complete();
    inexact.accounting_exact = false;
    inexact.accounting_mismatched = 1;
    const Verdict d = EvaluateCoverage(inexact);
    REQUIRE(d.failed.size() == 1);
    CHECK(d.failed[0] == "frame_accounting_exact");
}

TEST_CASE("NVN census verdict: nothing discovered gives no share and no completeness", "[core][nvn_census]") {
    CoverageInputs in = Complete();
    in.named_targets = 0;
    in.audit_targets = 0;
    in.instrumented_targets = 0;
    const Verdict verdict = EvaluateCoverage(in);
    CHECK_FALSE(verdict.complete);
    CHECK(verdict.resolved_share == 0.0);
    CHECK(Failed(verdict, "every_target_resolved"));
    CHECK(Failed(verdict, "every_target_instrumented"));
}

TEST_CASE("NVN census verdict: a failed capture does not make the run unclean",
          "[core][nvn_census][capture]") {
    using Reason = Common::RunOutcome::Reason;
    using Recorded = Common::RunOutcome::Recorded;
    CHECK(Common::RunOutcome::ReasonName(Reason::CaptureFailed) == "capture_failed");
    CHECK(Common::RunOutcome::ExitCode(Reason::CaptureFailed) != 0);
    // A capture that failed replaced the clean exit that stopped the run; the census judges that exit.
    for (const Reason ended : {Reason::GuestExit, Reason::WindowClosed, Reason::Requested, Reason::FrameLimit}) {
        const StopFacts facts = ClassifyStop(Recorded{.reason = Reason::CaptureFailed, .displaced = ended});
        CHECK(facts.capture_failed);
        CHECK(facts.ended_by == ended);
        CHECK(facts.clean);
    }
    CoverageInputs in = Complete();
    in.run_complete = ClassifyStop(Recorded{.reason = Reason::CaptureFailed, .displaced = Reason::FrameLimit}).clean;
    const Verdict verdict = EvaluateCoverage(in);
    CHECK(verdict.complete);
    CHECK(verdict.failed.empty());

    // The census's own conditions still decide: too few frames fail by their own name, not by the stop.
    in.frames_closed = 3;
    in.presents = 4;
    const Verdict few = EvaluateCoverage(in);
    CHECK_FALSE(few.complete);
    REQUIRE(few.failed.size() == 1);
    CHECK(few.failed[0] == "enough_frames");
}

TEST_CASE("NVN census verdict: a capture failure on top of no recorded stop is still not a clean run",
          "[core][nvn_census][capture]") {
    using Reason = Common::RunOutcome::Reason;
    using Recorded = Common::RunOutcome::Recorded;
    const StopFacts facts = ClassifyStop(Recorded{.reason = Reason::CaptureFailed, .displaced = Reason::None});
    CHECK(facts.capture_failed);
    CHECK(facts.ended_by == Reason::None);
    CHECK_FALSE(facts.clean);
}

TEST_CASE("NVN census verdict: every other reason is judged as it stands, whatever was displaced",
          "[core][nvn_census][capture]") {
    using Reason = Common::RunOutcome::Reason;
    using Recorded = Common::RunOutcome::Recorded;
    for (const Reason clean : {Reason::GuestExit, Reason::WindowClosed, Reason::Requested, Reason::FrameLimit,
                               Reason::Timeout}) {
        const StopFacts facts = ClassifyStop(Recorded{.reason = clean, .displaced = Reason::None});
        CHECK_FALSE(facts.capture_failed);
        CHECK(facts.ended_by == clean);
        CHECK(facts.clean);
    }
    for (const Reason unclean : {Reason::None, Reason::Restart, Reason::Interrupted, Reason::StartupError,
                                 Reason::ShaderFailure, Reason::GuestCrash, Reason::DeviceLost, Reason::HostCrash,
                                 Reason::BadArguments, Reason::ReportIoError}) {
        // A stale displaced reason is ignored unless the capture is what is on record.
        const StopFacts facts = ClassifyStop(Recorded{.reason = unclean, .displaced = Reason::FrameLimit});
        CHECK_FALSE(facts.capture_failed);
        CHECK(facts.ended_by == unclean);
        CHECK_FALSE(facts.clean);
    }
}
