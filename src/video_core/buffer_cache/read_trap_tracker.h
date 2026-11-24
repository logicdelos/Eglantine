// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <type_traits>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/range_sets.h"
#include "common/range_sets.inc"

namespace VideoCommon {

class ReadTrapTracker {
public:
    /// Arms this page-aligned range. Calls `arm(address, size)` for each part that was not armed
    /// yet, the parts the core must be notified of, and returns whether there was any.
    ///
    /// `arm` may return a bool: false says the core could not arm that part (a page it does not
    /// track as GPU cached), and the part is then not recorded, so the next Arm offers it again.
    /// A callback returning void records every part.
    template <typename Func>
    bool Arm(DAddr address, u64 size, Func&& arm) {
        const DAddr end = address + size;
        DAddr next = address;
        bool any = false;
        boost::container::small_vector<std::pair<DAddr, u64>, 4> armed_parts;
        const auto notify = [&](DAddr part_address, u64 part_size) {
            any = true;
            if constexpr (std::is_same_v<std::invoke_result_t<Func&, DAddr, u64>, bool>) {
                if (!arm(part_address, part_size)) {
                    return;
                }
            } else {
                arm(part_address, part_size);
            }
            armed_parts.emplace_back(part_address, part_size);
        };
        ranges.ForEachInRange(address, size, [&](DAddr lower, DAddr upper) {
            if (lower > next) {
                notify(next, lower - next);
            }
            next = (std::max)(next, upper);
        });
        if (next < end) {
            notify(next, end - next);
        }
        for (const auto& [part_address, part_size] : armed_parts) {
            ranges.Add(part_address, part_size);
        }
        return any;
    }

    /// Returns whether the core must be notified to arm this page-aligned range.
    bool Arm(DAddr address, u64 size) {
        return Arm(address, size, [](DAddr, u64) {});
    }

    /// A CPU read consumes the core's trap; unmapping also forgets the range.
    void Subtract(DAddr address, u64 size) {
        ranges.Subtract(address, size);
    }

private:
    Common::RangeSet<DAddr> ranges;
};

} // namespace VideoCommon
