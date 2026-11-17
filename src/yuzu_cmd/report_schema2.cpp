// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <istream>
#include <ostream>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "yuzu_cmd/run_control.h"
#include "yuzu_cmd/run_report.h"

// The schema 2 members of the run report as text. Compiled alone by the unit tests: it uses
// nothing of run_report.cpp, only the constants and inline helpers of nx2_spikes.h and
// cache_envelope.h.
namespace RunReport::Schema2 {

namespace {

using Json = nlohmann::json;
namespace Spikes = VideoCommon::Nx2Spikes;
namespace Envelope = VideoCommon::CacheEnvelope;

constexpr auto replace = Json::error_handler_t::replace;

std::string Dump(const Json& json) {
    return json.dump(-1, ' ', false, replace);
}

std::string Hex(u64 value, size_t digits) {
    static constexpr char table[] = "0123456789abcdef";
    std::string text(digits, '0');
    for (size_t i = digits; i-- > 0;) {
        text[i] = table[value & 15];
        value >>= 4;
    }
    return text;
}

/// Not finite (nothing was observed, or a division by zero upstream) is null, never a number.
Json Fraction(double value) {
    return std::isfinite(value) ? Json(value) : Json(nullptr);
}

std::string_view CauseNameOf(Spikes::Cause cause) noexcept {
    const auto index = static_cast<size_t>(cause);
    return index < Spikes::CauseCount ? Spikes::CauseNames[index] : std::string_view{"invalid"};
}

std::string_view PresentResultName(Spikes::PresentResult result) noexcept {
    switch (result) {
    case Spikes::PresentResult::Success:
        return "success";
    case Spikes::PresentResult::Suboptimal:
        return "suboptimal";
    case Spikes::PresentResult::OutOfDate:
        return "out_of_date";
    case Spikes::PresentResult::Error:
        return "error";
    }
    return "invalid";
}

std::string_view IntervalStatusName(Spikes::IntervalStatus status) noexcept {
    switch (status) {
    case Spikes::IntervalStatus::NotObserved:
        return "not_observed";
    case Spikes::IntervalStatus::Complete:
        return "complete";
    case Spikes::IntervalStatus::Incomplete:
        return "incomplete";
    }
    return "invalid";
}

std::string_view PhaseNameOf(Spikes::BootPhase phase) noexcept {
    const auto index = static_cast<size_t>(phase);
    return index < Spikes::BootPhaseNames.size() ? Spikes::BootPhaseNames[index]
                                                  : std::string_view{"invalid"};
}

std::string_view WriteStatusName(Envelope::WriteStatus status) noexcept {
    switch (status) {
    case Envelope::WriteStatus::NotAttempted:
        return "not_attempted";
    case Envelope::WriteStatus::Ok:
        return "ok";
    case Envelope::WriteStatus::Failed:
        return "failed";
    }
    return "invalid";
}

Json CausesJson(const std::array<u64, Spikes::CauseCount>& values) {
    Json json = Json::object();
    for (size_t i = 0; i < Spikes::CauseCount; ++i) {
        json[std::string{Spikes::CauseNames[i]}] = values[i];
    }
    return json;
}

Json IntervalJson(const Spikes::IntervalAttribution& interval, bool enabled) {
    const bool observed = enabled && interval.status != Spikes::IntervalStatus::NotObserved;
    Json json = Json::object();
    json["duration_ns"] = observed ? Json(interval.duration_ns) : Json(nullptr);
    json["cause_ns"] = observed ? CausesJson(interval.cause_ns) : Json(nullptr);
    json["status"] = std::string{
        IntervalStatusName(enabled ? interval.status : Spikes::IntervalStatus::NotObserved)};
    return json;
}

Json SceneCheckJson(const Spikes::AttributionSnapshot& snapshot) {
    if (!snapshot.enabled || snapshot.interactive_status == Spikes::IntervalStatus::NotObserved) {
        return nullptr;
    }
    return snapshot.interactive.scene_check_passed ? "passed" : "failed";
}

Json InteractiveSceneJson(const Spikes::AttributionSnapshot& snapshot) {
    const bool observed =
        snapshot.enabled && snapshot.interactive_status != Spikes::IntervalStatus::NotObserved;
    Json json = Json::object();
    json["id"] = observed ? Json(snapshot.interactive.scene_id) : Json(nullptr);
    json["marker_ns"] = observed ? Json(snapshot.interactive.marker_ns) : Json(nullptr);
    json["origin"] = observed ? Json(snapshot.interactive.origin) : Json(nullptr);
    json["scene_check"] = SceneCheckJson(snapshot);
    json["status"] = std::string{IntervalStatusName(
        snapshot.enabled ? snapshot.interactive_status : Spikes::IntervalStatus::NotObserved)};
    return json;
}

Json HostIdentityJson(const Envelope::CacheIdentity& identity) {
    Json device = Json::object();
    device["vendor_id"] = identity.device.vendor_id;
    device["device_id"] = identity.device.device_id;
    device["driver_id"] = identity.device.driver_id;
    device["driver_version"] = identity.device.driver_version;
    std::string uuid;
    for (const u8 byte : identity.device.pipeline_cache_uuid) {
        uuid += Hex(byte, 2);
    }
    device["pipeline_cache_uuid"] = std::move(uuid);

    // The 64-bit values are hexadecimal text: JSON readers that use doubles would round them.
    Json json = Json::object();
    json["envelope_version"] = identity.envelope_version;
    json["shader_format_version"] = identity.shader_format_version;
    json["build_id"] = Hex(identity.host.build_id, 16);
    json["title_id"] = Hex(identity.host.title_id, 16);
    json["game_version"] = identity.host.game_version;
    json["content_stamp"] = Hex(identity.host.content_stamp, 16);
    json["settings_hash"] = Hex(identity.host.settings_hash, 16);
    json["device"] = std::move(device);
    return json;
}

Json CacheFileJson(const Envelope::CacheFileReport& file) {
    const Envelope::CacheLoadResult& load = file.load;
    // accepted, rejected for a reason that is a mismatch or damage, or not there at all (the
    // first launch, or a file nobody looked at: reason none).
    const char* status = "rejected";
    if (load.accepted) {
        status = "accepted";
    } else if (load.reason == Envelope::RejectReason::Missing ||
               load.reason == Envelope::RejectReason::None) {
        status = "missing";
    }
    const bool mismatch = !load.accepted && load.reason != Envelope::RejectReason::Missing &&
                          load.reason != Envelope::RejectReason::None;
    Json json = Json::object();
    json["kind"] = std::string{Envelope::PayloadKindName(file.kind)};
    json["status"] = status;
    json["reason"] = std::string{Envelope::RejectReasonName(load.reason)};
    json["expected"] = mismatch ? Json(load.expected) : Json(nullptr);
    json["found"] = mismatch ? Json(load.found) : Json(nullptr);
    json["records"] = load.accepted ? Json(load.records) : Json(nullptr);
    return json;
}

/// How many complete lines the spool holds, or nullopt when it cannot be read. A last line with
/// no newline was cut by a crash or a write in progress and is not counted.
std::optional<u64> CountSpoolLines(std::istream& spool) {
    u64 lines = 0;
    std::string line;
    while (std::getline(spool, line)) {
        if (spool.eof()) {
            break;
        }
        if (!line.empty()) {
            ++lines;
        }
    }
    if (spool.bad()) {
        return std::nullopt;
    }
    return lines;
}

/// Copies up to `count` complete lines of the spool, separated by commas.
bool StreamSpoolLines(std::istream& spool, std::ostream& out, u64 count) {
    std::string line;
    u64 written = 0;
    while (written < count && std::getline(spool, line)) {
        if (spool.eof()) {
            break;
        }
        if (line.empty()) {
            continue;
        }
        if (written != 0) {
            out << ',';
        }
        out << line;
        ++written;
    }
    return written == count;
}

constexpr std::string_view CrashHeadEnabled =
    ",\"attribution\":{\"source\":\"host_present_submission\",\"threshold_ns\":";
constexpr std::string_view CrashHeadRest = ",\"enabled\":";
constexpr std::string_view CrashHeadEnd = ",\"status\":\"host_crash\",\"long_frames\":[";
constexpr std::string_view CrashTail = "]},\"startup\":null,\"first_120s\":null,\"cache\":null";

} // Anonymous namespace

std::string LongFrameJson(const Spikes::LongFrame& frame) {
    Json reason_list = Json::array();
    for (u32 bit = 0; bit < 32; ++bit) {
        if ((frame.unknown_reasons & (u32{1} << bit)) == 0) {
            continue;
        }
        // A bit this build has no name for is reported by number, never dropped.
        reason_list.push_back(bit < Spikes::UnknownReasonNames.size()
                                  ? std::string{Spikes::UnknownReasonNames[bit]}
                                  : "bit_" + std::to_string(bit));
    }

    Json counters = Json::object();
    for (size_t i = 0; i < frame.counter_deltas.size(); ++i) {
        if (frame.counter_deltas[i] != 0) {
            counters[std::string{Spikes::EventTokens[i]}] = frame.counter_deltas[i];
        }
    }

    Json work = Json::array();
    const size_t work_count =
        std::min<size_t>(frame.critical_work_count, Spikes::MaxCriticalWork);
    for (size_t i = 0; i < work_count; ++i) {
        work.push_back(frame.critical_work_ids[i]);
    }

    Json json = Json::object();
    json["id"] = frame.id;
    json["start_ns"] = frame.start_ns;
    json["end_ns"] = frame.end_ns;
    json["duration_ns"] = frame.duration_ns;
    json["cause_ns"] = CausesJson(frame.cause_ns);
    json["activity_ns"] = CausesJson(frame.activity_ns);
    json["inferred_ns"] = CausesJson(frame.inferred_ns);
    json["primary_cause"] = std::string{CauseNameOf(frame.primary_cause)};
    json["confidence_permille"] = frame.confidence_permille;
    json["unknown_reasons"] = std::move(reason_list);
    json["counter_deltas"] = std::move(counters);
    json["critical_work_ids"] = std::move(work);
    json["present_result"] = std::string{PresentResultName(frame.present_result)};
    json["instrumentation_complete"] = frame.instrumentation_complete;
    return Dump(json);
}

std::string AttributionJsonHead(const Spikes::AttributionSnapshot& snapshot) {
    const bool enabled = snapshot.enabled;
    Json json = Json::object();
    json["source"] = "host_present_submission";
    json["threshold_ns"] = enabled ? Json(snapshot.threshold_ns) : Json(nullptr);
    json["enabled"] = enabled;
    json["complete"] = enabled ? Json(snapshot.complete) : Json(nullptr);
    json["lost_events"] = enabled ? Json(snapshot.lost_events) : Json(nullptr);
    json["dropped_boundaries"] = enabled ? Json(snapshot.dropped_boundaries) : Json(nullptr);
    json["sink_write_errors"] = enabled ? Json(snapshot.sink_write_errors) : Json(nullptr);
    json["rejected_presents"] = enabled ? Json(snapshot.rejected_presents) : Json(nullptr);
    json["unknown_wall_fraction"] =
        enabled ? Fraction(snapshot.unknown_wall_fraction) : Json(nullptr);
    json["unknown_primary_frame_fraction"] =
        enabled ? Fraction(snapshot.unknown_primary_frame_fraction) : Json(nullptr);
    json["producer_unknown_wait_ns"] =
        enabled ? Json(snapshot.producer_unknown_wait_ns) : Json(nullptr);
    json["inferred_ns"] = enabled ? CausesJson(snapshot.inferred_ns) : Json(nullptr);
    std::string text = Dump(json);
    text.pop_back(); // the closing brace: the writer appends the long frames and `]}`
    text += ",\"long_frames\":[";
    return text;
}

std::string StartupJson(const Spikes::AttributionSnapshot& snapshot) {
    const bool enabled = snapshot.enabled;
    Json created = nullptr;
    if (enabled && snapshot.process_created_ns != 0) {
        created = Json::object();
        created["ns"] = snapshot.process_created_ns;
        created["uncertainty_ns"] = snapshot.process_created_uncertainty_ns;
    }
    Json phases = nullptr;
    if (enabled) {
        phases = Json::array();
        for (const Spikes::PhaseRecord& phase : snapshot.phases) {
            Json entry = Json::object();
            entry["phase"] = std::string{PhaseNameOf(phase.phase)};
            entry["outcome"] = std::string{RunControl::PhaseOutcomeName(phase.outcome)};
            entry["begin_ns"] = phase.begin_ns;
            entry["end_ns"] = phase.end_ns != 0 ? Json(phase.end_ns) : Json(nullptr);
            phases.push_back(std::move(entry));
        }
    }
    Json json = Json::object();
    json["process_created"] = std::move(created);
    json["first_host_present_ns"] =
        enabled && snapshot.first_host_present_ns != 0 ? Json(snapshot.first_host_present_ns)
                                                       : Json(nullptr);
    json["process_to_first_present"] = IntervalJson(snapshot.process_to_first_present, enabled);
    json["first_present_to_interactive"] =
        IntervalJson(snapshot.first_present_to_interactive, enabled);
    json["interactive_scene"] = InteractiveSceneJson(snapshot);
    json["phases"] = std::move(phases);
    return Dump(json);
}

std::string First120sJson(const Spikes::AttributionSnapshot& snapshot) {
    const Spikes::FirstWindow& window = snapshot.first_120s;
    const bool anchored = snapshot.enabled && window.anchor_ns != 0;
    const bool complete = anchored && window.complete;
    Json json = Json::object();
    json["anchor"] = "first_real_host_present";
    json["complete"] = complete;
    json["elapsed_ns"] = anchored ? Json(window.elapsed_ns) : Json(nullptr);
    json["frames"] = anchored ? Json(window.frames) : Json(nullptr);
    json["frames_over_100ms"] = anchored ? Json(window.frames_over_threshold) : Json(nullptr);
    json["max_ns"] = anchored ? Json(window.max_ns) : Json(nullptr);
    json["crossing_tail_interval_ns"] =
        complete ? Json(window.crossing_tail_interval_ns) : Json(nullptr);
    json["scene_check"] = SceneCheckJson(snapshot);
    return Dump(json);
}

std::string CacheJson(const Envelope::CacheReport& report) {
    const Envelope::CacheStats& stats = report.stats;
    // A cache that never published (no disk cache this run) is the default report: nothing was
    // measured, so nothing is zero.
    const bool reported = !report.files.empty() || report.identity.shader_format_version != 0 ||
                          stats.recorded != 0 || stats.prepared != 0 || stats.adopted != 0 ||
                          stats.write_status != Envelope::WriteStatus::NotAttempted;
    Json files = Json::array();
    for (const Envelope::CacheFileReport& file : report.files) {
        files.push_back(CacheFileJson(file));
    }
    const auto number = [reported](u64 value) { return reported ? Json(value) : Json(nullptr); };
    Json json = Json::object();
    json["status"] = reported ? "reported" : "not_reported";
    json["identity"] = reported ? HostIdentityJson(report.identity) : Json(nullptr);
    json["files"] = std::move(files);
    json["recorded"] = number(stats.recorded);
    json["prepared"] = number(stats.prepared);
    json["adopted"] = number(stats.adopted);
    json["demand_promoted"] = number(stats.demand_promoted);
    json["demand_wait_ns"] = number(stats.demand_wait_ns);
    json["prepare_failures"] = number(stats.prepare_failures);
    json["records_skipped"] = number(stats.records_skipped);
    json["write_status"] = reported ? Json(std::string{WriteStatusName(stats.write_status)})
                                    : Json(nullptr);
    json["queue_high_water"] = number(stats.queue_high_water);
    json["memory_high_water_bytes"] = number(stats.memory_high_water_bytes);
    json["budget_pauses"] = number(stats.budget_pauses);
    json["evicted"] = number(stats.evicted);
    json["checkpoints"] = number(stats.checkpoints);
    json["bytes_committed"] = number(stats.bytes_committed);
    // Translation is timed inside the build of a record (build_cost_us), never on its own.
    json["translate_cost"] = reported ? Json("not_separately_measured") : Json(nullptr);
    return Dump(json);
}

std::string SessionJson(const RunControl::SessionInfo& session) {
    Json rejected = Json::object();
    rejected["malformed"] = session.rejected.malformed;
    rejected["missing_credentials"] = session.rejected.missing_credentials;
    rejected["title_mismatch"] = session.rejected.title_mismatch;
    rejected["nonce_mismatch"] = session.rejected.nonce_mismatch;
    rejected["refused"] = session.rejected.refused;
    Json manifest = Json::object();
    manifest["entries"] = session.manifest.entries;
    manifest["over_cap"] = session.manifest.over_cap;
    manifest["duration_ns"] =
        session.manifest.duration_ns ? Json(*session.manifest.duration_ns) : Json(nullptr);
    Json json = Json::object();
    json["title_id"] = Hex(session.title_id, 16);
    json["nonce"] = session.nonce != 0 ? Json(Hex(session.nonce, 16)) : Json(nullptr);
    json["markers_rejected"] = std::move(rejected);
    json["content_manifest"] = std::move(manifest);
    return Dump(json);
}

bool WriteMembers(std::ostream& out, const MembersInput& input) {
    Spikes::AttributionSnapshot snapshot = input.snapshot;

    std::optional<u64> lines = u64{0};
    if (snapshot.enabled && input.spool != nullptr) {
        input.spool->clear();
        input.spool->seekg(0);
        lines = CountSpoolLines(*input.spool);
        input.spool->clear();
        input.spool->seekg(0);
    }
    // Every long frame the recorder counted must be in the spool, the shutdown must have
    // finished, and nothing may have failed on the way.
    snapshot.complete = snapshot.complete && input.run_complete && !input.spool_failed &&
                        lines.has_value() && *lines == snapshot.long_frames;

    // The session is a member of startup, spliced in before its closing brace.
    std::string startup = StartupJson(snapshot);
    startup.pop_back();
    startup += ",\"session\":" + SessionJson(input.session) + "}";
    out << ",\n  \"startup\": " << startup;
    out << ",\n  \"first_120s\": " << First120sJson(snapshot);
    out << ",\n  \"cache\": " << CacheJson(input.cache);
    out << ",\n  \"attribution\": " << AttributionJsonHead(snapshot);
    bool streamed = true;
    if (snapshot.enabled && input.spool != nullptr && lines.has_value()) {
        streamed = StreamSpoolLines(*input.spool, out, *lines);
    }
    out << "]}";
    return streamed && static_cast<bool>(out);
}

size_t JoinSpoolLines(char* data, size_t size) noexcept {
    size_t end = size;
    while (end != 0 && data[end - 1] != '\n') {
        --end; // a line that was still being written
    }
    // Empty lines and the last newline go; every other newline becomes the comma between two.
    size_t length = 0;
    bool pending_separator = false;
    size_t line_start = 0;
    for (size_t i = 0; i < end; ++i) {
        if (data[i] != '\n') {
            continue;
        }
        const size_t line_length = i - line_start;
        if (line_length != 0) {
            if (pending_separator) {
                data[length++] = ',';
            }
            std::memmove(data + length, data + line_start, line_length);
            length += line_length;
            pending_separator = true;
        }
        line_start = i + 1;
    }
    return length;
}

namespace {

/// Appends text to a fixed buffer; `ok` turns false instead of writing past the end.
struct FixedText {
    char* data;
    size_t capacity;
    size_t size{};
    bool ok{true};

    void Add(std::string_view text) noexcept {
        if (!ok || text.size() > capacity - size) {
            ok = false;
            return;
        }
        std::memcpy(data + size, text.data(), text.size());
        size += text.size();
    }

    void AddNumber(unsigned long long value) noexcept {
        char digits[24];
        size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        char text[24];
        for (size_t i = 0; i < count; ++i) {
            text[i] = digits[count - 1 - i];
        }
        Add(std::string_view{text, count});
    }
};

} // Anonymous namespace

size_t CrashMembersHead(char* out, size_t capacity, bool enabled, u64 threshold_ns) noexcept {
    FixedText text{out, capacity};
    text.Add(CrashHeadEnabled);
    if (enabled) {
        text.AddNumber(threshold_ns);
    } else {
        text.Add("null");
    }
    text.Add(CrashHeadRest);
    text.Add(enabled ? "true,\"complete\":false" : "false,\"complete\":null");
    text.Add(CrashHeadEnd);
    return text.ok ? text.size : 0;
}

size_t CrashMembersTail(char* out, size_t capacity) noexcept {
    if (CrashTail.size() > capacity) {
        return 0;
    }
    std::memcpy(out, CrashTail.data(), CrashTail.size());
    return CrashTail.size();
}

} // namespace RunReport::Schema2
