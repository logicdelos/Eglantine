// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>

namespace VideoCore::Replay {

// Per GPU. Capture is armed before guest execution starts and stays armed through teardown:
// stopping the recorder cannot let a publisher bypass a cut that is still returning. On an
// ordinary session channel/mapping publication takes no lock.
class CutPublication {
public:
    [[nodiscard]] bool ArmBeforeRun() {
        std::lock_guard lock{mutex};
        if (started) {
            return false;
        }
        armed = true;
        return true;
    }

    void BeforeGpuStart() {
        std::lock_guard lock{mutex};
        started = true;
    }

    [[nodiscard]] std::unique_lock<std::mutex> Lock() {
        return armed ? std::unique_lock{mutex} : std::unique_lock<std::mutex>{};
    }

private:
    bool armed = false;
    bool started = false;
    std::mutex mutex;
};

} // namespace VideoCore::Replay
