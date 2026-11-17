// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "common/common_types.h"

namespace Settings {

struct Values;

/// The baseline settings are the settings every game is tested on: the default of every emulation
/// setting, with every EDEN_NX2_* switch unset. There is no second table of values; the baseline
/// is read from the settings themselves. fork-notes/baseline.md lists it and keeps its history.
///
/// Raise this number, and add a line to the baseline history in fork-notes/baseline.md, whenever a
/// default of a baseline setting changes or a baseline setting is added or removed.
/// src/tests/common/baseline_settings.cpp pins the list for the current version.
namespace Baseline {

inline constexpr u32 kBaselineVersion = 1;

/// Settings that sit in a baseline category but belong to the user or to the running session, not
/// to the emulation: the selected profile, the console's device name and the volume controls.
[[nodiscard]] bool IsExcludedKey(std::string_view key);

struct Entry {
    std::string key;
    std::string value; ///< The default of the setting as text (enums as their number).
};

struct Deviation {
    std::string key;
    std::string baseline;
    std::string effective;
    /// "global config", "per-game config", "command line", "environment switch",
    /// "hardware fallback" or "program requirement".
    std::string source;
};

/// The source of a setting the emulator sets for a program by itself (for example the memory layout
/// of a Switch 2 program). It is listed as a deviation but does not take a run off the baseline:
/// no one chose it, and it is the same on every run of that program.
inline constexpr std::string_view kProgramRequirement = "program requirement";

/// An EDEN_NX2_* environment switch that is set. Every one is a deviation from the baseline.
struct EnvironmentSwitch {
    std::string name;
    std::string value;
};

/// Prefix of the environment variables that are switches.
inline constexpr std::string_view kSwitchPrefix = "EDEN_NX2_";

/// The baseline, sorted by key: the default of every persisted setting in the emulation
/// categories (core, CPU, renderer, system, audio, GPU driver), without the excluded keys.
[[nodiscard]] std::vector<Entry> Entries(const Values& source);
[[nodiscard]] std::vector<Entry> Entries();

/// "key=value\n" for every entry, in order. This is the text that is hashed.
[[nodiscard]] std::string CanonicalText(const std::vector<Entry>& entries);

/// SHA-256 of a text as lower-case hex.
[[nodiscard]] std::string Sha256Hex(std::string_view text);

/// SHA-256 of the canonical text of the baseline, as lower-case hex.
[[nodiscard]] std::string Hash(const std::vector<Entry>& entries);
[[nodiscard]] std::string Hash();

/// Records where the current value of a setting came from when it is not the configuration file:
/// "command line", "hardware fallback" or "program requirement". Used by Deviations.
void NoteSource(std::string_view key, std::string_view source);
void ClearNotedSources();

/// The EDEN_NX2_* variables that are set in the process environment.
[[nodiscard]] std::vector<EnvironmentSwitch> SetEnvironmentSwitches();

/// The instrumentation class: the variables the test harness sets on every run to capture and
/// drive it (diagnostics folder, frame capture, window hiding, key script). They are neither
/// refused by --baseline nor deviations, and a report names them without their values. Nothing
/// else joins this class: every other EDEN_NX2_* variable is an ordinary switch.
[[nodiscard]] bool IsHarnessSwitch(std::string_view name);

/// The names of the harness variables in `switches`, sorted. Never the values.
[[nodiscard]] std::vector<std::string> HarnessSwitches(const std::vector<EnvironmentSwitch>& switches);

/// True for a variable whose reader takes a path, a file name or a script as its value
/// (EDEN_NX2_DIAG_DIR, EDEN_NX2_FRAME_CAPTURE, EDEN_NX2_SHADER_DUMP and the like, and any name
/// ending in _DIR, _FILE, _PATH, _SCRIPT, _PYTHON, _DUMP, _RECORD, _OUT or _CAPTURE).
[[nodiscard]] bool IsPathSwitch(std::string_view name);

/// The text of an environment variable's value that a report may carry. A path variable (by name,
/// before the shape of the value is looked at: a relative path can be "123" or "on") is always
/// "set". Any other variable carries its value when it is a plain number or one of on, off, true,
/// false, yes, no, and "set" otherwise (a free text).
[[nodiscard]] std::string PublicSwitchValue(std::string_view name, std::string_view value);

/// Shader ablation (a diagnostics file that makes the renderer skip programs) is not a setting.
/// The renderer calls NoteShaderAblation when it has read at least one active entry; from then on
/// Deviations lists "shader_ablation" (baseline off, effective on, environment switch).
void NoteShaderAblation();
[[nodiscard]] bool ShaderAblationActive();

/// A function called (once, from the thread that notes it) when the first ablation entry becomes
/// active, so that a report snapshot taken earlier can be refreshed. May be null.
void SetChangeHook(void (*hook)());

/// Every baseline setting whose effective value is not its default, plus every set environment
/// switch, sorted by key. A setting uses the source noted for it; otherwise "per-game config"
/// when it uses a per-game value and "global config" when not.
[[nodiscard]] std::vector<Deviation> Deviations(const Values& source,
                                                const std::vector<EnvironmentSwitch>& switches);
[[nodiscard]] std::vector<Deviation> Deviations();

/// True when every deviation is one of the declared keys (or there is none).
[[nodiscard]] bool OnBaseline(const std::vector<Deviation>& deviations,
                              const std::vector<std::string>& declared);

/// --baseline-allow takes "KEY[,KEY...]"; an item is a setting key, an EDEN_NX2_* switch name, or
/// "KEY=VALUE". SplitAllowList cuts the argument at the commas (empty pieces are kept, so the
/// caller can refuse them); AllowedKeys gives the key of every item, without any "=VALUE".
[[nodiscard]] std::vector<std::string> SplitAllowList(std::string_view text);
[[nodiscard]] std::vector<std::string> AllowedKeys(const std::vector<std::string>& items);

/// Sets every "KEY=VALUE" item on the settings (after ResetToBaseline) and notes the command line
/// as its source. Returns the first problem as text (not a baseline setting, a value the setting
/// does not accept), empty on success. The value is written as ToString (enums as their number)
/// or as the canonical name.
[[nodiscard]] std::string ApplyAllowedValues(Values& target, const std::vector<std::string>& items);

/// The set EDEN_NX2_* switches that no item names.
[[nodiscard]] std::vector<std::string> UndeclaredSwitches(
    const std::vector<EnvironmentSwitch>& switches, const std::vector<std::string>& items);

/// Puts every baseline setting on its default and switchable ones on their global value.
/// Controls, paths, profiles and everything else are left alone.
void ResetToBaseline(Values& target);
void ResetToBaseline();

/// The "baseline" object of a run report:
/// {"version","hash","requested","on_baseline","deviations":[{"key","baseline","effective",
/// "source"}],"declared":[key...],"harness":[name...]}. "harness" names the harness variables that
/// were set, sorted, and never carries a value.
[[nodiscard]] nlohmann::json ReportJson(bool requested, const std::vector<std::string>& declared,
                                        const std::vector<Deviation>& deviations,
                                        const std::vector<std::string>& harness);
[[nodiscard]] nlohmann::json ReportJson(bool requested, const std::vector<std::string>& declared);

/// The text `--print-baseline` writes: version, hash and one "key=value" line per entry.
[[nodiscard]] std::string PrintableText();

} // namespace Baseline
} // namespace Settings
