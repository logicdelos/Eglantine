// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/run_outcome.h"

namespace Core::NvnCensus {

/// A branch into the graphics library from code outside it, classified from what the library's own
/// code says, never from a list of names or addresses.
enum class EntryClass : u32 {
    /// A RET that lands just after a call instruction: the library called out, the callee returned.
    ReturnSite = 0,
    /// Anything else: a call (or jump) into the library that is an entry point in its own right.
    Entry,
};

/// Whether `insn` (an A64 word) is a call that sets the link register: BL, BLR, or the pointer
/// authenticated BLRAA / BLRAAZ / BLRAB / BLRABZ. The instruction after one is a return site.
[[nodiscard]] constexpr bool IsCallInstruction(u32 insn) noexcept {
    if ((insn & 0xFC000000U) == 0x94000000U) {
        return true; // BL
    }
    if ((insn & 0xFFFFFC1FU) == 0xD63F0000U) {
        return true; // BLR
    }
    if ((insn & 0xFFFFFC1FU) == 0xD63F081FU || (insn & 0xFFFFFC1FU) == 0xD63F0C1FU) {
        return true; // BLRAAZ, BLRABZ (Rm is 31)
    }
    return (insn & 0xFFFFFC00U) == 0xD73F0800U || (insn & 0xFFFFFC00U) == 0xD73F0C00U; // BLRAA, BLRAB
}

/// Classifies a RET that lands at `target` inside the library, given the instruction word before it.
/// Only a RET can be a return: any other branch kind is an entry whatever the word before it is.
[[nodiscard]] constexpr EntryClass ClassifyReturn(u32 previous_insn) noexcept {
    return IsCallInstruction(previous_insn) ? EntryClass::ReturnSite : EntryClass::Entry;
}

/// A producer numbers accepted presents consecutively. Reordering is a discontinuity, not a duplicate.
constexpr void ObservePresentId(u64 last, u64 id, u64& duplicates, u64& gaps) noexcept {
    if (id == last) {
        ++duplicates;
    } else if (id != last + 1) {
        ++gaps;
    }
}

struct Condition {
    std::string name;
    bool ok{};
    std::string detail;
};

/// Everything the completeness verdict reads. Plain numbers: the census fills it from one final
/// snapshot, and a test fills it by hand.
struct CoverageInputs {
    bool final_report{};
    bool run_complete{};
    bool audit_enabled{};
    bool audit_region_known{};

    u64 bootstrap_sites{};
    u64 bootstrap_calls{};
    u64 late_modules{};
    u64 code_changes{};

    /// Entry points the census knows by name (exports and what the resolvers returned).
    u64 named_targets{};
    /// Entry points the audit found for itself: no name, but counted from their first call on.
    u64 audit_targets{};
    /// Of those, the ones with a live probe at their first instruction: calls from inside the library
    /// are counted for these too. Without an acknowledged entry probe coverage cannot be complete.
    u64 audit_targets_entry_counted{};
    /// Entry points a branch from outside reached that nothing could attribute.
    u64 unknown_targets{};
    u64 unknown_calls{};
    u64 unknown_overflow{};
    u64 unaudited_entries{};
    /// Targets (named or found by audit) whose calls are counted.
    u64 instrumented_targets{};

    /// Branches from outside the library that landed on a counted entry point.
    u64 audited_entries{};
    /// Entries the audit saw that the counter never saw (the counter must be at least the audit).
    u64 audit_shortfall_functions{};
    u64 audit_shortfall_calls{};

    bool accounting_exact{};
    u64 accounting_late{};
    u64 executed{};
    u64 startup{};
    u64 frames{};
    u64 tail{};
    u64 accounting_mismatched{};

    u64 paths_used{};
    u64 streams{};
    bool stream_ambiguous{};
    u64 stream_tokens{};
    bool identity_ok{};
    u64 presents{};
    u64 frames_closed{};
    u64 min_frames{};
    u64 id_gaps{};
    u64 id_duplicates{};

    u64 lost_events{};
    std::string lost_detail;
};

/// How the census reads the way a run ended, kept apart from what a replay capture did. A failed
/// capture (CaptureFailed) only ever replaces a clean exit as the reason on record: the census
/// judges the run by the exit it replaced, and reports the capture in a field of its own.
struct StopFacts {
    /// What ended the run itself, apart from a failed capture.
    Common::RunOutcome::Reason ended_by{};
    /// A stop the run asked for itself, with the cores shut down: the workload ran to its end. A
    /// crash, a lost device, a signal or no recorded reason at all does not count.
    bool clean{};
    bool capture_failed{};
};

[[nodiscard]] inline bool IsCleanStop(Common::RunOutcome::Reason reason) noexcept {
    using Common::RunOutcome::Reason;
    switch (reason) {
    case Reason::GuestExit:
    case Reason::WindowClosed:
    case Reason::Requested:
    case Reason::FrameLimit:
    case Reason::Timeout:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline StopFacts ClassifyStop(const Common::RunOutcome::Recorded& recorded) noexcept {
    using Common::RunOutcome::Reason;
    StopFacts facts;
    facts.capture_failed = recorded.reason == Reason::CaptureFailed;
    facts.ended_by = facts.capture_failed ? recorded.displaced : recorded.reason;
    facts.clean = IsCleanStop(facts.ended_by);
    return facts;
}

struct Verdict {
    bool complete{};
    double resolved_share{};
    double instrumented_share{};
    std::vector<Condition> conditions;
    /// Names of the conditions that failed, then `not_a_final_report` / `run_not_complete`.
    std::vector<std::string> failed;
    /// One line per failure: the condition and why. Empty when complete.
    std::vector<std::string> reasons;
};

/// The census decides for itself whether it is complete. Every condition is computed from counted
/// figures, none from the name of a game or a title.
[[nodiscard]] inline Verdict EvaluateCoverage(const CoverageInputs& in) {
    Verdict verdict;
    const u64 known = in.named_targets + in.audit_targets;
    const u64 discovered = known + in.unknown_targets;
    verdict.resolved_share = discovered != 0 ? static_cast<double>(known) / static_cast<double>(discovered) : 0.0;
    verdict.instrumented_share =
        discovered != 0 ? static_cast<double>(in.instrumented_targets) / static_cast<double>(discovered) : 0.0;

    auto& c = verdict.conditions;
    c.push_back({"bootstrap_found_before_first_request",
                 in.bootstrap_sites != 0 && in.bootstrap_calls != 0 && in.late_modules == 0 && in.code_changes == 0,
                 fmt::format("sites={} bootstrap_calls={} late_modules={} code_changes={}", in.bootstrap_sites,
                             in.bootstrap_calls, in.late_modules, in.code_changes)});
    c.push_back({"every_target_resolved", in.audit_enabled && known != 0 && in.unknown_targets == 0,
                 fmt::format("audit={} resolved_share={:.6f} ({} named, {} found by the audit, {} unknown)",
                             in.audit_enabled, verdict.resolved_share, in.named_targets, in.audit_targets,
                             in.unknown_targets)});
    c.push_back({"every_target_instrumented", discovered != 0 && in.instrumented_targets == discovered,
                 fmt::format("instrumented_share={:.6f} ({} counted of {})", verdict.instrumented_share,
                             in.instrumented_targets, discovered)});
    // The branch audit cannot substitute for an acknowledged probe at the target.
    c.push_back({"audit_found_entries_counted_at_entry", in.audit_targets_entry_counted == in.audit_targets,
                 fmt::format("audit_found={} counted_at_entry={}", in.audit_targets, in.audit_targets_entry_counted)});
    c.push_back({"no_unknown_dispatch",
                 in.audit_enabled && in.unknown_calls == 0 && in.unknown_overflow == 0 && in.unaudited_entries == 0,
                 fmt::format("audit={} unknown_targets={} unknown_calls={} overflow={} unaudited={}", in.audit_enabled,
                             in.unknown_targets, in.unknown_calls, in.unknown_overflow, in.unaudited_entries)});
    c.push_back({"audit_matches_counts",
                 in.audit_enabled && in.audit_region_known && in.audited_entries != 0 &&
                     in.audited_entries <= in.executed && in.audit_shortfall_functions == 0 && in.audit_shortfall_calls == 0,
                 fmt::format("audit={} region={} audited_entries={} short_functions={} short_calls={}",
                             in.audit_enabled, in.audit_region_known, in.audited_entries, in.audit_shortfall_functions,
                             in.audit_shortfall_calls)});
    c.push_back({"frame_accounting_exact", in.accounting_exact && in.accounting_late == 0 && in.accounting_mismatched == 0 &&
                     in.executed == in.startup + in.frames + in.tail,
                 fmt::format("last_report={} executed={} = startup {} + frames {} + late {} + tail {}; mismatched={}",
                             in.final_report, in.executed, in.startup, in.frames, in.accounting_late, in.tail,
                             in.accounting_mismatched)});
    c.push_back({"one_present_stream", in.paths_used == 1 && in.identity_ok,
                 fmt::format("paths={} streams={} ambiguous={} tokens={}", in.paths_used, in.streams,
                             in.stream_ambiguous, in.stream_tokens)});
    c.push_back({"presents_consistent",
                 in.presents != 0 && in.id_gaps == 0 && in.id_duplicates == 0 && in.frames_closed + 1 == in.presents,
                 fmt::format("presents={} closed={} gaps={} duplicates={}", in.presents, in.frames_closed, in.id_gaps,
                             in.id_duplicates)});
    c.push_back({"enough_frames", in.frames_closed >= in.min_frames,
                 fmt::format("closed={} required={}", in.frames_closed, in.min_frames)});
    c.push_back({"no_dropped_or_lost_events", in.lost_events == 0, in.lost_detail.empty() ? "none" : in.lost_detail});

    bool all_ok = true;
    for (const Condition& condition : c) {
        if (!condition.ok) {
            all_ok = false;
            verdict.failed.push_back(condition.name);
            verdict.reasons.push_back(fmt::format("{}: {}", condition.name, condition.detail));
        }
    }
    if (!in.final_report) {
        verdict.failed.emplace_back("not_a_final_report");
        verdict.reasons.emplace_back("not_a_final_report: a report written while the game runs is never complete");
    } else if (!in.run_complete) {
        verdict.failed.emplace_back("run_not_complete");
        verdict.reasons.emplace_back("run_not_complete: the run did not end with a stop it asked for itself");
    }
    verdict.complete = in.final_report && in.run_complete && all_ok;
    return verdict;
}

} // namespace Core::NvnCensus
