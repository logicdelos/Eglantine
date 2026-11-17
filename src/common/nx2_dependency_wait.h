// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "common/nx2_sampler.h"

namespace Nx2Sampler {

// Only used with dependency tracing enabled. The detached watchdog may outlive the emulator
// system, so store text (never kernel objects) in a process-lifetime registry.
class DependencyWait final {
public:
    DependencyWait() = default;
    DependencyWait(const DependencyWait&) = delete;
    DependencyWait& operator=(const DependencyWait&) = delete;

    void Begin(std::uint64_t thread, std::string description) {
        active = true;
        auto& waits = Active();
        std::scoped_lock lock{waits.mutex};
        waits.entries.insert_or_assign(this, std::pair{thread, std::move(description)});
    }

    ~DependencyWait() {
        if (active) {
            auto& waits = Active();
            std::scoped_lock lock{waits.mutex};
            waits.entries.erase(this);
        }
    }

    static std::string Snapshot() {
        auto& waits = Active();
        std::scoped_lock lock{waits.mutex};
        std::string text{"Active guest dependency waits (guest TID):\n"};
        for (const auto& entry : waits.entries) {
            const auto& [thread, description] = entry.second;
            fmt::format_to(std::back_inserter(text), "  t{} {}\n", thread, description);
        }
        return text;
    }

private:
    struct Registry {
        std::mutex mutex;
        std::map<const DependencyWait*, std::pair<std::uint64_t, std::string>> entries;
    };

    static Registry& Active() {
        static auto* const waits = new Registry;
        return *waits;
    }

    bool active{};
};

} // namespace Nx2Sampler
