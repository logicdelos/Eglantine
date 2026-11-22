// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>

#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/k_thread_queue.h"
#include "core/hle/kernel/svc_common.h"

namespace Kernel {

// One ordered registration per input handle, including duplicates. The waiting KThread owns this
// queue across host fiber switches; callers' handle arrays are not retained. Access under the
// scheduler lock. These are runtime links, not pointers suitable for a checkpoint file.
class KSynchronizationWait final : public KThreadQueueWithoutEndWait {
public:
    using KThreadQueueWithoutEndWait::KThreadQueueWithoutEndWait;

    void Link(KThread* thread, std::span<KSynchronizationObject* const> objects);
    void NotifyAvailable(KernelCore& kernel, KThread* thread, KSynchronizationObject* object,
                         Result result) override;
    void CancelWait(KernelCore& kernel, KThread* thread, Result result, bool cancel_timer) override;

private:
    s32 Unlink(KSynchronizationObject* signaled_object);

    struct Entry {
        KSynchronizationObject::ThreadListNode node;
        KSynchronizationObject* object;
    };
    std::array<Entry, Svc::ArgumentHandleCountMax> entries;
    s32 count{};
};

} // namespace Kernel
