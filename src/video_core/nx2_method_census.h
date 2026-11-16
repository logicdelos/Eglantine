// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"

// Switch 2 census of what the tree does not model, reported once at shutdown (Report):
//
//   Nx2 method census: writes to the Ampere_B (0xC797) / Ampere compute B (0xC7C0) class methods
//   NVIDIA's clc797.h / clc7c0.h add and the engines only store. Names and byte offsets are the
//   header's; the tables hold dword method indices (offset / 4). Pushbuffer writes (host) and the
//   writes a macro emits are counted apart. No per-write log, one counter bump per hit.
//
//   Nx2 SEM_EXECUTE census: the host class SEM_EXECUTE (method 0x1B, clc56f.h) by operation and
//   flag combination, and whether Puller::ProcessSemaphoreExecute implements it.
//
// EDEN_NX2_SEM_EXECUTE=legacy: ProcessSemaphoreExecute keeps its old release (a 4-byte payload
// whatever PAYLOAD_SIZE_64BIT and RELEASE_TIMESTAMP say).
//
//   Nx2 class census: how often the titles use the audited class features (clc76f.h, clc7b5.h,
//   clc7c0.h, clc7c0qmd.h) the tree does not implement or only just implements, listed as
//   "<name> <count>, ..." (non-zero entries only, or "none"). Counters are plain increments on
//   the GPU thread that runs the engine; the SEM_EXECUTE and REPORT_SEMAPHORE_EXECUTE entries are
//   derived from the tables above when reporting.
//
// EDEN_NX2_CLASS_FIXES=legacy: every class fix below (Fix) is off, the old behaviour.
// EDEN_NX2_CLASS_FIXES_OFF=h1,h3,...: only those fixes are off (the Fix enumerators, lower case).
// EDEN_NX2_CLASS_FIXES_ON=h4,...: those fixes are on; H4 is the only one off by default.
namespace VideoCommon::Nx2Census {

enum class Engine : u32 { Maxwell3D = 0, Compute = 1 };
enum class Source : u32 { Host = 0, Macro = 1 };

inline constexpr u32 In3D = 1;
inline constexpr u32 InCompute = 2;

struct MethodName {
    u32 index; // dword index; the documented byte offset is index * 4
    const char* name;
    u32 engines;
};

inline constexpr std::array<MethodName, 21> kMethods{{
    {0x56, "SET_REPORT_SEMAPHORE_PAYLOAD_LOWER", In3D | InCompute},
    {0x57, "SET_REPORT_SEMAPHORE_PAYLOAD_UPPER", In3D | InCompute},
    {0x58, "SET_REPORT_SEMAPHORE_ADDRESS_LOWER", In3D | InCompute},
    {0x59, "SET_REPORT_SEMAPHORE_ADDRESS_UPPER", In3D | InCompute},
    {0x5A, "REPORT_SEMAPHORE_EXECUTE", In3D | InCompute},
    {0x7B, "SET_MME_SWITCH_STATE", In3D | InCompute},
    {0xB0, "SEND_SIGNALING_PCAS2_B", InCompute},
    {0xB4, "SET_ROOT_TABLE_PREFETCH", In3D},
    {0x140, "SET_Z_ROP_SLICE_MAP", In3D},
    {0x154, "SET_MME_MEM_ADDRESS_A", InCompute},
    {0x155, "SET_MME_MEM_ADDRESS_B", InCompute},
    {0x156, "SET_MME_DATA_RAM_ADDRESS", InCompute},
    {0x157, "MME_DMA_READ", InCompute},
    {0x158, "MME_DMA_READ_FIFOED", InCompute},
    {0x159, "MME_DMA_WRITE", InCompute},
    {0x15A, "MME_DMA_REDUCTION", InCompute},
    {0x15B, "MME_DMA_SYSMEMBAR", InCompute},
    {0x15C, "MME_DMA_SYNC", InCompute},
    {0x15D, "SET_MME_DATA_FIFO_CONFIG", InCompute},
    {0x1FF, "THROTTLE_SM", In3D | InCompute},
    {0x37B, "MME_DMA_WRITE_METHOD_BARRIER", In3D | InCompute},
}};

inline constexpr u32 SlotLimit = 0x37C; // one past the highest method index above

/// Per engine, method index -> counter number (0 = not counted). The pusher keeps the row of each
/// bound subchannel's engine, so a pushbuffer word costs a bounds compare and one byte load.
using SlotRow = std::array<u8, SlotLimit>;

constexpr std::array<SlotRow, 2> MakeSlots() {
    std::array<SlotRow, 2> slots{};
    for (size_t i = 0; i < kMethods.size(); ++i) {
        if ((kMethods[i].engines & In3D) != 0) {
            slots[0][kMethods[i].index] = static_cast<u8>(i + 1);
        }
        if ((kMethods[i].engines & InCompute) != 0) {
            slots[1][kMethods[i].index] = static_cast<u8>(kMethods.size() + i + 1);
        }
    }
    return slots;
}

inline constexpr std::array<SlotRow, 2> kSlots = MakeSlots();

/// The row for the engine a subchannel is bound to; nullptr when the engine has none.
inline const u8* Slots(Engine engine) {
    return kSlots[static_cast<size_t>(engine)].data();
}

struct Counter {
    std::atomic<u64> writes[2]{};
    std::atomic<u32> first{};
    std::atomic<u32> last{};
    std::atomic<bool> seen{};
};

inline std::array<Counter, 2 * kMethods.size() + 1> g_counters{};

[[gnu::noinline]] inline void Record(u32 counter_number, Source source, u32 first, u32 last,
                                     u64 count) {
    Counter& counter = g_counters[counter_number];
    counter.writes[static_cast<size_t>(source)].fetch_add(count, std::memory_order_relaxed);
    if (!counter.seen.load(std::memory_order_relaxed) &&
        !counter.seen.exchange(true, std::memory_order_relaxed)) {
        counter.first.store(first, std::memory_order_relaxed);
    }
    counter.last.store(last, std::memory_order_relaxed);
}

/// One method write against a slot row.
[[gnu::always_inline]] inline void NoteMethod(const u8* slots, u32 method, u32 value,
                                              Source source) {
    if (method < SlotLimit && slots[method] != 0) [[unlikely]] {
        Record(slots[method], source, value, value, 1);
    }
}

/// One method write of a macro (the row is constant at each call site).
[[gnu::always_inline]] inline void NoteMethod(Engine engine, u32 method, u32 value,
                                              Source source) {
    NoteMethod(Slots(engine), method, value, source);
}

[[gnu::noinline]] inline void NoteHostRun(const u8* slots, u32 method, const u32* values,
                                          u32 count) {
    for (u32 i = 0; i < count; ++i) {
        if (const u8 slot = slots[method + i]; slot != 0) {
            Record(slot, Source::Host, values[i], values[i], 1);
        }
    }
}

/// `count` pushbuffer words of one command: method, method + 1, ... when `incrementing`, else all
/// of them at `method`. `slots` is nullptr for an engine without a row.
[[gnu::always_inline]] inline void NoteHostWords(const u8* slots, u32 method, const u32* values,
                                                 u32 count, bool incrementing) {
    if (slots == nullptr || method >= SlotLimit || count == 0) {
        return;
    }
    if (!incrementing || count == 1) {
        if (const u8 slot = slots[method]; slot != 0) [[unlikely]] {
            Record(slot, Source::Host, values[0], values[count - 1], count);
        }
        return;
    }
    NoteHostRun(slots, method, values, count < SlotLimit - method ? count : SlotLimit - method);
}

/// EDEN_NX2_SEM_EXECUTE=legacy (read once).
inline bool SemExecuteLegacy() {
    static const bool legacy = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_EXECUTE");
        return value != nullptr && std::string_view{value} == "legacy";
    }();
    return legacy;
}

// SEM_EXECUTE (clc56f.h): operation 2:0, ACQUIRE_SWITCH_TSG 12, RELEASE_WFI 20, PAYLOAD_SIZE_64BIT
// 24, RELEASE_TIMESTAMP 25, REDUCTION 30:27 (operation REDUCTION only), REDUCTION_FORMAT 31
// (operation REDUCTION only; 1 is unsigned).
inline constexpr u32 SemSwitch = 1U << 0;
inline constexpr u32 SemWfi = 1U << 1;
inline constexpr u32 Sem64Bit = 1U << 2;
inline constexpr u32 SemTimestamp = 1U << 3;
inline constexpr u32 SemUnsigned = 1U << 4;

inline std::array<std::atomic<u32>, 8 * 32 * 16> g_sem_execute{};

constexpr u32 SemIndex(u32 argument) {
    const u32 operation = argument & 7;
    u32 flags = ((argument >> 12) & 1) * SemSwitch | ((argument >> 20) & 1) * SemWfi |
                ((argument >> 24) & 1) * Sem64Bit | ((argument >> 25) & 1) * SemTimestamp;
    u32 reduction = 0;
    if (operation == 6) {
        flags |= ((argument >> 31) & 1) * SemUnsigned;
        reduction = (argument >> 27) & 15;
    }
    return (operation << 9) | (flags << 4) | reduction;
}

inline void NoteSemExecute(u32 argument) {
    g_sem_execute[SemIndex(argument)].fetch_add(1, std::memory_order_relaxed);
}

inline std::string_view SemOperationName(u32 operation) {
    constexpr std::array<std::string_view, 8> names{"ACQUIRE",      "RELEASE", "ACQ_STRICT_GEQ",
                                                    "ACQ_CIRC_GEQ", "ACQ_AND", "ACQ_NOR",
                                                    "REDUCTION",    "RESERVED_7"};
    return names[operation];
}

/// What Puller::ProcessSemaphoreExecute does with the combination (keep in step with it).
inline std::string_view SemHandled(u32 operation, u32 flags) {
    if (operation == 1) {
        // The release itself; WFI and the TSG switch have nothing to do on this back. A 64-bit
        // payload is written as 4 bytes; a timestamp release writes it unless legacy.
        if ((flags & Sem64Bit) != 0) {
            return "partial (64-bit payload)";
        }
        return SemExecuteLegacy() && (flags & SemTimestamp) != 0 ? "partial (timestamp, legacy)"
                                                                  : "yes";
    }
    if (operation == 0 || (operation >= 2 && operation <= 5)) {
        // One check against the 32-bit payload, never suspending, for the first 256 acquires.
        return "partial";
    }
    return "no";
}

/// The class fixes of the audit (research3/class_audit.md); each one is a runtime switch.
enum class Fix : u32 {
    H1, ///< DMA LAUNCH_DMA with DATA_TRANSFER_TYPE NONE copies nothing
    H3, ///< host SEMAPHORED.OPERATION is 4:0 and REDUCTION (0x10) is implemented
    H4, ///< Switch 2 host MEM_OP_B is an address word, the operation is in MEM_OP_D 31:27
    C2, ///< compute QMD SAMPLER_INDEX decoded into linked_tsc
    A1, ///< DMA address upper words are 17 bits
    L1, ///< QMD local memory sizes are 24 bits
    D1, ///< DMA SEMAPHORE_TYPE 3 releases a word instead of asserting
    H5, ///< host methods 0x17-0x19 and unimplemented puller methods do not log an error per write
    C4, ///< SET_REPORT_SEMAPHORE_D REDUCTION_FORMAT 18:17 picks a signed reduction
};

inline u32 FixMask() {
    static const u32 mask = [] {
        constexpr std::array<std::string_view, 9> names{"h1", "h3", "h4", "c2", "a1",
                                                        "l1", "d1", "h5", "c4"};
        // H4 changes what the GPU caches see; it stays off until a lab A/B shows the same picture.
        u32 result = ((1U << names.size()) - 1) & ~(1U << static_cast<u32>(Fix::H4));
        const char* all = std::getenv("EDEN_NX2_CLASS_FIXES");
        const char* off = std::getenv("EDEN_NX2_CLASS_FIXES_OFF");
        const char* on = std::getenv("EDEN_NX2_CLASS_FIXES_ON");
        if (all != nullptr && std::string_view{all} == "legacy") {
            result = 0;
        }
        const auto apply = [&](const char* value, bool enable) {
            std::string_view list{value != nullptr ? value : ""};
            while (!list.empty()) {
                const size_t comma = list.find(',');
                const std::string_view item = list.substr(0, comma);
                list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
                for (size_t i = 0; i < names.size(); ++i) {
                    if (item == names[i]) {
                        result = enable ? result | (1U << i) : result & ~(1U << i);
                    }
                }
            }
        };
        apply(off, false);
        apply(on, true);
        if (all != nullptr || off != nullptr || on != nullptr) {
            LOG_INFO(HW_GPU,
                     "Nx2 class fixes: mask {:#x} (EDEN_NX2_CLASS_FIXES={}, _OFF={}, _ON={})",
                     result, all != nullptr ? all : "", off != nullptr ? off : "",
                     on != nullptr ? on : "");
        }
        return result;
    }();
    return mask;
}

inline bool FixOn(Fix fix) {
    return (FixMask() >> static_cast<u32>(fix)) & 1;
}

enum class Class : u32 {
    H1_NONE_LAUNCH,
    H2_SEM_EXECUTE_REDUCTION, // derived
    H2_SEM_EXECUTE_64BIT,     // derived
    H2_SEM_EXECUTE_ACQUIRE,   // derived
    H3_SEMAPHORED_REDUCTION,
    C1_PCAS_ACTION_6,
    C1_PCAS_ACTION_8,
    C1_PCAS_ACTION_9,
    C1_PCAS_ACTION_10,
    C1_PCAS_ACTION_11,
    C1_PCAS_ACTION_13,
    C1_SEND_PCAS_B,
    C3_QMD_RELEASE_PAYLOAD64B,
    C3_QMD_RELEASE_TWO_WORDS,
    C3_QMD_RELEASE_TRAP_TYPE,
    C4_REPORT_SEMAPHORE_EXECUTE, // derived
    C5_I2M_RELEASE_SEMAPHORE,
    D1_DMA_TWO_WORD_PAYLOAD,
    D1_DMA_SEMAPHORE_REDUCTION,
    D1_DMA_SEMAPHORE_TYPE_3,
    D2_DMA_REMAP_UNSUPPORTED,
    C6_QMD_V02,
    C6_DEPENDENT_QMD1,
    C6_IS_QUEUE,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Class::Count)> kClassNames{
    "H1_NONE_LAUNCH",
    "H2_SEM_EXECUTE_REDUCTION",
    "H2_SEM_EXECUTE_64BIT",
    "H2_SEM_EXECUTE_ACQUIRE",
    "H3_SEMAPHORED_REDUCTION",
    "C1_PCAS_ACTION_6",
    "C1_PCAS_ACTION_8",
    "C1_PCAS_ACTION_9",
    "C1_PCAS_ACTION_10",
    "C1_PCAS_ACTION_11",
    "C1_PCAS_ACTION_13",
    "C1_SEND_PCAS_B",
    "C3_QMD_RELEASE_PAYLOAD64B",
    "C3_QMD_RELEASE_TWO_WORDS",
    "C3_QMD_RELEASE_TRAP_TYPE",
    "C4_REPORT_SEMAPHORE_EXECUTE",
    "C5_I2M_RELEASE_SEMAPHORE",
    "D1_DMA_TWO_WORD_PAYLOAD",
    "D1_DMA_SEMAPHORE_REDUCTION",
    "D1_DMA_SEMAPHORE_TYPE_3",
    "D2_DMA_REMAP_UNSUPPORTED",
    "C6_QMD_V02",
    "C6_DEPENDENT_QMD1",
    "C6_IS_QUEUE",
};

inline std::array<std::atomic<u64>, static_cast<size_t>(Class::Count)> g_class{};
/// H4: host MEM_OP_D operations by their 31:27 value.
inline std::array<std::atomic<u64>, 32> g_mem_op{};

inline void Bump(std::atomic<u64>& counter) {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

[[gnu::always_inline]] inline void NoteClass(Class what) {
    Bump(g_class[static_cast<size_t>(what)]);
}

[[gnu::always_inline]] inline void NoteMemOp(u32 operation) {
    Bump(g_mem_op[operation & 31]);
}

inline std::string_view MemOpName(u32 operation) {
    switch (operation) {
    case 0x05: return "MEMBAR";
    case 0x09: return "MMU_TLB_INVALIDATE";
    case 0x0a: return "MMU_TLB_INVALIDATE_TARGETED";
    case 0x0b: return "MMU_OPERATION";
    case 0x0d: return "L2_PEERMEM_INVALIDATE";
    case 0x0e: return "L2_SYSMEM_INVALIDATE";
    case 0x0f: return "L2_CLEAN_COMPTAGS";
    case 0x10: return "L2_FLUSH_DIRTY";
    case 0x15: return "L2_WAIT_FOR_SYS_PENDING_READS";
    case 0x16: return "ACCESS_COUNTER_CLR";
    default: return {};
    }
}

/// The class census line (see the header comment).
inline void ReportClasses() {
    std::array<u64, static_cast<size_t>(Class::Count)> counts{};
    for (size_t i = 0; i < counts.size(); ++i) {
        counts[i] = g_class[i].load(std::memory_order_relaxed);
    }
    for (u32 index = 0; index < g_sem_execute.size(); ++index) {
        const u64 count = g_sem_execute[index].load(std::memory_order_relaxed);
        const u32 operation = index >> 9;
        const u32 flags = (index >> 4) & 31;
        if (operation == 6) counts[static_cast<size_t>(Class::H2_SEM_EXECUTE_REDUCTION)] += count;
        if ((flags & Sem64Bit) != 0) counts[static_cast<size_t>(Class::H2_SEM_EXECUTE_64BIT)] += count;
        if (operation == 0 || (operation >= 2 && operation <= 5)) {
            counts[static_cast<size_t>(Class::H2_SEM_EXECUTE_ACQUIRE)] += count;
        }
    }
    for (size_t i = 0; i < kMethods.size(); ++i) {
        if (kMethods[i].index == 0x5A) {
            const Counter& counter = g_counters[kMethods.size() + i + 1]; // the compute row
            counts[static_cast<size_t>(Class::C4_REPORT_SEMAPHORE_EXECUTE)] =
                counter.writes[0].load(std::memory_order_relaxed) +
                counter.writes[1].load(std::memory_order_relaxed);
        }
    }
    std::string text;
    const auto add = [&text](std::string_view name, u64 count) {
        text += text.empty() ? "" : ", ";
        text += name;
        text += ' ';
        text += std::to_string(count);
    };
    for (size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] != 0) add(kClassNames[i], counts[i]);
    }
    for (u32 operation = 0; operation < g_mem_op.size(); ++operation) {
        if (const u64 count = g_mem_op[operation].load(std::memory_order_relaxed); count != 0) {
            const std::string_view name = MemOpName(operation);
            add(name.empty() ? "MEMOP_" + std::to_string(operation) : "MEMOP_" + std::string{name},
                count);
        }
    }
    LOG_INFO(HW_GPU, "Nx2 class census: {}", text.empty() ? "none" : text);
}

inline void Report() {
    static std::atomic<bool> reported{};
    if (reported.exchange(true)) {
        return;
    }
    bool any = false;
    for (size_t engine = 0; engine < 2; ++engine) {
        for (size_t i = 0; i < kMethods.size(); ++i) {
            const Counter& counter = g_counters[engine * kMethods.size() + i + 1];
            const u64 host = counter.writes[0].load(std::memory_order_relaxed);
            const u64 macro = counter.writes[1].load(std::memory_order_relaxed);
            if (host + macro == 0) {
                continue;
            }
            any = true;
            LOG_INFO(HW_GPU,
                     "Nx2 method census: {:#x} {} writes={} first_value={:#x} last_value={:#x} "
                     "engine={} host={} macro={}",
                     kMethods[i].index * 4, kMethods[i].name, host + macro,
                     counter.first.load(std::memory_order_relaxed),
                     counter.last.load(std::memory_order_relaxed),
                     engine == 0 ? "3d" : "compute", host, macro);
        }
    }
    if (!any) {
        LOG_INFO(HW_GPU, "Nx2 method census: none");
    }
    any = false;
    for (u32 index = 0; index < g_sem_execute.size(); ++index) {
        const u32 count = g_sem_execute[index].load(std::memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        any = true;
        const u32 operation = index >> 9;
        const u32 flags = (index >> 4) & 31;
        std::string text;
        const auto add = [&text](std::string_view name) {
            text += text.empty() ? "" : "+";
            text += name;
        };
        if ((flags & SemSwitch) != 0) add("ACQUIRE_SWITCH_TSG");
        if ((flags & SemWfi) != 0) add("RELEASE_WFI");
        if ((flags & Sem64Bit) != 0) add("PAYLOAD_64BIT");
        if ((flags & SemTimestamp) != 0) add("RELEASE_TIMESTAMP");
        if (operation == 6) {
            constexpr std::array<std::string_view, 8> reductions{"IMIN", "IMAX", "IXOR", "IAND",
                                                                 "IOR",  "IADD", "INC",  "DEC"};
            const u32 reduction = index & 15;
            add(reduction < 8 ? reductions[reduction] : "RESERVED");
            add((flags & SemUnsigned) != 0 ? "UNSIGNED" : "SIGNED");
        }
        LOG_INFO(HW_GPU, "Nx2 SEM_EXECUTE census: op={} flags={} count={} handled={}",
                 SemOperationName(operation), text.empty() ? "none" : text, count,
                 SemHandled(operation, flags));
    }
    if (!any) {
        LOG_INFO(HW_GPU, "Nx2 SEM_EXECUTE census: none");
    }
    ReportClasses();
}

} // namespace VideoCommon::Nx2Census
