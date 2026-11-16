// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/common_types.h"
#include "common/settings.h"
#include "common/settings_common.h"
#include "common/settings_secret.h"
#include "yuzu_cmd/cli_settings.h"

namespace CliSettings {

namespace {

using Json = nlohmann::json;
using Settings::BasicSetting;

enum class Kind {
    Bool,
    String,
    Enum,
    Integer,
    Float,
    Other, // a whole number that is neither of the above (an optional number)
};

Kind KindOf(const BasicSetting& setting) {
    if (setting.TypeId() == "bool") {
        return Kind::Bool;
    }
    if (setting.TypeId() == "string") {
        return Kind::String;
    }
    if (setting.IsEnum()) {
        return Kind::Enum;
    }
    if (setting.IsFloatingPoint()) {
        return Kind::Float;
    }
    if (setting.IsIntegral()) {
        return Kind::Integer;
    }
    return Kind::Other;
}

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Bool:
        return "bool";
    case Kind::String:
        return "string";
    case Kind::Enum:
        return "enum";
    case Kind::Integer:
        return "integer";
    case Kind::Float:
        return "float";
    case Kind::Other:
        break;
    }
    return "other";
}

/// A whole number of any sign up to 64 bits, kept apart from the sign so the comparison needs no
/// wider type.
struct Whole {
    bool negative = false;
    u64 magnitude = 0;

    [[nodiscard]] std::string ToText() const {
        return (negative ? "-" : "") + std::to_string(magnitude);
    }
};

bool Less(const Whole& lhs, const Whole& rhs) {
    if (lhs.negative != rhs.negative) {
        return lhs.negative;
    }
    return lhs.negative ? lhs.magnitude > rhs.magnitude : lhs.magnitude < rhs.magnitude;
}

/// Decimal digits with an optional sign; anything else, and anything that overflows 64 bits, is
/// refused.
std::optional<Whole> ParseWhole(std::string_view text) {
    Whole result;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        result.negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const u64 digit = static_cast<u64>(character - '0');
        if (result.magnitude > (std::numeric_limits<u64>::max() - digit) / 10) {
            return std::nullopt;
        }
        result.magnitude = result.magnitude * 10 + digit;
    }
    if (result.magnitude == 0) {
        result.negative = false;
    }
    return result;
}

std::optional<double> ParseNumber(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0' || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::string Lowered(std::string_view text) {
    std::string result{text};
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

using Choices = std::vector<std::pair<long long, std::string>>;

/// The numbers an enum setting accepts, with the name of each. The registry only offers
/// LoadString (which clamps) and Canonicalize, so each number is tried and kept while the setting
/// reads it back unchanged; the setting is put back as it was.
Choices EnumChoices(BasicSetting& setting) {
    constexpr long long limit = 4096;
    const std::string saved = setting.ToString();
    Choices choices;
    for (long long index = 0; index < limit; ++index) {
        const std::string text = std::to_string(index);
        setting.LoadString(text);
        if (setting.ToString() != text) {
            break;
        }
        choices.emplace_back(index, setting.Canonicalize());
    }
    setting.LoadString(saved);
    return choices;
}

std::string DescribeChoices(const Choices& choices) {
    std::string text;
    for (const auto& [index, name] : choices) {
        text += fmt::format("{}{} ({})", text.empty() ? "" : ", ", name, index);
    }
    return text;
}

/// The text to give LoadString for `value`, or the reason it is not valid for `setting`.
std::optional<std::string> ResolveValue(BasicSetting& setting, const std::string& value,
                                        std::string& reason) {
    switch (KindOf(setting)) {
    case Kind::Bool: {
        const std::string lowered = Lowered(value);
        if (lowered == "true" || lowered == "1") {
            return "true";
        }
        if (lowered == "false" || lowered == "0") {
            return "false";
        }
        reason = "expected true, false, 1 or 0";
        return std::nullopt;
    }
    case Kind::String:
        // An empty text makes LoadString use the default value.
        return value;
    case Kind::Enum: {
        const Choices choices = EnumChoices(setting);
        if (const auto number = ParseWhole(value)) {
            for (const auto& [index, name] : choices) {
                if (!number->negative && number->magnitude == static_cast<u64>(index)) {
                    return std::to_string(index);
                }
            }
        } else {
            const std::string wanted = Lowered(value);
            for (const auto& [index, name] : choices) {
                if (!wanted.empty() && Lowered(name) == wanted) {
                    return std::to_string(index);
                }
            }
        }
        reason = "expected one of: " + DescribeChoices(choices);
        return std::nullopt;
    }
    case Kind::Float: {
        const auto number = ParseNumber(value);
        const auto low = ParseNumber(setting.MinVal());
        const auto high = ParseNumber(setting.MaxVal());
        if (number && (!setting.Ranged() || !low || !high || (*low <= *number && *number <= *high))) {
            return value;
        }
        reason = setting.Ranged() ? fmt::format("expected a number from {} to {}", setting.MinVal(),
                                                setting.MaxVal())
                                  : std::string{"expected a number"};
        return std::nullopt;
    }
    case Kind::Integer:
    case Kind::Other: {
        Whole low;
        Whole high{false, std::numeric_limits<u32>::max()};
        if (KindOf(setting) == Kind::Integer) {
            const auto parsed_low = ParseWhole(setting.MinVal());
            const auto parsed_high = ParseWhole(setting.MaxVal());
            if (parsed_low && parsed_high) {
                low = *parsed_low;
                high = *parsed_high;
            }
        }
        const auto number = ParseWhole(value);
        if (number && !Less(*number, low) && !Less(high, *number)) {
            return number->ToText();
        }
        reason = fmt::format("expected a whole number from {} to {}", low.ToText(), high.ToText());
        return std::nullopt;
    }
    }
    return std::nullopt;
}

/// `text` (the canonical text of a setting) as a JSON value of the setting's own type.
Json Typed(Kind kind, const std::string& text) {
    switch (kind) {
    case Kind::Bool:
        return text == "true";
    case Kind::Integer:
    case Kind::Enum: {
        const auto number = ParseWhole(text);
        if (number && number->negative && number->magnitude <= (u64{1} << 63)) {
            return static_cast<long long>(0 - number->magnitude);
        }
        if (number && !number->negative) {
            return number->magnitude;
        }
        return text;
    }
    case Kind::Float: {
        const auto number = ParseNumber(text);
        return number ? Json(*number) : Json(text);
    }
    case Kind::String:
    case Kind::Other:
        break;
    }
    return text;
}

} // Anonymous namespace

std::string Resolve(const std::vector<std::pair<std::string, std::string>>& requests,
                    std::vector<Resolved>& resolved) {
    std::vector<Resolved> result;
    result.reserve(requests.size());
    for (const auto& [name, value] : requests) {
        const auto found = Settings::values.linkage.by_key.find(name);
        if (found == Settings::values.linkage.by_key.end()) {
            return fmt::format("--set {}: unknown setting (--list-settings shows them all)", name);
        }
        std::string reason;
        auto text = ResolveValue(*found->second, value, reason);
        if (!text) {
            // The value of a credential is not repeated.
            if (Settings::IsSecretSetting(name)) {
                return fmt::format("--set {}: invalid value: {}", name, reason);
            }
            return fmt::format("--set {}: invalid value '{}': {}", name, value, reason);
        }
        result.push_back({found->second, std::move(*text)});
    }
    resolved = std::move(result);
    return {};
}

void Apply(const std::vector<Resolved>& resolved) {
    for (const Resolved& entry : resolved) {
        entry.setting->LoadString(entry.text);
    }
}

std::string ListJsonLines() {
    std::string lines;
    for (const auto& [key, setting] : Settings::values.linkage.by_key) {
        const Kind kind = KindOf(*setting);
        Json line = Json::object();
        line["name"] = key;
        const char* const category = Settings::TranslateCategory(setting->GetCategory());
        line["category"] = category != nullptr ? category : "";
        line["type"] = KindName(kind);
        line["saved"] = setting->Save();
        if (Settings::IsSecretSetting(key)) {
            // Name, type and category only: neither the value nor the default is printed.
            line["redacted"] = true;
            lines += line.dump(-1, ' ', false, Json::error_handler_t::replace);
            lines += '\n';
            continue;
        }
        line["value"] = Typed(kind, setting->ToString());
        line["default"] = Typed(kind, setting->DefaultToString());
        if (kind == Kind::Enum) {
            const Choices choices = EnumChoices(*setting);
            line["value_name"] = setting->Canonicalize();
            const auto default_index = ParseWhole(setting->DefaultToString());
            for (const auto& [index, name] : choices) {
                if (default_index && !default_index->negative &&
                    default_index->magnitude == static_cast<u64>(index)) {
                    line["default_name"] = name;
                }
            }
            Json array = Json::array();
            for (const auto& [index, name] : choices) {
                Json choice = Json::object();
                choice["value"] = index;
                choice["name"] = name;
                array.push_back(std::move(choice));
            }
            line["choices"] = std::move(array);
        } else if ((kind == Kind::Integer || kind == Kind::Float) && setting->Ranged()) {
            line["min"] = Typed(kind, setting->MinVal());
            line["max"] = Typed(kind, setting->MaxVal());
        }
        lines += line.dump(-1, ' ', false, Json::error_handler_t::replace);
        lines += '\n';
    }
    return lines;
}

} // namespace CliSettings
