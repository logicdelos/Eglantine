// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

#include "common/settings_secret.h"

namespace Settings {

namespace {

bool Contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

} // Anonymous namespace

bool IsSecretSetting(std::string_view name) {
    std::string lowered{name};
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    constexpr std::array<std::string_view, 4> parts{"token", "password", "passwd", "secret"};
    for (const std::string_view part : parts) {
        if (Contains(lowered, part)) {
            return true;
        }
    }
    constexpr std::string_view suffix{"_key"};
    return lowered.size() >= suffix.size() &&
           std::string_view{lowered}.substr(lowered.size() - suffix.size()) == suffix;
}

} // namespace Settings
