// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "common/baseline_settings.h"
#include "common/settings.h"
#include "common/settings_common.h"

namespace Settings::Baseline {

namespace {

/// The categories of the emulation settings. Controls, paths, user interface, web service, network
/// and per-user data are not part of the baseline.
bool IsBaselineCategory(Category category) {
    switch (category) {
    case Category::Audio:
    case Category::Core:
    case Category::Cpu:
    case Category::CpuDebug:
    case Category::CpuUnsafe:
    case Category::Renderer:
    case Category::RendererAdvanced:
    case Category::RendererHacks:
    case Category::RendererExtensions:
    case Category::RendererDebug:
    case Category::RendererMemory:
    case Category::System:
    case Category::SystemAudio:
    case Category::GpuDriver:
        return true;
    default:
        return false;
    }
}

/// Settings of the debugging categories that change what a run does or draws: the macro engine
/// (compiled or interpreted, high-level replacements), the debug knob mask, the console identity
/// bit the guest reads, key loading, service stubbing, the web applet, the controller applet and
/// the Vulkan start-up check. The rest of those categories only write logs and dumps, open
/// servers, or hold a path, a secret or a per-install serial number, and stays out of the baseline.
constexpr std::array<std::string_view, 9> kDebugBaselineKeys{
    "debug_knobs",
    "disable_macro_hle",
    "disable_macro_jit",
    "disable_web_applet",
    "enable_all_controllers",
    "perform_vulkan_check",
    "quest_flag",
    "use_auto_stub",
    "use_dev_keys",
};

bool IsDebugBaselineKey(std::string_view key) {
    return std::find(kDebugBaselineKeys.begin(), kDebugBaselineKeys.end(), key) !=
           kDebugBaselineKeys.end();
}

bool IsBaselineSetting(const std::string& key, const BasicSetting& setting) {
    const Category category = setting.GetCategory();
    const bool included = IsBaselineCategory(category) ||
                          ((category == Category::Debugging ||
                            category == Category::DebuggingGraphics) &&
                           IsDebugBaselineKey(key));
    return included && setting.Save() && !IsExcludedKey(key);
}

/// The name of an EDEN_NX2_ switch: the prefix and at least one upper-case letter, digit or
/// underscore. There is no table of the switches the readers know (each reader calls getenv with
/// its own literal), so the grammar is what can be checked.
bool IsSwitchName(std::string_view name) {
    if (!name.starts_with(kSwitchPrefix) || name.size() == kSwitchPrefix.size()) {
        return false;
    }
    return std::all_of(name.begin() + kSwitchPrefix.size(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

std::mutex source_mutex;
std::atomic<bool> shader_ablation{false};
std::atomic<void (*)()> change_hook{nullptr};

std::map<std::string, std::string, std::less<>>& NotedSources() {
    static std::map<std::string, std::string, std::less<>> sources;
    return sources;
}

std::string NotedSourceOf(const std::string& key) {
    const std::scoped_lock lock{source_mutex};
    const auto& sources = NotedSources();
    const auto it = sources.find(key);
    return it == sources.end() ? std::string{} : it->second;
}

constexpr std::array<u32, 64> kRoundConstants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr u32 RotateRight(u32 value, u32 amount) {
    return (value >> amount) | (value << (32 - amount));
}

void Sha256Block(std::array<u32, 8>& state, const u8* block) {
    std::array<u32, 64> w{};
    for (size_t i = 0; i < 16; ++i) {
        w[i] = (u32{block[i * 4]} << 24) | (u32{block[i * 4 + 1]} << 16) |
               (u32{block[i * 4 + 2]} << 8) | u32{block[i * 4 + 3]};
    }
    for (size_t i = 16; i < 64; ++i) {
        const u32 s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = state[0];
    u32 b = state[1];
    u32 c = state[2];
    u32 d = state[3];
    u32 e = state[4];
    u32 f = state[5];
    u32 g = state[6];
    u32 h = state[7];
    for (size_t i = 0; i < 64; ++i) {
        const u32 big_s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const u32 choice = (e & f) ^ (~e & g);
        const u32 temp1 = h + big_s1 + choice + kRoundConstants[i] + w[i];
        const u32 big_s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const u32 majority = (a & b) ^ (a & c) ^ (b & c);
        const u32 temp2 = big_s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/// The value the emulator uses for a baseline setting right now.
std::string EffectiveValue(const Values& source, const std::string& key,
                           const BasicSetting& setting, bool& hardware_fallback) {
    hardware_fallback = false;
    if (&source == &Settings::values && key == "astc_recompression") {
        // The renderer can replace the ASTC recompression when the device can't decode ASTC.
        const auto in_effect = GetAstcRecompression();
        if (in_effect != source.astc_recompression.GetValue()) {
            hardware_fallback = true;
            return std::to_string(static_cast<std::underlying_type_t<AstcRecompression>>(in_effect));
        }
    }
    return setting.ToString();
}

} // namespace

bool IsExcludedKey(std::string_view key) {
    static constexpr std::array<std::string_view, 4> excluded{
        "current_user",
        "device_name",
        "volume",
        "audio_muted",
    };
    return std::find(excluded.begin(), excluded.end(), key) != excluded.end();
}

std::vector<Entry> Entries(const Values& source) {
    std::vector<Entry> entries;
    for (const auto& [key, setting] : source.linkage.by_key) {
        if (IsBaselineSetting(key, *setting)) {
            entries.push_back({key, setting->DefaultToString()});
        }
    }
    return entries;
}

std::vector<Entry> Entries() {
    return Entries(Settings::values);
}

std::string CanonicalText(const std::vector<Entry>& entries) {
    std::string text;
    for (const auto& entry : entries) {
        text += entry.key;
        text += '=';
        text += entry.value;
        text += '\n';
    }
    return text;
}

std::string Sha256Hex(std::string_view text) {
    std::array<u32, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string message{text};
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) {
        message.push_back('\0');
    }
    const u64 bit_count = static_cast<u64>(text.size()) * 8;
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<char>((bit_count >> shift) & 0xff));
    }
    for (size_t offset = 0; offset < message.size(); offset += 64) {
        Sha256Block(state, reinterpret_cast<const u8*>(message.data()) + offset);
    }
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const u32 word : state) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex.push_back(digits[(word >> shift) & 0xf]);
        }
    }
    return hex;
}

std::string Hash(const std::vector<Entry>& entries) {
    return Sha256Hex(CanonicalText(entries));
}

std::string Hash() {
    return Hash(Entries());
}

void NoteSource(std::string_view key, std::string_view source) {
    const std::scoped_lock lock{source_mutex};
    if (source.empty()) {
        const auto it = NotedSources().find(key);
        if (it != NotedSources().end()) {
            NotedSources().erase(it);
        }
        return;
    }
    NotedSources().insert_or_assign(std::string{key}, std::string{source});
}

void ClearNotedSources() {
    const std::scoped_lock lock{source_mutex};
    NotedSources().clear();
    shader_ablation.store(false, std::memory_order_release);
}

std::vector<EnvironmentSwitch> SetEnvironmentSwitches() {
    std::vector<EnvironmentSwitch> switches;
#ifdef _WIN32
    char** const variables = _environ;
#else
    char** const variables = environ;
#endif
    if (variables == nullptr) {
        return switches;
    }
    for (char** entry = variables; *entry != nullptr; ++entry) {
        const std::string_view text{*entry};
        const size_t equals = text.find('=');
        if (equals == std::string_view::npos || !text.starts_with(kSwitchPrefix)) {
            continue;
        }
        switches.push_back({std::string{text.substr(0, equals)}, std::string{text.substr(equals + 1)}});
    }
    std::sort(switches.begin(), switches.end(),
              [](const EnvironmentSwitch& lhs, const EnvironmentSwitch& rhs) {
                  return lhs.name < rhs.name;
              });
    return switches;
}

bool IsHarnessSwitch(std::string_view name) {
    static constexpr std::array<std::string_view, 4> harness{
        "EDEN_NX2_DIAG_DIR",
        "EDEN_NX2_FRAME_CAPTURE",
        "EDEN_NX2_HIDE_WINDOW",
        "EDEN_NX2_KEY_SCRIPT",
    };
    return std::find(harness.begin(), harness.end(), name) != harness.end();
}

std::vector<std::string> HarnessSwitches(const std::vector<EnvironmentSwitch>& switches) {
    std::vector<std::string> names;
    for (const auto& entry : switches) {
        if (IsHarnessSwitch(entry.name)) {
            names.push_back(entry.name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string PublicSwitchValue(std::string_view value) {
    static constexpr std::array<std::string_view, 6> words{"on", "off", "true", "false", "yes", "no"};
    if (std::find(words.begin(), words.end(), value) != words.end()) {
        return std::string{value};
    }
    // A plain number: an optional minus sign, digits, and at most one decimal point with digits.
    size_t index = !value.empty() && value.front() == '-' ? 1 : 0;
    const size_t first_digit = index;
    while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
        ++index;
    }
    bool plain = index > first_digit;
    if (plain && index < value.size() && value[index] == '.') {
        const size_t point = index++;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
            ++index;
        }
        plain = index > point + 1;
    }
    plain = plain && index == value.size() && value.size() <= 24;
    return plain ? std::string{value} : std::string{"set"};
}

void NoteShaderAblation() {
    if (!shader_ablation.exchange(true, std::memory_order_acq_rel)) {
        if (const auto hook = change_hook.load(std::memory_order_acquire)) {
            hook();
        }
    }
}

bool ShaderAblationActive() {
    return shader_ablation.load(std::memory_order_acquire);
}

void SetChangeHook(void (*hook)()) {
    change_hook.store(hook, std::memory_order_release);
}

std::vector<Deviation> Deviations(const Values& current,
                                  const std::vector<EnvironmentSwitch>& switches) {
    std::vector<Deviation> deviations;
    for (const auto& [key, setting] : current.linkage.by_key) {
        if (!IsBaselineSetting(key, *setting)) {
            continue;
        }
        bool hardware_fallback = false;
        std::string effective = EffectiveValue(current, key, *setting, hardware_fallback);
        std::string baseline = setting->DefaultToString();
        if (effective == baseline) {
            continue;
        }
        std::string source = NotedSourceOf(key);
        if (source.empty()) {
            if (hardware_fallback) {
                source = "hardware fallback";
            } else {
                source = setting->UsingGlobal() ? "global config" : "per-game config";
            }
        }
        deviations.push_back({key, std::move(baseline), std::move(effective), std::move(source)});
    }
    for (const auto& entry : switches) {
        // The harness variables are instrumentation, not a choice of the run.
        if (!IsHarnessSwitch(entry.name)) {
            deviations.push_back(
                {entry.name, "unset", PublicSwitchValue(entry.value), "environment switch"});
        }
    }
    if (ShaderAblationActive()) {
        deviations.push_back({"shader_ablation", "off", "on", "environment switch"});
    }
    std::sort(deviations.begin(), deviations.end(),
              [](const Deviation& lhs, const Deviation& rhs) { return lhs.key < rhs.key; });
    return deviations;
}

std::vector<Deviation> Deviations() {
    return Deviations(Settings::values, SetEnvironmentSwitches());
}

bool OnBaseline(const std::vector<Deviation>& deviations,
                const std::vector<std::string>& declared) {
    return std::all_of(deviations.begin(), deviations.end(), [&declared](const Deviation& deviation) {
        // What the emulator sets for the program by itself is part of running it, not a choice.
        return deviation.source == kProgramRequirement ||
               std::find(declared.begin(), declared.end(), deviation.key) != declared.end();
    });
}

std::vector<std::string> SplitAllowList(std::string_view text) {
    std::vector<std::string> items;
    size_t begin = 0;
    while (true) {
        const size_t comma = text.find(',', begin);
        if (comma == std::string_view::npos) {
            items.emplace_back(text.substr(begin));
            return items;
        }
        items.emplace_back(text.substr(begin, comma - begin));
        begin = comma + 1;
    }
}

std::vector<std::string> AllowedKeys(const std::vector<std::string>& items) {
    std::vector<std::string> keys;
    keys.reserve(items.size());
    for (const auto& item : items) {
        keys.emplace_back(item.substr(0, item.find('=')));
    }
    return keys;
}

std::string ApplyAllowedValues(Values& target, const std::vector<std::string>& items) {
    for (const auto& item : items) {
        const size_t equals = item.find('=');
        if (equals == std::string::npos) {
            // A bare item declares a setting that --baseline-allow may change by another route
            // (the config file, the Qt window) or an EDEN_NX2_ switch. It has to name one.
            const auto found = target.linkage.by_key.find(item);
            const bool setting =
                found != target.linkage.by_key.end() && IsBaselineSetting(item, *found->second);
            if (!setting && !IsSwitchName(item)) {
                return "'" + item +
                       "' is neither a baseline setting nor an EDEN_NX2_ switch name";
            }
            continue;
        }
        const std::string key = item.substr(0, equals);
        const std::string text = item.substr(equals + 1);
        const auto found = target.linkage.by_key.find(key);
        if (found == target.linkage.by_key.end() || !IsBaselineSetting(key, *found->second)) {
            return "'" + key + "' is not a baseline setting";
        }
        BasicSetting& setting = *found->second;
        setting.SetGlobal(true);
        setting.LoadString(text);
        if (setting.ToString() != text && setting.Canonicalize() != text) {
            return "'" + text + "' is not accepted for '" + key + "' (written like '" +
                   setting.DefaultToString() + "')";
        }
        NoteSource(key, "command line");
    }
    return {};
}

std::vector<std::string> UndeclaredSwitches(const std::vector<EnvironmentSwitch>& switches,
                                            const std::vector<std::string>& items) {
    const std::vector<std::string> keys = AllowedKeys(items);
    std::vector<std::string> undeclared;
    for (const auto& entry : switches) {
        if (!IsHarnessSwitch(entry.name) &&
            std::find(keys.begin(), keys.end(), entry.name) == keys.end()) {
            undeclared.push_back(entry.name);
        }
    }
    return undeclared;
}

void ResetToBaseline(Values& target) {
    for (const auto& [key, setting] : target.linkage.by_key) {
        if (IsBaselineSetting(key, *setting)) {
            setting->SetGlobal(true);
            setting->LoadString("");
        }
    }
}

void ResetToBaseline() {
    ResetToBaseline(Settings::values);
}

nlohmann::json ReportJson(bool requested, const std::vector<std::string>& declared,
                          const std::vector<Deviation>& deviations,
                          const std::vector<std::string>& harness) {
    using Json = nlohmann::json;
    Json list = Json::array();
    for (const auto& deviation : deviations) {
        Json item = Json::object();
        item["key"] = deviation.key;
        item["baseline"] = deviation.baseline;
        item["effective"] = deviation.effective;
        item["source"] = deviation.source;
        list.push_back(std::move(item));
    }
    Json harness_list = Json::array();
    for (const auto& name : harness) {
        harness_list.push_back(name);
    }
    Json declared_list = Json::array();
    for (const auto& key : declared) {
        declared_list.push_back(key);
    }
    Json result = Json::object();
    result["version"] = kBaselineVersion;
    result["hash"] = Hash();
    result["requested"] = requested;
    result["on_baseline"] = OnBaseline(deviations, declared);
    result["deviations"] = std::move(list);
    result["declared"] = std::move(declared_list);
    result["harness"] = std::move(harness_list);
    return result;
}

nlohmann::json ReportJson(bool requested, const std::vector<std::string>& declared) {
    return ReportJson(requested, declared, Deviations(), HarnessSwitches(SetEnvironmentSwitches()));
}

std::string PrintableText() {
    const auto entries = Entries();
    std::string text = "baseline_version=" + std::to_string(kBaselineVersion) + "\n";
    text += "baseline_hash=" + Hash(entries) + "\n";
    text += CanonicalText(entries);
    return text;
}

} // namespace Settings::Baseline
