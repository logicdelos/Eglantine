// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/common_types.h"
#include "common/nx2_dependency_wait.h"
#include "core/hle/kernel/k_synchronization_wait.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_scheduler_lock_and_sleep.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_thread_queue.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/kernel/svc_results.h"

namespace Kernel {

void KSynchronizationWait::Link(KThread* thread, std::span<KSynchronizationObject* const> objects) {
    ASSERT(count == 0 && objects.size() <= entries.size());
    for (auto* object : objects) {
        auto& entry = entries[count++];
        entry.node = {.next = nullptr, .thread = thread};
        entry.object = object;
        object->LinkNode(&entry.node);
    }
}

s32 KSynchronizationWait::Unlink(KSynchronizationObject* signaled_object) {
    s32 index = -1;
    for (s32 i = 0; i < count; ++i) {
        auto& entry = entries[i];
        if (entry.object == signaled_object && index < 0) {
            index = i;
        }
        entry.object->UnlinkNode(&entry.node);
        entry.object = nullptr;
    }
    count = 0;
    return index;
}

void KSynchronizationWait::NotifyAvailable(KernelCore& kernel, KThread* thread,
                                           KSynchronizationObject* object, Result result) {
    thread->SetSyncedIndex(Unlink(object));
    thread->ClearCancellable();
    KThreadQueue::EndWait(kernel, thread, result);
}

void KSynchronizationWait::CancelWait(KernelCore& kernel, KThread* thread, Result result,
                                      bool cancel_timer) {
    Unlink(nullptr);
    thread->ClearCancellable();
    KThreadQueue::CancelWait(kernel, thread, result, cancel_timer);
}

void KSynchronizationObject::Finalize(KernelCore& kernel) {
    this->OnFinalizeSynchronizationObject();
    KAutoObject::Finalize(kernel);
}

Result KSynchronizationObject::Wait(KernelCore& kernel, s32* out_index,
                                    KSynchronizationObject** objects, const s32 num_objects,
                                    s64 timeout) {
    // Prepare for wait.
    KThread* thread = GetCurrentThreadPointer(kernel);
    KHardwareTimer* timer{};

    Nx2Sampler::DependencyWait dependency_wait;
    {
        // Setup the scheduling lock and sleep.
        KScopedSchedulerLockAndSleep slp(kernel, std::addressof(timer), thread, timeout);

        // Check if the thread should terminate.
        if (thread->IsTerminationRequested()) {
            slp.CancelSleep();
            R_THROW(ResultTerminationRequested);
        }

        // Check if any of the objects are already signaled.
        for (auto i = 0; i < num_objects; ++i) {
            ASSERT(objects[i] != nullptr);

            if (objects[i]->IsSignaled(kernel)) {
                *out_index = i;
                slp.CancelSleep();
                R_THROW(ResultSuccess);
            }
        }

        // Check if the timeout is zero.
        if (timeout == 0) {
            slp.CancelSleep();
            R_THROW(ResultTimedOut);
        }

        // Check if waiting was canceled.
        if (thread->IsWaitCancelled()) {
            slp.CancelSleep();
            thread->ClearWaitCancelled();
            R_THROW(ResultCancelled);
        }

        // Allocate once, only for threads that actually block. Never publish stack addresses to
        // kernel objects; even internal waits may supply a caller-local array of object pointers.
        if (!thread->m_sync_wait) {
            thread->m_sync_wait = std::make_unique<KSynchronizationWait>(kernel);
        }
        auto& wait_queue = *thread->m_sync_wait;
        wait_queue.Link(thread, {objects, static_cast<size_t>(num_objects)});

        // Mark the thread as cancellable.
        thread->SetCancellable();

        // Clear the thread's synced index.
        thread->SetSyncedIndex(-1);

        if (Nx2Sampler::DependencyTraceEnabled()) {
            std::string description = fmt::format("sync objects timeout={} [", timeout);
            for (auto i = 0; i < num_objects; ++i) {
                auto* object = objects[i];
                fmt::format_to(std::back_inserter(description), "{}{}", i == 0 ? "" : ", ",
                               fmt::ptr(object));
                if (const auto* event = object->DynamicCast<KReadableEvent*>()) {
                    fmt::format_to(std::back_inserter(description), "({})",
                                   Service::KernelHelpers::DebugEventName(event));
                } else if (const auto* target = object->DynamicCast<KThread*>()) {
                    fmt::format_to(std::back_inserter(description), "(thread t{})",
                                   target->GetThreadId());
                }
            }
            description += ']';
            dependency_wait.Begin(thread->GetThreadId(), std::move(description));
        }
        // Wait for an object to be signaled.
        wait_queue.SetHardwareTimer(timer);
        thread->BeginWait(kernel, std::addressof(wait_queue));
        thread->SetWaitReasonForDebugging(ThreadWaitReasonForDebugging::Synchronization);
    }

    // Set the output index.
    *out_index = thread->GetSyncedIndex();

    // Get the wait result.
    R_RETURN(thread->GetWaitResult());
}

KSynchronizationObject::KSynchronizationObject(KernelCore& kernel) : KAutoObjectWithList{kernel} {}

KSynchronizationObject::~KSynchronizationObject() = default;

void KSynchronizationObject::NotifyAvailable(KernelCore& kernel, Result result) {
    KScopedSchedulerLock sl(kernel);

    // If we're not signaled, we've nothing to notify.
    if (!this->IsSignaled(kernel)) {
        return;
    }

    // Iterate over each thread.
    for (auto* cur_node = m_thread_list_head; cur_node != nullptr; cur_node = cur_node->next) {
        cur_node->thread->NotifyAvailable(kernel, this, result);
    }
}

std::vector<KThread*> KSynchronizationObject::GetWaitingThreadsForDebugging(KernelCore& kernel) const {
    std::vector<KThread*> threads;

    // If debugging, dump the list of waiters.
    {
        KScopedSchedulerLock lock(kernel);
        for (auto* cur_node = m_thread_list_head; cur_node != nullptr; cur_node = cur_node->next) {
            threads.emplace_back(cur_node->thread);
        }
    }

    return threads;
}
} // namespace Kernel
