// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-FileCopyrightText: 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cctype>
#include <string>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <unordered_map>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/atomic_ops.h"
#include "common/common_types.h"
#include "common/heap_tracker.h"
#include "common/logging.h"
#include "common/page_table.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/armed_read_traps.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/gpu_dirty_memory_manager.h"
#include "video_core/nx2_hazard.h"
#include "video_core/nx2_spikes.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"
#include "video_core/rasterizer_download_area.h"
#include "video_core/replay/capture.h"

namespace Core::Memory {

u32& UnmappedReadFlag() {
    thread_local u32 flag{};
    return flag;
}

namespace {

/// EDEN_NX2_NO_REACTIVE=1 turns reactive flushing off for Switch 2 titles (read once).
bool NoReactiveRequested() {
    static const bool requested = [] {
        const char* const value = std::getenv("EDEN_NX2_NO_REACTIVE");
        return value != nullptr && value[0] == '1';
    }();
    return requested;
}

/// How an armed page stopped being armed (Verify's page history).
enum class VerifyPageEnd : u8 {
    None,
    Read,
    Uncached,
    Remapped,
};

/// Verify only: the life of a page in the shadow set, kept after it is released.
struct VerifyPageHistory {
    u64 arms{};
    u64 last_arm_ns{};
    u64 refused{};
    u64 last_refused_ns{};
    u64 last_end_ns{};
    VerifyPageEnd last_end{VerifyPageEnd::None};
};

/// What a miss reports about its page: the shadow set now, and the history.
struct VerifyPageReport {
    bool known{};
    bool armed_now{};
    bool cached_now{};
    u64 armed_total{};
    u64 arms{};
    u64 last_arm_age_ms{};
    u64 refused{};
    u64 last_refused_age_ms{};
    u64 last_end_age_ms{};
    VerifyPageEnd last_end{VerifyPageEnd::None};
};

/// Fills a VerifyPageReport for a guest address; `context` is the Impl that audits.
using VerifyProbe = void (*)(void* context, u64 vaddr, VerifyPageReport& report);

/// One guest read audited by Verify: what the shadow of armed pages says about its page.
struct VerifyAccess {
    bool active{};
    bool shadow_armed{};
    u64 vaddr{};
    void* context{};
    VerifyProbe probe{};
};

thread_local VerifyAccess t_verify_access{};
thread_local ReactiveFlushSource t_flush_source{ReactiveFlushSource::None};
thread_local u32 t_buffer_facts{};
thread_local bool t_page_consume{};

std::atomic<u64> g_verify_checks{};
std::atomic<u64> g_verify_requested{};
std::atomic<u64> g_verify_uncovered{};
std::atomic<u64> g_verify_armed_after{};
std::atomic<u64> g_verify_last_miss_log_ns{};
std::atomic<u64> g_verify_last_summary_ns{};

constexpr u64 VERIFY_MISS_LOG_INTERVAL_NS = 1'000'000'000;
constexpr u64 VERIFY_SUMMARY_INTERVAL_NS = 10'000'000'000;

u64 SteadyNowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

const char* SourceName(ReactiveFlushSource source) {
    switch (source) {
    case ReactiveFlushSource::Texture:
        return "texture";
    case ReactiveFlushSource::Buffer:
        return "buffer";
    case ReactiveFlushSource::None:
        break;
    }
    return "unknown";
}

/// Sets t_page_consume for the scope of one page-wide flush.
struct PageConsumeScope {
    PageConsumeScope() {
        t_page_consume = true;
    }
    ~PageConsumeScope() {
        t_page_consume = false;
    }
    PageConsumeScope(const PageConsumeScope&) = delete;
    PageConsumeScope& operator=(const PageConsumeScope&) = delete;
};

/// Sets t_verify_access for the scope of one audited read.
struct VerifyAccessScope {
    VerifyAccessScope(bool shadow_armed, u64 vaddr, void* context, VerifyProbe probe) {
        t_verify_access = VerifyAccess{true, shadow_armed, vaddr, context, probe};
        t_flush_source = ReactiveFlushSource::None;
        t_buffer_facts = 0;
    }
    ~VerifyAccessScope() {
        t_verify_access = VerifyAccess{};
        t_flush_source = ReactiveFlushSource::None;
        t_buffer_facts = 0;
    }
    VerifyAccessScope(const VerifyAccessScope&) = delete;
    VerifyAccessScope& operator=(const VerifyAccessScope&) = delete;
};

const char* VerifyEndName(VerifyPageEnd end) {
    switch (end) {
    case VerifyPageEnd::None:
        return "never";
    case VerifyPageEnd::Read:
        return "a guest read";
    case VerifyPageEnd::Uncached:
        return "the page leaving the GPU caches";
    case VerifyPageEnd::Remapped:
        return "a remap";
    }
    return "?";
}

/// What the shadow set and its history know about the page of a miss (written by the probe).
std::string DescribeVerifyPage(const VerifyPageReport& report) {
    std::string text = report.armed_now ? "is armed now (armed after the read looked), "
                                        : "is not armed, ";
    text += report.cached_now ? "cached" : "NOT cached";
    text += fmt::format(" ({} armed pages in all); ", report.armed_total);
    if (!report.known) {
        return text + "never armed or refused";
    }
    text += fmt::format("armed {}x, last {} ms ago; ended by {} {} ms ago; arm refused {}x, last {} "
                        "ms ago",
                        report.arms, report.last_arm_age_ms, VerifyEndName(report.last_end),
                        report.last_end_age_ms, report.refused, report.last_refused_age_ms);
    return text;
}

std::string DescribeBufferFacts(u32 facts) {
    if ((facts & ReactiveBufferFact::Valid) == 0) {
        return "not asked";
    }
    static constexpr std::pair<u32, const char*> NAMES[] = {
        {ReactiveBufferFact::TrackerFullyArmed, "tracker armed"},
        {ReactiveBufferFact::TrackerPartlyArmed, "tracker partly armed"},
        {ReactiveBufferFact::Preflushable, "read bytes preflushable"},
        {ReactiveBufferFact::GpuBitOnPage, "GPU bits on page"},
        {ReactiveBufferFact::ModifiedRangeOnPage, "modified range on page"},
        {ReactiveBufferFact::ModifiedRangeHitsRead, "modified range hits the read"},
        {ReactiveBufferFact::UndownloadedAtFence, "GPU bits no fence downloads"},
        {ReactiveBufferFact::AsyncDownloads, "async downloads"},
        {ReactiveBufferFact::QueuedUncommitted, "fence download uncommitted"},
        {ReactiveBufferFact::QueuedCommitted, "fence download committed"},
        {ReactiveBufferFact::QueuedPending, "fence download pending"},
    };
    std::string text;
    for (const auto& [bit, name] : NAMES) {
        if ((facts & bit) != 0) {
            text += text.empty() ? "" : ", ";
            text += name;
        }
    }
    return text.empty() ? "no flag set" : text;
}

} // Anonymous namespace

ReactiveFlushMode ReactiveFlushModeFromEnvironment() {
    const char* const value = std::getenv("EDEN_NX2_REACTIVE_MODE");
    if (value == nullptr || value[0] == '\0') {
        return ReactiveFlushMode::Blanket;
    }
    std::string text{value};
    std::ranges::transform(text, text.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    if (text == "blanket") {
        return ReactiveFlushMode::Blanket;
    }
    if (text == "verify") {
        LOG_INFO(HW_Memory, "EDEN_NX2_REACTIVE_MODE=verify: blanket read protection, with a check "
                            "of what precise flushing would have covered");
        return ReactiveFlushMode::Verify;
    }
    if (text == "precise") {
        LOG_INFO(HW_Memory, "EDEN_NX2_REACTIVE_MODE=precise: only pages the GPU modified and has "
                            "not written back lose read permission");
        return ReactiveFlushMode::Precise;
    }
    LOG_WARNING(HW_Memory, "EDEN_NX2_REACTIVE_MODE={} is not blanket, verify or precise; using "
                           "blanket",
                value);
    return ReactiveFlushMode::Blanket;
}

bool ReactiveFlushingActive() {
    return Settings::values.use_reactive_flushing.GetValue() &&
           !(NoReactiveRequested() && Settings::CurrentProgramIsSwitch2());
}

void SetReactiveFlushSource(ReactiveFlushSource source) {
    t_flush_source = source;
}

bool ReactivePageConsumeActive() {
    return t_page_consume;
}

void SetReactiveBufferFacts(u32 facts) {
    t_buffer_facts = facts;
}

void ReactiveVerifyDecision(DAddr addr, u64 size, bool requested_sync, u64 area_begin,
                            u64 area_end) {
    const VerifyAccess access = t_verify_access;
    const ReactiveFlushSource source = std::exchange(t_flush_source, ReactiveFlushSource::None);
    const u32 buffer_facts = std::exchange(t_buffer_facts, 0U);
    if (!access.active) {
        return;
    }
    g_verify_checks.fetch_add(1, std::memory_order_relaxed);
    if (requested_sync) {
        g_verify_requested.fetch_add(1, std::memory_order_relaxed);
        if (!access.shadow_armed) {
            // The blanket path flushes here but the precise one would have let the read through:
            // a producer did not arm this page (or released it too early), or a GPU write armed
            // it after the read looked (the page is armed again now: a race, not a hole).
            g_verify_uncovered.fetch_add(1, std::memory_order_relaxed);
            VerifyPageReport report;
            if (access.probe != nullptr) {
                access.probe(access.context, access.vaddr, report);
            }
            if (report.armed_now) {
                g_verify_armed_after.fetch_add(1, std::memory_order_relaxed);
            }
            const u64 now = SteadyNowNs();
            u64 last = g_verify_last_miss_log_ns.load(std::memory_order_relaxed);
            if ((last == 0 || now - last >= VERIFY_MISS_LOG_INTERVAL_NS) &&
                g_verify_last_miss_log_ns.compare_exchange_strong(last, now,
                                                                  std::memory_order_relaxed)) {
                LOG_WARNING(HW_GPU,
                            "reactive verify: flush at guest page {:#x} (device {:#x}, {} bytes, "
                            "area [{:#x}, {:#x}), source {}) was not covered by an armed trap; "
                            "page {}; buffer cache: {}",
                            access.vaddr & ~YUZU_PAGEMASK, addr, size, area_begin, area_end,
                            SourceName(source), DescribeVerifyPage(report),
                            DescribeBufferFacts(buffer_facts));
            }
        }
    }
    const u64 now = SteadyNowNs();
    u64 last_summary = g_verify_last_summary_ns.load(std::memory_order_relaxed);
    if (last_summary == 0) {
        g_verify_last_summary_ns.compare_exchange_strong(last_summary, now,
                                                         std::memory_order_relaxed);
    } else if (now - last_summary >= VERIFY_SUMMARY_INTERVAL_NS &&
               g_verify_last_summary_ns.compare_exchange_strong(last_summary, now,
                                                                std::memory_order_relaxed)) {
        LOG_INFO(HW_GPU,
                 "reactive verify: {} guest reads checked, {} requested a flush, {} of those not "
                 "covered by an armed trap ({} of them armed by a GPU write after the read "
                 "looked)",
                 g_verify_checks.load(std::memory_order_relaxed),
                 g_verify_requested.load(std::memory_order_relaxed),
                 g_verify_uncovered.load(std::memory_order_relaxed),
                 g_verify_armed_after.load(std::memory_order_relaxed));
    }
}

static inline bool AddressSpaceContains(const Common::PageTable& table, const Common::ProcessAddress addr, const std::size_t size) {
    const Common::ProcessAddress max_addr = 1ULL << table.GetAddressSpaceBits();
    return addr + size >= addr && addr + size <= max_addr;
}

// Implementation class used to keep the specifics of the memory subsystem hidden
// from outside classes. This also allows modification to the internals of the memory
// subsystem without needing to rebuild all files that make use of the memory interface.
struct Memory::Impl {
    explicit Impl(Core::System& system_) : system{system_} {}

    void SetCurrentPageTable(Kernel::KProcess& process) {
        current_page_table = &process.GetPageTable().GetImpl();

        if (process.IsApplication() && Settings::IsFastmemEnabled()) {
            current_page_table->fastmem_arena = system.DeviceMemory().buffer.VirtualBasePointer();
        } else {
            current_page_table->fastmem_arena = nullptr;
        }

#ifdef __ANDROID__
        heap_tracker.emplace(system.DeviceMemory().buffer);
        host_buffer = std::addressof(*heap_tracker);
#else
        host_buffer = std::addressof(system.DeviceMemory().buffer);
#endif
    }

    // A replay has no guest process: its own page table, never with the fastmem arena.
    void SetCurrentPageTable(Common::PageTable& page_table) {
        current_page_table = &page_table;
        current_page_table->fastmem_arena = nullptr;
#ifdef __ANDROID__
        heap_tracker.emplace(system.DeviceMemory().buffer);
        host_buffer = std::addressof(*heap_tracker);
#else
        host_buffer = std::addressof(system.DeviceMemory().buffer);
#endif
    }

    /// A page the core remaps or unmaps is no longer GPU cached: its armed trap is dropped, as
    /// the later mapping does not restore the page's protection.
    void ForgetArmedPagesOnRemap(const Common::PageTable& page_table, u64 vaddr, u64 size) {
        if (&page_table != current_page_table ||
            GetReactiveFlushMode() == ReactiveFlushMode::Blanket ||
            armed_read_trap_count.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lk{read_trap_mutex};
        EraseArmedPagesLocked(vaddr, size, VerifyPageEnd::Remapped);
    }

    void MapMemoryRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                         Common::PhysicalAddress target, Common::MemoryPermission perms,
                         bool separate_heap) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", GetInteger(base));
        ASSERT_MSG(target >= DramMemoryMap::Base, "Out of bounds target: {:016X}",
                   GetInteger(target));
        ForgetArmedPagesOnRemap(page_table, GetInteger(base), size);
        MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, target,
                 Common::PageType::Memory);

        if (current_page_table->fastmem_arena) {
            host_buffer->Map(GetInteger(base), GetInteger(target) - DramMemoryMap::Base, size, perms, separate_heap);
        }
    }

    void UnmapRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                     bool separate_heap) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", GetInteger(base));
        ForgetArmedPagesOnRemap(page_table, GetInteger(base), size);
        MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, 0,
                 Common::PageType::Unmapped);

        if (current_page_table->fastmem_arena) {
            host_buffer->Unmap(GetInteger(base), size, separate_heap);
        }
    }

    void ProtectRegion(Common::PageTable& page_table, VAddr vaddr, u64 size,
                       Common::MemoryPermission perms) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((vaddr & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", vaddr);

        if (!current_page_table->fastmem_arena) {
            return;
        }

        u64 protect_bytes = 0, protect_begin = 0;

        current_page_table->entries.CommitRegion(vaddr >> YUZU_PAGEBITS, (vaddr + size) >> YUZU_PAGEBITS);
        for (u64 addr = vaddr; addr < vaddr + size; addr += YUZU_PAGESIZE) {
            const Common::PageType page_type = current_page_table->entries.GetUnchecked(addr >> YUZU_PAGEBITS).Type();
            switch (page_type) {
            case Common::PageType::RasterizerCachedMemory:
                if (protect_bytes > 0) {
                    host_buffer->Protect(protect_begin, protect_bytes, perms);
                    protect_bytes = 0;
                }
                break;
            default:
                if (protect_bytes == 0)
                    protect_begin = addr;
                protect_bytes += YUZU_PAGESIZE;
            }
        }

        if (protect_bytes > 0) {
            host_buffer->Protect(protect_begin, protect_bytes, perms);
        }
    }

    [[nodiscard]] u8* GetPointerFromRasterizerCachedMemory(u64 vaddr) const {
        if (u64 paddr = current_page_table->entries[vaddr >> YUZU_PAGEBITS].Pointer(true); paddr)
            return reinterpret_cast<u8*>(paddr) + vaddr;
        return {};
    }

    [[nodiscard]] u8* GetPointerFromDebugMemory(u64 vaddr) const {
        if (u64 paddr = current_page_table->entries[vaddr >> YUZU_PAGEBITS].Pointer(true); paddr)
            return reinterpret_cast<u8*>(paddr) + vaddr;
        return {};
    }

    u8 Read8(const Common::ProcessAddress addr) {
        return Read<u8>(addr);
    }

    u16 Read16(const Common::ProcessAddress addr) {
        if ((addr & 1) == 0) {
            return Read<u16_le>(addr);
        } else {
            const u32 a{Read<u8>(addr)};
            const u32 b{Read<u8>(addr + sizeof(u8))};
            return static_cast<u16>((b << 8) | a);
        }
    }

    u32 Read32(const Common::ProcessAddress addr) {
        if ((addr & 3) == 0) {
            return Read<u32_le>(addr);
        } else {
            const u32 a{Read16(addr)};
            const u32 b{Read16(addr + sizeof(u16))};
            return (b << 16) | a;
        }
    }

    u64 Read64(const Common::ProcessAddress addr) {
        if ((addr & 7) == 0) {
            return Read<u64_le>(addr);
        } else {
            const u32 a{Read32(addr)};
            const u32 b{Read32(addr + sizeof(u32))};
            return (static_cast<u64>(b) << 32) | a;
        }
    }

    void Write8(const Common::ProcessAddress addr, const u8 data) {
        Write<u8>(addr, data);
    }

    void Write16(const Common::ProcessAddress addr, const u16 data) {
        if ((addr & 1) == 0) {
            Write<u16_le>(addr, data);
        } else {
            Write<u8>(addr, static_cast<u8>(data));
            Write<u8>(addr + sizeof(u8), static_cast<u8>(data >> 8));
        }
    }

    void Write32(const Common::ProcessAddress addr, const u32 data) {
        if ((addr & 3) == 0) {
            Write<u32_le>(addr, data);
        } else {
            Write16(addr, static_cast<u16>(data));
            Write16(addr + sizeof(u16), static_cast<u16>(data >> 16));
        }
    }

    void Write64(const Common::ProcessAddress addr, const u64 data) {
        if ((addr & 7) == 0) {
            Write<u64_le>(addr, data);
        } else {
            Write32(addr, static_cast<u32>(data));
            Write32(addr + sizeof(u32), static_cast<u32>(data >> 32));
        }
    }

    bool WriteExclusive8(const Common::ProcessAddress addr, const u8 data, const u8 expected) {
        return WriteExclusive<u8>(addr, data, expected);
    }

    bool WriteExclusive16(const Common::ProcessAddress addr, const u16 data, const u16 expected) {
        return WriteExclusive<u16_le>(addr, data, expected);
    }

    bool WriteExclusive32(const Common::ProcessAddress addr, const u32 data, const u32 expected) {
        return WriteExclusive<u32_le>(addr, data, expected);
    }

    bool WriteExclusive64(const Common::ProcessAddress addr, const u64 data, const u64 expected) {
        return WriteExclusive<u64_le>(addr, data, expected);
    }

    std::string ReadCString(Common::ProcessAddress vaddr, std::size_t max_length) {
        std::string string;
        string.reserve(max_length);
        for (std::size_t i = 0; i < max_length; ++i) {
            const char c = Read<s8>(vaddr);
            if (c == '\0') {
                break;
            }
            string.push_back(c);
            ++vaddr;
        }
        string.shrink_to_fit();
        return string;
    }

    template<typename F1, typename F2, typename F3>
    inline bool WalkBlock(const Common::ProcessAddress addr, const std::size_t size, F1&& on_unmapped, F2&& on_memory, F3&& on_rasterizer) {
        std::size_t offset = 0;
        if (!AddressSpaceContains(*current_page_table, addr, size)) {
            on_unmapped(offset, size, addr);
            return false;
        }
        std::size_t remaining_size = size;
        std::size_t page_index = addr >> YUZU_PAGEBITS;
        std::size_t page_offset = addr & YUZU_PAGEMASK;
        bool user_accessible = true;

        current_page_table->entries.CommitRegion(page_index, page_index + (size >> YUZU_PAGEBITS) + 1);
        while (remaining_size != 0) {
            const std::size_t copy_amount = (std::min)(std::size_t(YUZU_PAGESIZE) - page_offset, remaining_size);
            const auto current_vaddr = u64((page_index << YUZU_PAGEBITS) + page_offset);
            const auto [pointer, type, _] = current_page_table->entries.GetUnchecked(page_index).PointerTypeBlock();
            switch (type) {
            case Common::PageType::Unmapped: {
                user_accessible = false;
                on_unmapped(offset, copy_amount, current_vaddr);
                break;
            }
            case Common::PageType::Memory: {
                u8* mem_ptr = reinterpret_cast<u8*>(pointer + page_offset + (page_index << YUZU_PAGEBITS));
                on_memory(offset, copy_amount, mem_ptr);
                break;
            }
            case Common::PageType::DebugMemory: {
                u8* const mem_ptr = GetPointerFromDebugMemory(current_vaddr);
                on_memory(offset, copy_amount, mem_ptr);
                break;
            }
            case Common::PageType::RasterizerCachedMemory: {
                u8* const host_ptr = GetPointerFromRasterizerCachedMemory(current_vaddr);
                on_rasterizer(current_vaddr, offset, copy_amount, host_ptr);
                break;
            }
            default:
                UNREACHABLE();
            }
            page_index++;
            page_offset = 0;
            offset += copy_amount;
            remaining_size -= copy_amount;
        }
        return user_accessible;
    }

    bool ReadBlockImpl(const Common::ProcessAddress addr, void* buffer, const std::size_t size, bool unsafe) {
        return WalkBlock(addr, size,
        [addr, size, &buffer](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(addr), size);
            std::memset(reinterpret_cast<u8*>(buffer) + offset, 0, copy_amount);
        },
        [&](const std::size_t offset, const std::size_t copy_amount, const u8* const ptr) {
            std::memcpy(reinterpret_cast<u8*>(buffer) + offset, ptr, copy_amount);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, const u8* const ptr) {
            if (!unsafe) {
                HandleRasterizerDownload(GetInteger(current_vaddr), copy_amount);
            }
            std::memcpy(reinterpret_cast<u8*>(buffer) + offset, ptr, copy_amount);
        });
    }

    [[nodiscard]] inline const u8* GetSpan(const VAddr addr, const std::size_t size) const noexcept {
        const auto first = current_page_table->entries[addr >> YUZU_PAGEBITS].Block();
        const auto last = current_page_table->entries[(addr + size) >> YUZU_PAGEBITS].Block();
        if (Common::PageTable::PageEntryData::CanFastSpan(first, last)) {
            return GetPointerSilent(addr);
        }
        if (first != Common::PageTable::PageEntryData::INVALID_BLOCK_ID &&
            last != Common::PageTable::PageEntryData::INVALID_BLOCK_ID) {
            return nullptr;
        }
        return GetContiguousSpan(addr, size);
    }
    [[nodiscard]] inline u8* GetSpan(const VAddr addr, const std::size_t size) noexcept {
        const auto first = current_page_table->entries[addr >> YUZU_PAGEBITS].Block();
        const auto last = current_page_table->entries[(addr + size) >> YUZU_PAGEBITS].Block();
        if (Common::PageTable::PageEntryData::CanFastSpan(first, last)) {
            return GetPointerSilent(addr);
        }
        if (first != Common::PageTable::PageEntryData::INVALID_BLOCK_ID &&
            last != Common::PageTable::PageEntryData::INVALID_BLOCK_ID) {
            return nullptr;
        }
        return GetContiguousSpan(addr, size);
    }

    bool WriteBlockImpl(const Common::ProcessAddress addr, const void* buffer, const std::size_t size, bool unsafe) {
        return WalkBlock(addr, size,
        [addr, size](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(addr), size);
        },
        [&](const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            std::memcpy(ptr, reinterpret_cast<u8 const*>(buffer) + offset, copy_amount);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            if (!unsafe) {
                HandleRasterizerWrite(GetInteger(current_vaddr), copy_amount);
            }
            std::memcpy(ptr, reinterpret_cast<u8 const*>(buffer) + offset, copy_amount);
        });
    }

    bool ZeroBlock(const Common::ProcessAddress addr, const std::size_t size) {
        return WalkBlock(addr, size,
        [addr, size](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(addr), size);
        },
        [=](const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            std::memset(ptr, 0, copy_amount);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            HandleRasterizerWrite(GetInteger(current_vaddr), copy_amount);
            std::memset(ptr, 0, copy_amount);
        });
    }

    bool CopyBlock(Common::ProcessAddress dest_addr, Common::ProcessAddress src_addr, const std::size_t size) {
        return WalkBlock(dest_addr, size,
        [&](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
            LOG_ERROR(HW_Memory, "Unmapped @ {:#016x} (start address = {:#016x}, size = {})", GetInteger(current_vaddr), GetInteger(src_addr), size);
            ZeroBlock(dest_addr + offset, copy_amount);
        },
        [&](const std::size_t offset, const std::size_t copy_amount, const u8* const ptr) {
            WriteBlockImpl(dest_addr + offset, ptr, copy_amount, false);
        },
        [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const ptr) {
            HandleRasterizerDownload(GetInteger(current_vaddr), copy_amount);
            WriteBlockImpl(dest_addr + offset, ptr, copy_amount, false);
        });
    }

    template <typename Callback>
    Result PerformCacheOperation(Common::ProcessAddress dest_addr, std::size_t size, Callback&& cb) {
        bool had_unmapped = false;
        WalkBlock(dest_addr, size,
            [&](const std::size_t offset, const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
                LOG_ERROR(HW_Memory, "Unmapped @ {:#018X}", GetInteger(current_vaddr));
                had_unmapped = true;
            },
            [](const std::size_t offset, const std::size_t copy_amount, u8* const host_ptr) {},
            [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t copy_amount, u8* const host_ptr) { cb(current_vaddr, offset, copy_amount); }
        );
        return had_unmapped ? Kernel::ResultInvalidCurrentMemory : ResultSuccess;
    }

    Result InvalidateDataCache(Common::ProcessAddress dest_addr, std::size_t size) {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t block_size) {
            // dc ivac: Invalidate to point of coherency
            // GPU flush -> CPU invalidate
            HandleRasterizerDownload(GetInteger(current_vaddr), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    Result StoreDataCache(Common::ProcessAddress dest_addr, std::size_t size) {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t block_size) {
            // dc cvac: Store to point of coherency
            // CPU flush -> GPU invalidate
            HandleRasterizerWrite(GetInteger(current_vaddr), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    Result FlushDataCache(Common::ProcessAddress dest_addr, std::size_t size) {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr, const std::size_t offset, const std::size_t block_size) {
            // dc civac: Store to point of coherency, and invalidate from cache
            // CPU flush -> GPU invalidate
            HandleRasterizerWrite(GetInteger(current_vaddr), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    void MarkRegionDebug(u64 vaddr, u64 size, bool debug) {
        if (vaddr == 0 || !AddressSpaceContains(*current_page_table, vaddr, size)) {
            return;
        }

        if (debug) {
            // A watchpoint takes every permission away: no trap release may give them back.
            ForgetArmedPagesOnRemap(*current_page_table, vaddr, size);
        }

        if (current_page_table->fastmem_arena) {
            const auto perm{debug ? Common::MemoryPermission{}
                                  : Common::MemoryPermission::ReadWrite};
            host_buffer->Protect(vaddr, size, perm);
        }

        // Iterate over a contiguous CPU address space, marking/unmarking the region.
        // The region is at a granularity of CPU pages.

        const u64 num_pages = ((vaddr + size - 1) >> YUZU_PAGEBITS) - (vaddr >> YUZU_PAGEBITS) + 1;

        current_page_table->entries.CommitRegion(vaddr >> YUZU_PAGEBITS, (vaddr >> YUZU_PAGEBITS) + num_pages);
        for (u64 i = 0; i < num_pages; ++i, vaddr += YUZU_PAGESIZE) {
            auto& entry = current_page_table->entries.GetUnchecked(vaddr >> YUZU_PAGEBITS);
            const auto [pointer, type, block] = entry.PointerTypeBlock(true);
            if (debug) {
                // Switch page type to debug if now debug
                switch (type) {
                case Common::PageType::Unmapped:
                    ASSERT(false && "Attempted to mark unmapped pages as debug");
                    break;
                case Common::PageType::RasterizerCachedMemory:
                case Common::PageType::DebugMemory:
                    // Page is already marked.
                    break;
                case Common::PageType::Memory:
                    entry.MarkDebug(pointer, block);
                    break;
                default:
                    UNREACHABLE();
                }
            } else {
                // Switch page type to non-debug if now non-debug
                switch (type) {
                case Common::PageType::Unmapped:
                    ASSERT(false && "Attempted to mark unmapped pages as non-debug");
                    break;
                case Common::PageType::RasterizerCachedMemory:
                case Common::PageType::Memory:
                    // Don't mess with already non-debug or rasterizer memory.
                    break;
                case Common::PageType::DebugMemory: {
                    entry.Store(false, Common::PageType::Memory, block, pointer);
                    break;
                }
                default:
                    UNREACHABLE();
                }
            }
        }
    }

    void RasterizerMarkRegionCached(u64 vaddr, u64 size, bool cached) {
        if (vaddr == 0 || !AddressSpaceContains(*current_page_table, vaddr, size)) {
            return;
        }

        // Precise and Verify keep a set of armed pages. The lock covers the protection change and
        // the page type update, so an arm never sees a half-cached page and a cached armed page
        // is never readable.
        const ReactiveFlushMode trap_mode = ActiveReactiveMode();
        const bool precise = trap_mode == ReactiveFlushMode::Precise;
        std::unique_lock<std::mutex> trap_lock;
        if (trap_mode != ReactiveFlushMode::Blanket) {
            trap_lock = std::unique_lock<std::mutex>{read_trap_mutex};
            if (!cached) {
                // The pages are not GPU cached any more: their traps go with them.
                EraseArmedPagesLocked(vaddr, size, VerifyPageEnd::Uncached);
            }
        }

        if (current_page_table->fastmem_arena) {
            Common::MemoryPermission perm{};
            // EDEN_NX2_NO_REACTIVE=1 drops the read traps on Switch 2 programs. Opt-in: without
            // them a CPU read of GPU-written memory can see stale data.
            const bool reactive_flushing = ReactiveFlushingActive();
            // Precise keeps cached pages readable: only armed pages lose read permission.
            if (!reactive_flushing || !cached || precise) {
                perm |= Common::MemoryPermission::Read;
            }
            if (!cached) {
                perm |= Common::MemoryPermission::Write;
            }
            // EDEN_NX2_PROTECT_STATS=1: how many protection changes the tracker makes, and what
            // they cost, per 5 s (diagnostic).
            static const bool stats_enabled = std::getenv("EDEN_NX2_PROTECT_STATS") != nullptr;
            const bool spikes = VideoCommon::Nx2Spikes::Counting();
            const auto stats_start = stats_enabled || spikes
                                         ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
            if (precise && cached) {
                ProtectCachedReadOnlyAroundArmed(vaddr, size);
            } else {
                host_buffer->Protect(vaddr, size, perm);
            }
            if (spikes) {
                VideoCommon::Nx2Spikes::Note(VideoCommon::Nx2Spikes::Event::ProtectChange,
                                             std::chrono::steady_clock::now() - stats_start,
                                             size >> YUZU_PAGEBITS);
            }
            if (stats_enabled) [[unlikely]] {
                static std::atomic<u64> calls[2]{}, pages[2]{}, nanos[2]{};
                static std::atomic<u64> window_start{0};
                const auto now = std::chrono::steady_clock::now();
                const u64 now_ns = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
                        .count());
                const int side = cached ? 1 : 0;
                calls[side].fetch_add(1, std::memory_order_relaxed);
                pages[side].fetch_add(size >> YUZU_PAGEBITS, std::memory_order_relaxed);
                nanos[side].fetch_add(
                    static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now - stats_start)
                            .count()),
                    std::memory_order_relaxed);
                u64 started = window_start.load(std::memory_order_relaxed);
                if (started == 0) {
                    window_start.compare_exchange_strong(started, now_ns);
                } else if (now_ns - started > 5'000'000'000ULL &&
                           window_start.compare_exchange_strong(started, now_ns)) {
                    const double seconds = static_cast<double>(now_ns - started) / 1e9;
                    LOG_INFO(HW_Memory,
                             "nx2 protect: to read-only (tracked) {:.0f}/s ({:.0f} pages/s, {:.0f} "
                             "ms/s), to writable (untracked) {:.0f}/s ({:.0f} pages/s, {:.0f} ms/s)",
                             static_cast<double>(calls[1].exchange(0)) / seconds,
                             static_cast<double>(pages[1].exchange(0)) / seconds,
                             static_cast<double>(nanos[1].exchange(0)) / 1e6 / seconds,
                             static_cast<double>(calls[0].exchange(0)) / seconds,
                             static_cast<double>(pages[0].exchange(0)) / seconds,
                             static_cast<double>(nanos[0].exchange(0)) / 1e6 / seconds);
                }
            }
            if (!precise && cached && perm != Common::MemoryPermission{}) {
                ProtectArmedPages(vaddr, size);
            }
        }

        // Iterate over a contiguous CPU address space, which corresponds to the specified GPU
        // address space, marking the region as un/cached. The region is marked un/cached at a
        // granularity of CPU pages, hence why we iterate on a CPU page basis (note: GPU page size
        // is different). This assumes the specified GPU address region is contiguous as well.

        const u64 num_pages = ((vaddr + size - 1) >> YUZU_PAGEBITS) - (vaddr >> YUZU_PAGEBITS) + 1;
        current_page_table->entries.CommitRegion(vaddr >> YUZU_PAGEBITS, (vaddr >> YUZU_PAGEBITS) + num_pages);
        for (u64 i = 0; i < num_pages; ++i, vaddr += YUZU_PAGESIZE) {
            auto& entry = current_page_table->entries.GetUnchecked(vaddr >> YUZU_PAGEBITS);
            const Common::PageType page_type = entry.Type();
            if (cached) {
                // Switch page type to cached if now cached
                switch (page_type) {
                case Common::PageType::Unmapped:
                    // It is not necessary for a process to have this region mapped into its address
                    // space, for example, a system module need not have a VRAM mapping.
                    break;
                case Common::PageType::DebugMemory:
                case Common::PageType::Memory:
                    entry.MarkRasterizerCached();
                    break;
                case Common::PageType::RasterizerCachedMemory:
                    // There can be more than one GPU region mapped per CPU region, so it's common
                    // that this area is already marked as cached.
                    break;
                default:
                    UNREACHABLE();
                }
            } else {
                // Switch page type to uncached if now uncached
                switch (page_type) {
                case Common::PageType::Unmapped: // NOLINT(bugprone-branch-clone)
                    // It is not necessary for a process to have this region mapped into its address
                    // space, for example, a system module need not have a VRAM mapping.
                    break;
                case Common::PageType::DebugMemory:
                case Common::PageType::Memory:
                    // There can be more than one GPU region mapped per CPU region, so it's common
                    // that this area is already unmarked as cached.
                    break;
                case Common::PageType::RasterizerCachedMemory: {
                    if (auto [ptr, _, block] = entry.PointerTypeBlock(true); ptr == 0) {
                        // It's possible that this function has been called while updating the
                        // pagetable after unmapping a VMA. In that case the underlying VMA will no
                        // longer exist, and we should just leave the pagetable entry blank.
                        entry.Store(false, Common::PageType::Unmapped, block, 0);
                    } else {
                        entry.Store(false, Common::PageType::Memory, block, ptr);
                    }
                    break;
                }
                default:
                    UNREACHABLE();
                }
            }
        }
    }

    /// The mode that is in force for this process: Blanket unless EDEN_NX2_REACTIVE_MODE asks for
    /// more, reactive flushing is on, a fastmem arena exists and the CPU backend is not NCE (under
    /// NCE every fault is taken as a write, so a read trap cannot work).
    [[nodiscard]] ReactiveFlushMode ActiveReactiveMode() const {
        const ReactiveFlushMode mode = GetReactiveFlushMode();
        if (mode == ReactiveFlushMode::Blanket || current_page_table == nullptr ||
            !current_page_table->fastmem_arena || !ReactiveFlushingActive() ||
            Settings::values.cpu_backend.GetValue() == Settings::CpuBackend::Nce) {
            return ReactiveFlushMode::Blanket;
        }
        return mode;
    }

    /// Read traps replace reactive flushing's blanket read protection of every cached page: only
    /// pages the GPU writes are protected, and each one only until its first read
    /// (EDEN_NX2_READ_TRAPS=0 turns them off). Under NCE every fault is taken as a write, so they
    /// are not used there. With EDEN_NX2_REACTIVE_MODE=precise or verify they are used with
    /// reactive flushing on, and EDEN_NX2_READ_TRAPS=0 does not apply.
    bool ReadTrapsEnabled() const {
        if (ActiveReactiveMode() != ReactiveFlushMode::Blanket) {
            return true;
        }
        static const bool disabled = [] {
            const char* value = std::getenv("EDEN_NX2_READ_TRAPS");
            return value != nullptr && value[0] == '0';
        }();
        return !disabled && current_page_table->fastmem_arena &&
               !Settings::values.use_reactive_flushing.GetValue() &&
               Settings::values.cpu_backend.GetValue() != Settings::CpuBackend::Nce;
    }

    /// Keeps the lock-free count in step with the armed pages. The caller holds read_trap_mutex.
    void SyncArmedReadTrapCount() {
        armed_read_trap_count.store(armed_read_traps.Size(), std::memory_order_relaxed);
    }

    /// Forgets the armed pages of a range without touching their protection. The caller holds
    /// read_trap_mutex.
    void EraseArmedPagesLocked(u64 vaddr, u64 size, VerifyPageEnd why) {
        if (size == 0) {
            return;
        }
        const u64 first = vaddr >> YUZU_PAGEBITS;
        const u64 end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        if (ReactiveVerifyActive()) [[unlikely]] {
            armed_read_traps.ForEachInRange(first, end,
                                            [&](u64 page, u64) { NoteVerifyEnd(page, why); });
        }
        armed_read_traps.Erase(first, end);
        SyncArmedReadTrapCount();
    }

    /// Verify's history entry of a page, or null outside Verify (and for new pages once the
    /// history is full). The caller holds read_trap_mutex.
    VerifyPageHistory* VerifyHistoryFor(u64 page) {
        if (!ReactiveVerifyActive()) [[likely]] {
            return nullptr;
        }
        if (verify_history.size() >= VERIFY_HISTORY_LIMIT) {
            const auto it = verify_history.find(page);
            return it == verify_history.end() ? nullptr : &it->second;
        }
        return &verify_history[page];
    }

    void NoteVerifyEnd(u64 page, VerifyPageEnd end) {
        if (VerifyPageHistory* const history = VerifyHistoryFor(page)) {
            history->last_end = end;
            history->last_end_ns = SteadyNowNs();
        }
    }

    static constexpr size_t VERIFY_HISTORY_LIMIT = size_t{1} << 21;

    /// VerifyProbe: the shadow set and history of the page of a missed read.
    static void ProbeVerifyPage(void* context, u64 vaddr, VerifyPageReport& report) {
        Impl& self = *static_cast<Impl*>(context);
        const u64 page = vaddr >> YUZU_PAGEBITS;
        const u64 now = SteadyNowNs();
        const auto age_ms = [now](u64 then) -> u64 {
            return then == 0 || then > now ? 0 : (now - then) / 1'000'000;
        };
        std::scoped_lock lk{self.read_trap_mutex};
        report.armed_now = self.armed_read_traps.Version(page).has_value();
        report.armed_total = self.armed_read_traps.Size();
        report.cached_now = self.current_page_table != nullptr &&
                            std::as_const(self.current_page_table->entries)[page].Type() ==
                                Common::PageType::RasterizerCachedMemory;
        const auto it = self.verify_history.find(page);
        if (it == self.verify_history.end()) {
            return;
        }
        const VerifyPageHistory& history = it->second;
        report.known = true;
        report.arms = history.arms;
        report.last_arm_age_ms = age_ms(history.last_arm_ns);
        report.refused = history.refused;
        report.last_refused_age_ms = age_ms(history.last_refused_ns);
        report.last_end = history.last_end;
        report.last_end_age_ms = age_ms(history.last_end_ns);
    }

    /// Precise mode: gives read permission to the pages of a just cached range, except the armed
    /// ones, which stay without any. The caller holds read_trap_mutex, so no page is ever
    /// readable while armed.
    void ProtectCachedReadOnlyAroundArmed(u64 vaddr, u64 size) {
        const u64 first = vaddr >> YUZU_PAGEBITS;
        const u64 end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        u64 cursor = first;
        armed_read_traps.ForEachInRange(first, end, [&](u64 page, u64) {
            if (page > cursor) {
                host_buffer->Protect(cursor << YUZU_PAGEBITS, (page - cursor) << YUZU_PAGEBITS,
                                     Common::MemoryPermission::Read);
            }
            host_buffer->Protect(page << YUZU_PAGEBITS, YUZU_PAGESIZE, Common::MemoryPermission{});
            cursor = page + 1;
        });
        if (cursor < end) {
            host_buffer->Protect(cursor << YUZU_PAGEBITS, (end - cursor) << YUZU_PAGEBITS,
                                 Common::MemoryPermission::Read);
        }
    }

    /// Returns whether every page of the range is now armed (or already was).
    ///
    /// Precise mode takes read permission away from the pages that were not armed yet. Verify
    /// mode only records them in the shadow set: its pages keep the blanket protection.
    bool RasterizerArmReadTraps(u64 vaddr, u64 size) {
        if (Settings::values.use_reactive_flushing.GetValue()) {
            // Clean cached read areas also need invalidation when reactive flushing is enabled.
            read_trap_generation.fetch_add(1, std::memory_order_release);
        }
        if (vaddr == 0 || size == 0 || !AddressSpaceContains(*current_page_table, vaddr, size) ||
            !ReadTrapsEnabled()) {
            return true;
        }
        const ReactiveFlushMode mode = ActiveReactiveMode();
        const bool protect_pages = mode != ReactiveFlushMode::Verify;
        const u64 page_end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        std::scoped_lock lk{read_trap_mutex};
        u64 run_begin = 0;
        u64 run_pages = 0;
        bool complete = true;
        const auto protect_run = [&] {
            if (run_pages > 0) {
                // Reads that hit a cached read area must not skip the flush of a new trap.
                read_trap_generation.fetch_add(1, std::memory_order_release);
                if (protect_pages) {
                    host_buffer->Protect(run_begin << YUZU_PAGEBITS, run_pages << YUZU_PAGEBITS,
                                         Common::MemoryPermission{});
                }
                run_pages = 0;
            }
        };
        const u64 now = ReactiveVerifyActive() ? SteadyNowNs() : 0;
        for (u64 page = vaddr >> YUZU_PAGEBITS; page != page_end; ++page) {
            const bool cached = std::as_const(current_page_table->entries)[page].Type() ==
                                Common::PageType::RasterizerCachedMemory;
            if (!cached) {
                // Not a page the GPU caches (or the core does not map): nothing to trap.
                complete = false;
                if (VerifyPageHistory* const history = VerifyHistoryFor(page)) {
                    ++history->refused;
                    history->last_refused_ns = now;
                }
                protect_run();
                continue;
            }
            if (VerifyPageHistory* const history = VerifyHistoryFor(page)) {
                ++history->arms;
                history->last_arm_ns = now;
            }
            if (!armed_read_traps.Arm(page)) {
                // Armed already; Arm gave it a new version, which cancels a release in flight.
                protect_run();
                continue;
            }
            if (run_pages == 0) {
                run_begin = page;
            }
            ++run_pages;
        }
        protect_run();
        SyncArmedReadTrapCount();
        return mode == ReactiveFlushMode::Blanket || complete;
    }

    /// Takes read permission away again from armed pages that were just cached again.
    void ProtectArmedPages(u64 vaddr, u64 size) {
        if (armed_read_trap_count.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lk{read_trap_mutex};
        const u64 page_end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        armed_read_traps.ForEachInRange(vaddr >> YUZU_PAGEBITS, page_end, [&](u64 page, u64) {
            host_buffer->Protect(page << YUZU_PAGEBITS, YUZU_PAGESIZE, Common::MemoryPermission{});
        });
    }

    /// A slow-path read of a cached page: its trap, if any, has done its job.
    void DisarmReadTraps(u64 vaddr, size_t size) {
        if (armed_read_trap_count.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lk{read_trap_mutex};
        const u64 page_end = ((vaddr + size - 1) >> YUZU_PAGEBITS) + 1;
        armed_read_traps.EraseEach(vaddr >> YUZU_PAGEBITS, page_end, [&](u64 page) {
            // Still cached (this read came through the rasterizer path): writes stay trapped.
            host_buffer->Protect(page << YUZU_PAGEBITS, YUZU_PAGESIZE,
                                 Common::MemoryPermission::Read);
            VideoCommon::Nx2Spikes::Note(VideoCommon::Nx2Spikes::Event::ReadTrapHit);
        });
        SyncArmedReadTrapCount();
    }

    /**
     * Maps a region of pages as a specific type.
     *
     * @param page_table The page table to use to perform the mapping.
     * @param base       The base address to begin mapping at.
     * @param size       The total size of the range in bytes.
     * @param target     The target address to begin mapping from.
     * @param type       The page type to map the memory as.
     */
    void MapPages(Common::PageTable& page_table, Common::ProcessAddress base_address, u64 size,
                  Common::PhysicalAddress target, Common::PageType type) {
        auto base = GetInteger(base_address);

        LOG_DEBUG(HW_Memory, "Mapping {:016X} onto {:016X}-{:016X}", GetInteger(target),
                  base * YUZU_PAGESIZE, (base + size) * YUZU_PAGESIZE);

        const auto end = base + size;
        ASSERT_MSG(end <= page_table.entries.size(), "out of range mapping at {:016X}", base + page_table.entries.size());

        if (!target) {
            ASSERT_MSG(type != Common::PageType::Memory,
                       "Mapping memory page without a pointer @ {:016x}", base * YUZU_PAGESIZE);

            last_mapped_table = nullptr;
            page_table.entries.ZeroRegion(base, end);
        } else if (base != end) {
            const u64 first_host_ptr =
                reinterpret_cast<u64>(system.DeviceMemory().GetPointer<u8>(target)) -
                (base << YUZU_PAGEBITS);
            u16 current_block = Common::PageTable::PageEntryData::INVALID_BLOCK_ID;
            if (last_mapped_table == &page_table && base == last_mapped_end &&
                first_host_ptr == last_mapped_host_ptr &&
                last_mapped_block != Common::PageTable::PageEntryData::INVALID_BLOCK_ID) {
                current_block = last_mapped_block;
                last_mapped_end = end;
            } else {
                u64 mapping_id = block_count.load(std::memory_order_relaxed);
                constexpr u64 exhausted_count =
                    u64{Common::PageTable::PageEntryData::INVALID_BLOCK_ID} + 1;
                while (mapping_id < exhausted_count &&
                       !block_count.compare_exchange_weak(mapping_id, mapping_id + 1,
                                                          std::memory_order_relaxed)) {
                }
                current_block =
                    Common::PageTable::PageEntryData::BlockIdForMapping(mapping_id);
                if (mapping_id == Common::PageTable::PageEntryData::INVALID_BLOCK_ID) {
                    LOG_WARNING(HW_Memory,
                                "Page table block ids are exhausted; validating spans page by page");
                }
                last_mapped_table = &page_table;
                last_mapped_end = end;
                last_mapped_host_ptr = first_host_ptr;
                last_mapped_block = current_block;
            }

            page_table.entries.CommitRegion(base, end);
            while (base != end) {
                auto host_ptr = reinterpret_cast<u64>(system.DeviceMemory().GetPointer<u8>(target)) - (base << YUZU_PAGEBITS);;
                auto& entry = page_table.entries.GetUnchecked(base);

                entry.Store(false, type, current_block, host_ptr);
                ASSERT_MSG(page_table.entries[base].Pointer(),
                           "memory mapping base yield a nullptr within the table");

                base += 1;
                target += YUZU_PAGESIZE;
            }
        }
    }

    template<typename F, typename G>
    [[nodiscard]] u8* GetPointerImpl(u64 vaddr, F&& on_unmapped, G&& on_rasterizer) const {
        // AARCH64 masks the upper 16 bit of all memory accesses
        vaddr &= 0xffffffffffffULL;
        if (AddressSpaceContains(*current_page_table, vaddr, 1)) [[likely]] {
            // Avoid adding any extra logic to this fast-path block
            const auto raw = current_page_table->entries[vaddr >> YUZU_PAGEBITS].Raw();
            if (auto pointer = Common::PageTable::PageEntryData::ExtractPointer(raw); pointer) [[likely]] {
                return reinterpret_cast<u8*>(pointer + vaddr);
            } else {
                switch (static_cast<Common::PageType>(raw.type)) {
                case Common::PageType::Memory:
                    ASSERT_MSG(false, "Mapped memory page without a pointer @ {:#016x}", vaddr);
                    return nullptr;
                case Common::PageType::DebugMemory:
                    return GetPointerFromDebugMemory(vaddr);
                case Common::PageType::RasterizerCachedMemory: {
                    u8* const host_ptr{GetPointerFromRasterizerCachedMemory(vaddr)};
                    on_rasterizer();
                    return host_ptr;
                }
                case Common::PageType::Unmapped: [[unlikely]] {
                    on_unmapped();
                    return nullptr;
                }
                default:
                    UNREACHABLE();
                }
                return nullptr;
            }
        } else {
            on_unmapped();
            return nullptr;
        }
    }

    [[nodiscard]] u8* GetPointer(const Common::ProcessAddress vaddr) const {
        return GetPointerImpl(
            GetInteger(vaddr),
            [vaddr]() {
                LOG_ERROR(HW_Memory, "Unmapped GetPointer @ {:#016x}", GetInteger(vaddr));
            },
            []() {});
    }

    [[nodiscard]] u8* GetPointerSilent(const Common::ProcessAddress vaddr) const {
        return GetPointerImpl(
            GetInteger(vaddr), []() {}, []() {});
    }

    [[nodiscard]] u8* GetContiguousSpan(VAddr addr, std::size_t size) const noexcept {
        u8* const base = GetPointerSilent(addr);
        if (base == nullptr) {
            return nullptr;
        }

        const VAddr end = addr + size;
        const VAddr first_page = ((addr >> YUZU_PAGEBITS) + 1) << YUZU_PAGEBITS;
        const std::uintptr_t base_ptr = reinterpret_cast<std::uintptr_t>(base);
        for (VAddr page = first_page; page < end; page += YUZU_PAGESIZE) {
            const std::uintptr_t expected = base_ptr + (page - addr);
            if (reinterpret_cast<std::uintptr_t>(GetPointerSilent(page)) != expected) {
                return nullptr;
            }
        }
        return base;
    }

    /// @brief Reads a particular data type out of memory at the given virtual address.
    /// @param vaddr The virtual address to read the data type from.
    /// @tparam T The data type to read out of memory.
    /// @returns The instance of T read from the specified virtual address.
    template <typename T>
    inline T Read(Common::ProcessAddress vaddr) noexcept requires(std::is_trivially_copyable_v<T>) {
        const u64 addr = GetInteger(vaddr);
        if (auto const ptr = GetPointerImpl(addr, [addr]() {
            // A guest spinning on a bad pointer reads unmapped memory tens of thousands of times
            // a second: keep the first ones and then a sample.
            static std::atomic<u64> unmapped_reads{};
            if (const u64 count = unmapped_reads.fetch_add(1); count < 1000 || count % 100000 == 0) {
                LOG_ERROR(HW_Memory, "Unmapped Read{} @ {:#016x} (#{})", sizeof(T) * 8, addr, count);
            }
            ++UnmappedReadFlag();
        }, [&]() {
            HandleRasterizerDownload(addr, sizeof(T));
        }); ptr) [[likely]] {
            // It may be tempting to rewrite this particular section to use "reinterpret_cast";
            // afterall, it's trivially copyable so surely it can be copied ov- Alignment.
            // Remember, alignment. memcpy() will deal with all the alignment extremely fast.
            T result{};
            std::memcpy(&result, ptr, sizeof(T));
            return result;
        }
        return T{};
    }

    /// @brief Writes a particular data type to memory at the given virtual address.
    /// @param vaddr The virtual address to write the data type to.
    /// @tparam T The data type to write to memory.
    template <typename T>
    inline void Write(Common::ProcessAddress vaddr, const T data) noexcept requires(std::is_trivially_copyable_v<T>) {
        const u64 addr = GetInteger(vaddr);
        if (auto const ptr = GetPointerImpl(addr, [addr, data]() {
            LOG_ERROR(HW_Memory, "Unmapped Write{} @ {:#016x} = {:#016x}", sizeof(T) * 8, addr, u64(data));
        }, [&]() { HandleRasterizerWrite(addr, sizeof(T)); }); ptr) [[likely]]
            std::memcpy(ptr, &data, sizeof(T));
    }

    template <typename T>
    bool WriteExclusive(Common::ProcessAddress vaddr, const T data, const T expected) {
        u8* const ptr = GetPointerImpl(
            GetInteger(vaddr),
            [vaddr, data]() {
                LOG_ERROR(HW_Memory, "Unmapped WriteExclusive{} @ {:#016x} = {:#016x}",
                          sizeof(T) * 8, GetInteger(vaddr), static_cast<u64>(data));
            },
            [&]() { HandleRasterizerWrite(GetInteger(vaddr), sizeof(T)); });
        if (ptr) {
            return Common::AtomicCompareAndSwap(reinterpret_cast<T*>(ptr), data, expected);
        }
        return true;
    }

    bool WriteExclusive128(Common::ProcessAddress vaddr, const u128 data, const u128 expected) {
        u8* const ptr = GetPointerImpl(
            GetInteger(vaddr),
            [vaddr, data]() {
                LOG_ERROR(HW_Memory, "Unmapped WriteExclusive128 @ {:#016x} = {:#016x}{:016X}",
                          GetInteger(vaddr), static_cast<u64>(data[1]), static_cast<u64>(data[0]));
            },
            [&]() { HandleRasterizerWrite(GetInteger(vaddr), sizeof(u128)); });
        if (ptr) {
            return Common::AtomicCompareAndSwap(reinterpret_cast<u64*>(ptr), data, expected);
        }
        return true;
    }

    void HandleRasterizerDownload(VAddr v_address, size_t size) {
        if (GetReactiveFlushMode() != ReactiveFlushMode::Blanket) [[unlikely]] {
            if (const ReactiveFlushMode mode = ActiveReactiveMode();
                mode != ReactiveFlushMode::Blanket) {
                HandleReactiveDownload(mode, v_address, size);
                return;
            }
        }
        HandleCachedPageRead(v_address, size, true);
    }

    /// A slow-path read of a cached page asks the GPU to flush what it wrote there. `disarm`
    /// drops the page's legacy read trap first (Blanket mode, and reactive-off read traps).
    void HandleCachedPageRead(VAddr v_address, size_t size, bool disarm) {
        const auto* p = GetPointerImpl(
            v_address, []() {}, []() {});
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        const size_t core = system.GetCurrentHostThreadID();
        if (core >= Core::Hardware::NUM_CPU_CORES) {
            // A service thread has no slot in the arrays below, and it does not use a slot of a
            // core. It keeps no area, so it asks on each read.
            if (static std::atomic_flag logged; !logged.test_and_set(std::memory_order_relaxed)) {
                LOG_INFO(HW_Memory, "Read of cached guest memory from a thread that is not a core");
            }
            Common::ScratchBuffer<u32> scratch;
            gpu_device_memory->ApplyOpOnPointer(p, scratch, [&](DAddr address) {
                if (disarm) {
                    DisarmReadTraps(v_address, size);
                }
                (void)system.GPU().OnCPURead(address, size);
            });
            return;
        }
        auto& current_area = rasterizer_read_areas[core];
        if (const u64 generation = read_trap_generation.load(std::memory_order_acquire);
            read_area_generations[core] != generation) {
            current_area = {};
            read_area_generations[core] = generation;
        }
        gpu_device_memory->ApplyOpOnPointer(p, scratch_buffers[core], [&](DAddr address) {
            const DAddr end_address = address + size;
            if (current_area.start_address <= address && end_address <= current_area.end_address)
                [[likely]] {
                return;
            }
            if (disarm) {
                DisarmReadTraps(v_address, size);
            }
            current_area = system.GPU().OnCPURead(address, size);
        });
    }

    /// EDEN_NX2_REACTIVE_MODE=precise or verify. The caller took the read through the slow path
    /// of a cached page; one access never crosses a page (WalkBlock calls per page).
    ///
    /// Precise: an unarmed page has no GPU data owed to it (the legacy read-area check covers
    /// slow reads like HLE ReadBlock). An armed page is flushed whole, so a read of clean data
    /// cannot release a trap that protects other dirty data of the page, and is released after
    /// the flush only if no GPU write armed it again meanwhile.
    ///
    /// Verify: the blanket request is made as always, with no read area shortcut, and compared
    /// with the shadow set; an armed page is then consumed the way Precise would.
    void HandleReactiveDownload(ReactiveFlushMode mode, VAddr v_address, size_t size) {
        const u64 page = v_address >> YUZU_PAGEBITS;
        bool armed = false;
        u64 version = 0;
        if (armed_read_trap_count.load(std::memory_order_relaxed) != 0) {
            std::scoped_lock lk{read_trap_mutex};
            if (const std::optional<u64> found = armed_read_traps.Version(page)) {
                armed = true;
                version = *found;
            }
        }
        if (mode == ReactiveFlushMode::Precise) {
            if (!armed) {
                HandleCachedPageRead(v_address, size, false);
                return;
            }
            FlushCachedPage(v_address);
            ReleaseReadTrap(page, version, true);
            return;
        }

        const u8* const p = GetPointerImpl(
            v_address, []() {}, []() {});
        if (p == nullptr) [[unlikely]] {
            return;
        }
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        {
            const VerifyAccessScope audit{armed, v_address, this, &Impl::ProbeVerifyPage};
            Common::ScratchBuffer<u32> scratch;
            gpu_device_memory->ApplyOpOnPointer(p, scratch, [&](DAddr address) {
                (void)system.GPU().OnCPURead(address, size);
            });
        }
        if (armed) {
            FlushCachedPage(v_address);
            ReleaseReadTrap(page, version, false);
        }
    }

    /// Flushes everything the GPU owes the whole page of v_address, in each of its device
    /// aliases.
    void FlushCachedPage(VAddr v_address) {
        const u8* const p = GetPointerImpl(
            v_address, []() {}, []() {});
        if (p == nullptr) [[unlikely]] {
            return;
        }
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        const u8* const page_start = p - (v_address & YUZU_PAGEMASK);
        Common::ScratchBuffer<u32> scratch;
        const PageConsumeScope consume;
        gpu_device_memory->ApplyOpOnPointer(page_start, scratch, [&](DAddr address) {
            (void)system.GPU().OnCPURead(address, YUZU_PAGESIZE);
        });
    }

    /// Ends the trap of a page that was flushed for a reader, unless a GPU write armed it again
    /// since the reader saw `version`. `restore_read` gives the page its read permission back
    /// (Verify's shadow set never took it).
    void ReleaseReadTrap(u64 page, u64 version, bool restore_read) {
        std::scoped_lock lk{read_trap_mutex};
        if (!armed_read_traps.Release(page, version)) {
            return;
        }
        NoteVerifyEnd(page, VerifyPageEnd::Read);
        if (restore_read) {
            host_buffer->Protect(page << YUZU_PAGEBITS, YUZU_PAGESIZE,
                                 Common::MemoryPermission::Read);
        }
        SyncArmedReadTrapCount();
        VideoCommon::Nx2Spikes::Note(VideoCommon::Nx2Spikes::Event::ReadTrapHit);
    }

    void HandleRasterizerWrite(VAddr v_address, size_t size) {
        VideoCommon::Nx2Spikes::Note(VideoCommon::Nx2Spikes::Event::CpuWriteSlowPath, {}, size);
        const auto* p = GetPointerImpl(
            v_address, []() {}, []() {});
        constexpr size_t sys_core = Core::Hardware::NUM_CPU_CORES - 1;
        const size_t core = (std::min)(system.GetCurrentHostThreadID(),
                                     sys_core); // any other calls threads go to syscore.
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        // Guard on sys_core;
        if (core == sys_core) [[unlikely]] {
            sys_core_guard.lock();
        }
        SCOPE_EXIT {
            if (core == sys_core) [[unlikely]] {
                sys_core_guard.unlock();
            }
        };
        gpu_device_memory->ApplyOpOnPointer(p, scratch_buffers[core], [&](DAddr address) {
            VideoCore::Replay::NoteGuestWrite(address, size);
            auto& current_area = rasterizer_write_areas[core];
            PAddr subaddress = address >> YUZU_PAGEBITS;
            // Performance note:
            // It may not be a good idea to assume accesses are within the same subaddress (i.e same page)
            // It is often the case the games like to access wildly different addresses. Hence why I propose
            // we should let the compiler just do it's thing...
            if (current_area.last_address != subaddress) {
                // Short circuit the need to check for address/size
                auto const do_collection = (address != 0 && size != 0)
                    && system.GPU().OnCPUWrite(address, size);
                if (do_collection) {
                    current_area.last_address = subaddress;
                } else {
                    return;
                }
            }
            VideoCommon::Nx2Hazard::NoteGuestWrite(address, size);
            gpu_dirty_managers[core].Collect(address, size);
        });
    }

    struct GPUDirtyState {
        PAddr last_address;
    };

    void InvalidateGPUMemory(u8* p, size_t size) {
        constexpr size_t sys_core = Core::Hardware::NUM_CPU_CORES - 1;
        const size_t core = (std::min)(system.GetCurrentHostThreadID(),
                                     sys_core); // any other calls threads go to syscore.
        if (!gpu_device_memory) [[unlikely]] {
            gpu_device_memory = &system.Host1x().MemoryManager();
        }
        // Guard on sys_core;
        if (core == sys_core) [[unlikely]] {
            sys_core_guard.lock();
        }
        SCOPE_EXIT {
            if (core == sys_core) [[unlikely]] {
                sys_core_guard.unlock();
            }
        };
        auto& gpu = system.GPU();
        gpu_device_memory->ApplyOpOnPointer(
            p, scratch_buffers[core], [&](DAddr address) { gpu.InvalidateRegion(address, size, true); });
    }

    Core::System& system;
    Tegra::MaxwellDeviceMemoryManager* gpu_device_memory{};
    Common::PageTable* current_page_table = nullptr;

    std::array<VideoCore::RasterizerDownloadArea, Core::Hardware::NUM_CPU_CORES> rasterizer_read_areas{};
    std::array<u64, Core::Hardware::NUM_CPU_CORES> read_area_generations{};
    std::atomic<u64> read_trap_generation{};
    ArmedReadTraps armed_read_traps;
    /// Verify only: what happened to each page of the shadow set (guarded by read_trap_mutex).
    std::unordered_map<u64, VerifyPageHistory> verify_history;
    std::atomic<size_t> armed_read_trap_count{};
    std::mutex read_trap_mutex;
    std::array<GPUDirtyState, Core::Hardware::NUM_CPU_CORES> rasterizer_write_areas{};
    std::array<Common::ScratchBuffer<u32>, Core::Hardware::NUM_CPU_CORES> scratch_buffers{};
    std::span<Core::GPUDirtyMemoryManager> gpu_dirty_managers;
    std::mutex sys_core_guard;
#ifdef __ANDROID__
    std::optional<Common::HeapTracker> heap_tracker;
    Common::HeapTracker* host_buffer{};
#else
    Common::HostMemory* host_buffer{};
#endif
    std::atomic<u64> block_count = 0;
    const Common::PageTable* last_mapped_table{};
    u64 last_mapped_end{};
    u64 last_mapped_host_ptr{};
    u16 last_mapped_block{Common::PageTable::PageEntryData::INVALID_BLOCK_ID};
};

Memory::Memory(Core::System& system_) : system{system_} {
    Reset();
}

Memory::~Memory() = default;

void Memory::Reset() {
    impl = std::make_unique<Impl>(system);
}

void Memory::SetCurrentPageTable(Kernel::KProcess& process) {
    impl->SetCurrentPageTable(process);
}

void Memory::SetCurrentPageTable(Common::PageTable& page_table) {
    impl->SetCurrentPageTable(page_table);
}

void Memory::MapMemoryRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                             Common::PhysicalAddress target, Common::MemoryPermission perms,
                             bool separate_heap) {
    impl->MapMemoryRegion(page_table, base, size, target, perms, separate_heap);
}

void Memory::UnmapRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                         bool separate_heap) {
    impl->UnmapRegion(page_table, base, size, separate_heap);
}

void Memory::ProtectRegion(Common::PageTable& page_table, Common::ProcessAddress vaddr, u64 size,
                           Common::MemoryPermission perms) {
    impl->ProtectRegion(page_table, GetInteger(vaddr), size, perms);
}

bool Memory::IsValidVirtualAddress(const Common::ProcessAddress vaddr) const {
    const auto& page_table = *impl->current_page_table;
    const size_t page = vaddr >> YUZU_PAGEBITS;
    if (page >= page_table.entries.size()) {
        return false;
    }
    const auto [pointer, type, _] = page_table.entries[page].PointerTypeBlock();
    return pointer != 0 || type == Common::PageType::RasterizerCachedMemory ||
           type == Common::PageType::DebugMemory;
}

bool Memory::IsValidVirtualAddressRange(Common::ProcessAddress base, u64 size) const {
    Common::ProcessAddress end = base + size;
    Common::ProcessAddress page = Common::AlignDown(GetInteger(base), YUZU_PAGESIZE);

    for (; page < end; page += YUZU_PAGESIZE) {
        if (!IsValidVirtualAddress(page)) {
            return false;
        }
    }

    return true;
}

u8* Memory::GetPointer(Common::ProcessAddress vaddr) {
    return impl->GetPointer(vaddr);
}

u8* Memory::GetPointerSilent(Common::ProcessAddress vaddr) {
    return impl->GetPointerSilent(vaddr);
}

const u8* Memory::GetPointer(Common::ProcessAddress vaddr) const {
    return impl->GetPointer(vaddr);
}

u8 Memory::Read8(const Common::ProcessAddress addr) {
    return impl->Read8(addr);
}

u16 Memory::Read16(const Common::ProcessAddress addr) {
    return impl->Read16(addr);
}

u32 Memory::Read32(const Common::ProcessAddress addr) {
    return impl->Read32(addr);
}

u64 Memory::Read64(const Common::ProcessAddress addr) {
    return impl->Read64(addr);
}

void Memory::Write8(Common::ProcessAddress addr, u8 data) {
    impl->Write8(addr, data);
}

void Memory::Write16(Common::ProcessAddress addr, u16 data) {
    impl->Write16(addr, data);
}

void Memory::Write32(Common::ProcessAddress addr, u32 data) {
    impl->Write32(addr, data);
}

void Memory::Write64(Common::ProcessAddress addr, u64 data) {
    impl->Write64(addr, data);
}

bool Memory::WriteExclusive8(Common::ProcessAddress addr, u8 data, u8 expected) {
    return impl->WriteExclusive8(addr, data, expected);
}

bool Memory::WriteExclusive16(Common::ProcessAddress addr, u16 data, u16 expected) {
    return impl->WriteExclusive16(addr, data, expected);
}

bool Memory::WriteExclusive32(Common::ProcessAddress addr, u32 data, u32 expected) {
    return impl->WriteExclusive32(addr, data, expected);
}

bool Memory::WriteExclusive64(Common::ProcessAddress addr, u64 data, u64 expected) {
    return impl->WriteExclusive64(addr, data, expected);
}

bool Memory::WriteExclusive128(Common::ProcessAddress addr, u128 data, u128 expected) {
    return impl->WriteExclusive128(addr, data, expected);
}

std::string Memory::ReadCString(Common::ProcessAddress vaddr, std::size_t max_length) {
    return impl->ReadCString(vaddr, max_length);
}

bool Memory::ReadBlock(const Common::ProcessAddress src_addr, void* dest_buffer,
                       const std::size_t size) {
    return impl->ReadBlockImpl(src_addr, dest_buffer, size, false);
}

bool Memory::ReadBlockUnsafe(const Common::ProcessAddress src_addr, void* dest_buffer,
                             const std::size_t size) {
    return impl->ReadBlockImpl(src_addr, dest_buffer, size, true);
}

const u8* Memory::GetSpan(const VAddr src_addr, const std::size_t size) const {
    return impl->GetSpan(src_addr, size);
}

u8* Memory::GetSpan(const VAddr src_addr, const std::size_t size) {
    return impl->GetSpan(src_addr, size);
}

bool Memory::WriteBlock(const Common::ProcessAddress dest_addr, const void* src_buffer,
                        const std::size_t size) {
    return impl->WriteBlockImpl(dest_addr, src_buffer, size, false);
}

bool Memory::WriteBlockUnsafe(const Common::ProcessAddress dest_addr, const void* src_buffer,
                              const std::size_t size) {
    return impl->WriteBlockImpl(dest_addr, src_buffer, size, true);
}

bool Memory::CopyBlock(Common::ProcessAddress dest_addr, Common::ProcessAddress src_addr,
                       const std::size_t size) {
    return impl->CopyBlock(dest_addr, src_addr, size);
}

bool Memory::ZeroBlock(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->ZeroBlock(dest_addr, size);
}

void Memory::SetGPUDirtyManagers(std::span<Core::GPUDirtyMemoryManager> managers) {
    impl->gpu_dirty_managers = managers;
}

Result Memory::InvalidateDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->InvalidateDataCache(dest_addr, size);
}

Result Memory::StoreDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->StoreDataCache(dest_addr, size);
}

Result Memory::FlushDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->FlushDataCache(dest_addr, size);
}

void Memory::RasterizerMarkRegionCached(Common::ProcessAddress vaddr, u64 size, bool cached) {
    impl->RasterizerMarkRegionCached(GetInteger(vaddr), size, cached);
}

bool Memory::RasterizerArmReadTraps(Common::ProcessAddress vaddr, u64 size) {
    return impl->RasterizerArmReadTraps(GetInteger(vaddr), size);
}

void Memory::MarkRegionDebug(Common::ProcessAddress vaddr, u64 size, bool debug) {
    impl->MarkRegionDebug(GetInteger(vaddr), size, debug);
}

bool Memory::InvalidateNCE(Common::ProcessAddress vaddr, size_t size) {
    [[maybe_unused]] bool mapped = true;
    [[maybe_unused]] bool rasterizer = false;

    u8* const ptr = impl->GetPointerImpl(
        GetInteger(vaddr),
        [&] {
            LOG_ERROR(HW_Memory, "Unmapped InvalidateNCE for {} bytes @ {:#x}", size,
                      GetInteger(vaddr));
            mapped = false;
        },
        [&] { rasterizer = true; });
    if (rasterizer) {
        impl->InvalidateGPUMemory(ptr, size);
    }

#ifdef __ANDROID__
    if (!rasterizer && mapped) {
        impl->host_buffer->DeferredMapSeparateHeap(GetInteger(vaddr));
    }
#endif

    return mapped && ptr != nullptr;
}

bool Memory::InvalidateSeparateHeap(void* fault_address) {
#ifdef __ANDROID__
    return impl->host_buffer->DeferredMapSeparateHeap(static_cast<u8*>(fault_address));
#else
    return false;
#endif
}

} // namespace Core::Memory
