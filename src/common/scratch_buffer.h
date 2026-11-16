// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <cstdlib>
#include <iterator>
#include <memory>

#include "common/logging.h"
#include "common/memory_accounting.h"

namespace Common {

/**
 * ScratchBuffer class
 * This class creates a default initialized heap allocated buffer for cases such as intermediate
 * buffers being copied into entirely, where value initializing members during allocation or resize
 * is redundant.
 */
template <typename T>
class ScratchBuffer {
public:
    using element_type = T;
    using value_type = T;
    using size_type = size_t;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using iterator = pointer;
    using const_iterator = const_pointer;
    using iterator_category = std::random_access_iterator_tag;
    using iterator_concept = std::contiguous_iterator_tag;

    ScratchBuffer() = default;
    explicit ScratchBuffer(const char* name_) : name{name_} {}

    explicit ScratchBuffer(size_type initial_capacity, const char* name_ = "CPU scratch")
        : last_requested_size{initial_capacity}
        , buffer_capacity{initial_capacity}
        , buffer{std::make_unique_for_overwrite<T[]>(initial_capacity)}
        , name{name_} {
        Register();
    }

    ~ScratchBuffer() {
        Unregister();
    }
    ScratchBuffer(const ScratchBuffer&) = delete;
    ScratchBuffer& operator=(const ScratchBuffer&) = delete;

    ScratchBuffer(ScratchBuffer&& other) noexcept {
        swap(other);
        other.last_requested_size = 0;
        other.buffer_capacity = 0;
        other.buffer.reset();
    }

    ScratchBuffer& operator=(ScratchBuffer&& other) noexcept {
        swap(other);
        other.last_requested_size = 0;
        other.Unregister();
        other.buffer_capacity = 0;
        other.buffer.reset();
        return *this;
    }

    /// This will only grow the buffer's capacity if size is greater than the current capacity.
    /// The previously held data will remain intact.
    void resize(size_type size) {
        if (size > buffer_capacity) {
            auto new_buffer = std::make_unique_for_overwrite<T[]>(size);
            std::memcpy(new_buffer.get(), buffer.get(), buffer_capacity * sizeof(T));
            Unregister();
            buffer = std::move(new_buffer);
            buffer_capacity = size;
            Register();
        }
        last_requested_size = size;
        if (idle_frame_limit != 0) {
            used_since_tick = true;
        }
    }

    /// This will only grow the buffer's capacity if size is greater than the current capacity.
    /// The previously held data will be destroyed if a reallocation occurs.
    void resize_destructive(size_type size) {
        if (size > buffer_capacity) {
            Unregister();
            buffer_capacity = size;
            buffer = std::make_unique_for_overwrite<T[]>(buffer_capacity);
            Register();
        }
        last_requested_size = size;
        if (idle_frame_limit != 0) {
            used_since_tick = true;
        }
    }

    [[nodiscard]] pointer data() noexcept {
        return buffer.get();
    }

    [[nodiscard]] const_pointer data() const noexcept {
        return buffer.get();
    }

    [[nodiscard]] iterator begin() noexcept {
        return data();
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return data();
    }

    [[nodiscard]] iterator end() noexcept {
        return data() + last_requested_size;
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return data() + last_requested_size;
    }

    [[nodiscard]] reference operator[](size_type i) {
        return buffer[i];
    }

    [[nodiscard]] const_reference operator[](size_type i) const {
        return buffer[i];
    }

    [[nodiscard]] size_type size() const noexcept {
        return last_requested_size;
    }

    [[nodiscard]] size_type capacity() const noexcept {
        return buffer_capacity;
    }

    /// Frees the buffer; the next resize allocates it again.
    void release() noexcept {
        Unregister();
        buffer.reset();
        buffer_capacity = 0;
        last_requested_size = 0;
    }

    /// Only call at an owner's frame boundary, with no outstanding references.
    void TickFrame() {
        if (idle_frame_limit == 0) {
            return;
        }
        if (used_since_tick) {
            idle_frames = 0;
            used_since_tick = false;
        } else if (buffer_capacity * sizeof(T) >= (size_t{8} << 20) &&
                   ++idle_frames >= idle_frame_limit) {
            const size_t released = buffer_capacity * sizeof(T);
            release();
            idle_frames = 0;
            LOG_INFO(HW_GPU, "Memory idle trim: {} released {:.2f} MiB after {} frames",
                     name, static_cast<double>(released) / 1048576.0, idle_frame_limit);
        }
    }

    void swap(ScratchBuffer& other) noexcept {
        std::swap(last_requested_size, other.last_requested_size);
        std::swap(buffer_capacity, other.buffer_capacity);
        std::swap(buffer, other.buffer);
        std::swap(name, other.name);
        std::swap(idle_frames, other.idle_frames);
        std::swap(used_since_tick, other.used_since_tick);
    }

private:
    static size_t IdleFrameLimit() {
        static const size_t limit = [] {
            const char* const value = std::getenv("EDEN_NX2_SCRATCH_IDLE_FRAMES");
            if (!value || value[0] < '0' || value[0] > '9') {
                return size_t{0};
            }
            char* end{};
            const auto frames = std::strtoull(value, &end, 10);
            return *end == '\0' && frames <= 36000 ? static_cast<size_t>(frames) : size_t{0};
        }();
        return limit;
    }

    void Register() {
        if (!MemoryAccounting::RegistrationEnabled()) {
            return;
        }
        if (buffer_capacity * sizeof(T) >= (size_t{1} << 20)) {
            MemoryAccounting::Register(name, buffer.get(), buffer_capacity * sizeof(T));
        }
    }

    void Unregister() {
        if (!MemoryAccounting::RegistrationEnabled()) {
            return;
        }
        if (buffer_capacity * sizeof(T) >= (size_t{1} << 20)) {
            MemoryAccounting::Unregister(buffer.get());
        }
    }

    size_type last_requested_size{};
    size_type buffer_capacity{};
    std::unique_ptr<T[]> buffer{};
    const char* name{"CPU scratch"};
    const size_t idle_frame_limit{IdleFrameLimit()};
    size_t idle_frames{};
    bool used_since_tick{};
};

} // namespace Common
