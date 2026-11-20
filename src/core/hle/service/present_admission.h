// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include "common/common_types.h"

namespace Service::PresentAdmission {

constexpr bool HasNewLegacyComposition(bool acquired_new_buffer, bool composition_empty) noexcept {
    return acquired_new_buffer && !composition_empty;
}

constexpr bool HasMappedGsvImage(const std::optional<u64>& device_address) noexcept {
    return device_address.has_value();
}

} // namespace Service::PresentAdmission
