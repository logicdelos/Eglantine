// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/nx2_sampler.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/thread.h"

#include <windows.h>
// windows.h first: the other two use its types.
#include <dbghelp.h>
#include <tlhelp32.h>

namespace Nx2Sampler {
namespace {

constexpr size_t MaxDepth = 40;

struct Sample {
    long long qpc;
    unsigned thread;
    unsigned depth;
    std::array<DWORD64, MaxDepth> frames;
    unsigned epoch; // JIT cache epoch when taken
};

std::atomic<bool> recording_presents{false};
std::atomic<size_t> present_count{0};
std::array<long long, 1 << 17> presents{};

struct Target {
    HANDLE handle;
    std::string name;
};

double EnvSeconds(const char* name, double fallback) {
    const char* text = std::getenv(name);
    return text != nullptr ? std::strtod(text, nullptr) : fallback;
}

std::vector<std::string> SplitNames(const char* text) {
    std::vector<std::string> names;
    std::string current;
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == ',') {
            names.push_back(current);
            current.clear();
        } else {
            current += *c;
        }
    }
    if (!current.empty()) {
        names.push_back(current);
    }
    return names;
}

bool Matches(const std::string& thread_name, const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        if (!pattern.empty() && pattern.back() == '*') {
            if (thread_name.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0) {
                return true;
            }
        } else if (thread_name == pattern) {
            return true;
        }
    }
    return false;
}

std::vector<Target> FindThreads(const std::vector<std::string>& patterns) {
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleA("KernelBase.dll"), "GetThreadDescription"));
    std::vector<Target> targets;
    if (get_description == nullptr) {
        return targets;
    }
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return targets;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ThreadID);
        if (handle == nullptr) {
            continue;
        }
        PWSTR wide{};
        std::string name;
        if (SUCCEEDED(get_description(handle, &wide)) && wide != nullptr) {
            char buffer[256]{};
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, sizeof(buffer) - 1, nullptr, nullptr);
            name = buffer;
            LocalFree(wide);
        }
        if (Matches(name, patterns)) {
            targets.push_back({handle, name});
        } else {
            CloseHandle(handle);
        }
    }
    CloseHandle(snapshot);
    return targets;
}

// Walks a suspended thread's stack from its context. Nothing here may allocate: the thread
// could be paused holding the heap lock.
unsigned Walk(CONTEXT context, std::array<DWORD64, MaxDepth>& frames) {
    unsigned depth = 0;
    while (depth < MaxDepth && context.Rip != 0) {
        frames[depth++] = context.Rip;
        DWORD64 image_base{};
        const PRUNTIME_FUNCTION function =
            RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            // A leaf function (no unwind data): its return address is on top of the stack. JIT
            // code has none either; past the first frame, stop there.
            if (depth != 1) {
                break;
            }
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        PVOID handler_data{};
        DWORD64 establisher_frame{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher_frame, nullptr);
    }
    return depth;
}

/// "module+0xoffset" of a code address ("?+0xaddress" outside every module: JIT code).
std::string ModuleOffset(DWORD64 address) {
    HMODULE module{};
    char path[MAX_PATH] = "?";
    DWORD64 offset = address;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module)) {
        GetModuleFileNameA(module, path, sizeof(path));
        offset = address - reinterpret_cast<DWORD64>(module);
    }
    const char* base = std::strrchr(path, '\\');
    return fmt::format("{}+{:#x}", base != nullptr ? base + 1 : path, offset);
}

// dbghelp is one engine per process and not thread safe: a Symbolizer holds this for its lifetime.
std::mutex symbol_mutex;

struct Symbolizer {
    using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
    using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    using SymGetLineFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);

    std::scoped_lock<std::mutex> lock{symbol_mutex};

    Symbolizer() {
        const HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
        if (dbghelp == nullptr) {
            return;
        }
        const auto set_options =
            reinterpret_cast<SymSetOptionsFn>(GetProcAddress(dbghelp, "SymSetOptions"));
        const auto initialize =
            reinterpret_cast<SymInitializeFn>(GetProcAddress(dbghelp, "SymInitialize"));
        from_address = reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr"));
        line_from_address =
            reinterpret_cast<SymGetLineFn>(GetProcAddress(dbghelp, "SymGetLineFromAddr64"));
        if (set_options == nullptr || initialize == nullptr || from_address == nullptr) {
            from_address = nullptr;
            return;
        }
        set_options(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        // A process initialises dbghelp once; a later Symbolizer finds it initialised.
        if (!initialize(GetCurrentProcess(), nullptr, TRUE) &&
            GetLastError() != ERROR_INVALID_PARAMETER) {
            from_address = nullptr;
        }
    }

    const std::string& Name(DWORD64 address) {
        const auto [it, is_new] = cache.try_emplace(address);
        if (!is_new) {
            return it->second;
        }
        if (from_address != nullptr) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 1024]{};
            auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 1024;
            DWORD64 displacement{};
            if (from_address(GetCurrentProcess(), address, &displacement, symbol)) {
                it->second.assign(symbol->Name, std::min<size_t>(symbol->NameLen, 160));
                return it->second;
            }
        }
        HMODULE module{};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &module)) {
            char path[MAX_PATH] = "?";
            GetModuleFileNameA(module, path, sizeof(path));
            const char* base = std::strrchr(path, '\\');
            it->second = std::string{base != nullptr ? base + 1 : path} + "!?";
        } else {
            it->second = "(jit)";
        }
        return it->second;
    }

    /// One line of a stack listing: "module+0xoffset function (file:line)".
    std::string Where(DWORD64 address) {
        std::string text = fmt::format("{} {}", ModuleOffset(address), Name(address));
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD column{};
        if (from_address != nullptr && line_from_address != nullptr &&
            line_from_address(GetCurrentProcess(), address, &column, &line)) {
            text += fmt::format(" ({}:{})", line.FileName, line.LineNumber);
        }
        return text;
    }

    SymFromAddrFn from_address{};
    SymGetLineFn line_from_address{};
    std::unordered_map<DWORD64, std::string> cache;
};

// Guest attribution of JIT samples. The JIT appends one entry per code cache, block and cache
// clear to a log. The sampler never takes the mutex the JIT threads share: it reads the entries
// below the count they last published, so it cannot block a JIT thread or wait for one it has
// suspended.
struct JitEntry {
    u64 begin;
    u64 guest_pc; // ClearMark: cache cleared, RegionMark: a whole code cache
    u32 size;
    u32 epoch; // clears seen so far; a clear's own entry already carries the new value
};
constexpr u64 ClearMark = ~u64{0};
constexpr u64 RegionMark = ~u64{0} - 1;
constexpr u64 NoBlock = ~u64{0};
constexpr size_t ChunkEntries = size_t{1} << 16;
constexpr size_t MaxChunks = 64;

struct JitLog {
    std::mutex mutex; // writers only
    std::array<std::unique_ptr<JitEntry[]>, MaxChunks> chunks;
    std::atomic<size_t> count{};
    std::atomic<u64> dropped{};
    std::atomic<u32> epoch{};
    std::atomic<u32> stale_epoch{~u32{0}}; // from here on a lost clear may hide reused addresses
};
JitLog* const jit_log = new JitLog;

std::mutex modules_mutex;
std::vector<GuestModule> guest_modules;

void AppendJit(const void* begin, size_t size, u64 guest_pc) {
    JitLog& log = *jit_log;
    std::scoped_lock lock{log.mutex};
    const size_t n = log.count.load(std::memory_order_relaxed);
    if (n == MaxChunks * ChunkEntries) {
        log.dropped.fetch_add(1, std::memory_order_relaxed);
        if (guest_pc == ClearMark) {
            // The clear still starts a new epoch, but the replay cannot see it: samples from
            // here on are refused.
            const u32 epoch = log.epoch.fetch_add(1, std::memory_order_relaxed) + 1;
            if (epoch < log.stale_epoch.load(std::memory_order_relaxed)) {
                log.stale_epoch.store(epoch, std::memory_order_relaxed);
            }
        }
        return;
    }
    std::unique_ptr<JitEntry[]>& chunk = log.chunks[n / ChunkEntries];
    if (!chunk) {
        chunk.reset(new JitEntry[ChunkEntries]);
    }
    const u32 epoch = guest_pc == ClearMark ? log.epoch.fetch_add(1, std::memory_order_relaxed) + 1
                                            : log.epoch.load(std::memory_order_relaxed);
    chunk[n % ChunkEntries] = {reinterpret_cast<u64>(begin), guest_pc, static_cast<u32>(size),
                               epoch};
    log.count.store(n + 1, std::memory_order_release);
}

// The first stack frame of a sample inside a JIT code cache, and the guest block it ran.
struct JitFrame {
    int frame = -1; // -1: no frame in a code cache
    u64 pc = NoBlock; // NoBlock: dispatcher, thunk or code the log does not know
};

struct JitStats {
    size_t entries = 0;
    u64 dropped = 0;
    size_t in_jit = 0;
    size_t mapped = 0;
    size_t refused = 0; // in JIT code after the log filled and lost a clear
};

std::vector<JitFrame> ResolveJit(const std::vector<Sample>& samples, JitStats& stats) {
    std::vector<JitFrame> result(samples.size());
    JitLog& log = *jit_log;
    const size_t count = log.count.load(std::memory_order_acquire);
    stats.entries = count;
    stats.dropped = log.dropped.load(std::memory_order_relaxed);
    const u32 stale = log.stale_epoch.load(std::memory_order_relaxed);
    const auto entry = [&](size_t i) -> const JitEntry& {
        return log.chunks[i / ChunkEntries][i % ChunkEntries];
    };
    std::vector<std::pair<u64, u64>> regions; // [begin, end)
    for (size_t i = 0; i < count; ++i) {
        if (entry(i).guest_pc == RegionMark) {
            regions.emplace_back(entry(i).begin, entry(i).begin + entry(i).size);
        }
    }
    if (regions.empty()) {
        return result;
    }
    std::sort(regions.begin(), regions.end());
    const auto in_region = [&](u64 address) {
        const auto it = std::upper_bound(regions.begin(), regions.end(),
                                         std::make_pair(address, ~u64{0}));
        return it != regions.begin() && address < std::prev(it)->second;
    };

    struct Wanted {
        u32 epoch;
        u64 host;
        size_t sample;
    };
    std::vector<Wanted> wanted;
    for (size_t s = 0; s < samples.size(); ++s) {
        const Sample& sample = samples[s];
        for (unsigned i = 0; i < sample.depth; ++i) {
            if (in_region(sample.frames[i])) {
                result[s].frame = static_cast<int>(i);
                wanted.push_back({sample.epoch, sample.frames[i], s});
                break;
            }
        }
    }
    stats.in_jit = wanted.size();
    std::sort(wanted.begin(), wanted.end(),
              [](const Wanted& a, const Wanted& b) { return a.epoch < b.epoch; });
    std::vector<u64> hosts;
    for (const Wanted& w : wanted) {
        hosts.push_back(w.host);
    }
    std::sort(hosts.begin(), hosts.end());
    hosts.erase(std::unique(hosts.begin(), hosts.end()), hosts.end());

    // Replay the log up to each sample epoch. Only blocks holding a sampled address are kept.
    std::map<u64, std::pair<u64, u64>> live; // begin -> {end, guest pc}
    size_t next = 0;
    for (size_t w = 0; w < wanted.size();) {
        const u32 epoch = wanted[w].epoch;
        for (; next < count && entry(next).epoch <= epoch; ++next) {
            const JitEntry& e = entry(next);
            const u64 end = e.begin + e.size;
            if (e.guest_pc == RegionMark) {
                continue;
            }
            if (e.guest_pc == ClearMark) {
                live.erase(live.lower_bound(e.begin), live.lower_bound(end));
                continue;
            }
            const auto host = std::lower_bound(hosts.begin(), hosts.end(), e.begin);
            if (host != hosts.end() && *host < end) {
                live[e.begin] = {end, e.guest_pc};
            }
        }
        for (; w < wanted.size() && wanted[w].epoch == epoch; ++w) {
            if (wanted[w].epoch >= stale) {
                ++stats.refused;
                continue;
            }
            auto it = live.upper_bound(wanted[w].host);
            if (it != live.begin() && wanted[w].host < std::prev(it)->second.first) {
                result[wanted[w].sample].pc = std::prev(it)->second.second;
                ++stats.mapped;
            }
        }
    }
    return result;
}

// Guest pc -> module and offset.
struct GuestMap {
    GuestMap() = default;
    explicit GuestMap(std::vector<GuestModule> list) {
        for (GuestModule& module : list) {
            const u64 base = module.base;
            modules.emplace(base, std::move(module));
        }
    }

    std::pair<std::string, u64> Locate(u64 pc) const {
        auto it = modules.upper_bound(pc);
        if (it != modules.begin() && pc < std::prev(it)->second.end) {
            --it;
            return {it->second.name, pc - it->second.base};
        }
        return {"?", pc};
    }

    std::map<u64, GuestModule> modules;
};

struct BlockCounts {
    unsigned self = 0; // sampled inside the block
    unsigned host = 0; // sampled in a host call the block made
};

struct GuestThread {
    std::map<u64, BlockCounts> blocks;
    unsigned runtime = 0; // in the dispatcher or a thunk
    std::vector<std::pair<u64, unsigned>> by_self; // top blocks: guest pc, samples
    std::vector<std::pair<u64, unsigned>> by_host;
};

struct GuestTables {
    std::vector<GuestThread> threads;
    GuestMap map;
    bool any = false;
};

std::vector<std::pair<u64, unsigned>> TopBlocks(const std::map<u64, BlockCounts>& blocks,
                                                unsigned BlockCounts::*member, size_t limit) {
    std::vector<std::pair<u64, unsigned>> sorted;
    for (const auto& [pc, counts] : blocks) {
        if (counts.*member != 0) {
            sorted.emplace_back(pc, counts.*member);
        }
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    sorted.resize((std::min)(limit, sorted.size()));
    return sorted;
}

GuestTables BuildGuest(const std::vector<Sample>& samples, const std::vector<JitFrame>& jit,
                       size_t thread_count) {
    GuestTables tables;
    tables.threads.resize(thread_count);
    for (size_t s = 0; s < samples.size(); ++s) {
        const JitFrame& info = jit[s];
        if (info.frame < 0) {
            continue;
        }
        GuestThread& thread = tables.threads[samples[s].thread];
        if (info.pc == NoBlock) {
            thread.runtime += info.frame == 0 ? 1 : 0;
            tables.any = true;
            continue;
        }
        BlockCounts& counts = thread.blocks[info.pc];
        ++(info.frame == 0 ? counts.self : counts.host);
        tables.any = true;
    }
    if (!tables.any) {
        return tables;
    }
    std::vector<GuestModule> modules;
    {
        std::scoped_lock lock{modules_mutex};
        modules = guest_modules;
    }
    tables.map = GuestMap{std::move(modules)};
    for (GuestThread& thread : tables.threads) {
        thread.by_self = TopBlocks(thread.blocks, &BlockCounts::self, 40);
        thread.by_host = TopBlocks(thread.blocks, &BlockCounts::host, 20);
    }
    return tables;
}

void PrintGuest(std::string& text, const GuestTables& tables, const GuestThread& guest,
                unsigned total) {
    const auto share = [&](unsigned samples) {
        return 100.0 * samples / (std::max)(total, 1U);
    };
    const auto where = [&](u64 pc) {
        const auto [module, offset] = tables.map.Locate(pc);
        return fmt::format("{}+{:#x}", module, offset);
    };
    text += "-- guest code (JIT blocks the thread ran, module+offset of the block start)\n";
    for (const auto& [pc, count] : guest.by_self) {
        text += fmt::format("guest code: {:5.1f}% {} ({} samples)\n", share(count), where(pc),
                            count);
    }
    text += fmt::format("guest runtime: {:5.1f}% dispatcher, thunks, unmapped code ({} samples)\n",
                        share(guest.runtime), guest.runtime);
    text += "-- guest code in host calls (block that called a callback, module+offset)\n";
    for (const auto& [pc, count] : guest.by_host) {
        text += fmt::format("guest host call: {:5.1f}% {} ({} samples)\n", share(count),
                            where(pc), count);
    }
    std::map<std::string, std::pair<unsigned, unsigned>> modules;
    std::map<std::pair<std::string, u64>, unsigned> regions;
    for (const auto& [pc, counts] : guest.blocks) {
        const auto [module, offset] = tables.map.Locate(pc);
        modules[module].first += counts.self;
        modules[module].second += counts.host;
        if (counts.self != 0) {
            regions[{module, offset >> 16}] += counts.self;
        }
    }
    std::vector<std::pair<unsigned, std::string>> module_order;
    for (const auto& [name, counts] : modules) {
        module_order.emplace_back(counts.first, name);
    }
    std::sort(module_order.rbegin(), module_order.rend());
    text += "-- guest modules (samples inside blocks, and in host calls they made)\n";
    for (const auto& [self, name] : module_order) {
        text += fmt::format("guest module: {:5.1f}% {} ({} samples, {} in host calls)\n",
                            share(self), name, self, modules[name].second);
    }
    std::vector<std::pair<unsigned, std::pair<std::string, u64>>> region_order;
    for (const auto& [key, count] : regions) {
        region_order.emplace_back(count, key);
    }
    std::sort(region_order.rbegin(), region_order.rend());
    text += "-- guest regions (64 KiB of guest address space, samples inside blocks)\n";
    for (size_t i = 0; i < (std::min<size_t>)(region_order.size(), 30); ++i) {
        const auto& [count, key] = region_order[i];
        text += fmt::format("guest region: {:5.1f}% {}+{:#x} ({} samples)\n", share(count),
                            key.first, key.second << 16, count);
    }
}

// The label of a sample's first JIT frame in the folded stacks.
std::string JitLabel(const GuestTables& tables, const JitFrame& info) {
    if (info.pc == NoBlock) {
        return "(jit) runtime";
    }
    const auto [module, offset] = tables.map.Locate(info.pc);
    return fmt::format("(jit) {}+{:#x}", module, offset & ~u64{0xFFF});
}

// Where a CPUCore thread's time goes. Time in JIT code is split by the guest-code attribution;
// the rest by the host call the thread is in, matched on frame names (nearest cause first).
enum Split : unsigned {
    SplitFault,
    SplitIdle,
    SplitSvc,
    SplitTranslation,
    SplitMemory,
    SplitRunLoop,
    SplitOther,
    SplitCount,
};

unsigned SplitBits(const std::string& name) {
    unsigned bits = 0;
    const auto add = [&](Split split, std::initializer_list<const char*> parts) {
        for (const char* part : parts) {
            if (name.find(part) != std::string::npos) {
                bits |= 1U << split;
                return;
            }
        }
    };
    add(SplitFault, {"FastmemCallback", "ExceptionHandler", "KiUserExceptionDispatch",
                     "RtlDispatchException", "RtlpExecuteHandler", "RtlRaiseException",
                     "RtlUnwind", "RtlRestoreContext", "NtContinue", "ZwContinue",
                     "DynarmicCallbacks64::ExceptionRaised",
                     "DynarmicCallbacks64::ReturnException"});
    add(SplitIdle, {"PhysicalCore::Idle", "IdleThreadFunction", "ZwWaitFor", "NtWaitFor",
                    "WaitForMultipleObjects",
                    "WaitForSingleObject", "ZwDelayExecution", "NtDelayExecution", "SleepEx",
                    "sched_yield", "SwitchToThread", "NtYieldExecution", "ZwYieldExecution",
                    "pthread_cond", "RtlSleepConditionVariable", "RtlWaitOnAddress"});
    add(SplitSvc, {"Kernel::Svc::", "CallSVC", "SvcArguments"});
    add(SplitTranslation, {"Dynarmic::A64::Translate", "Impl::GetBlock", "Dynarmic::Optimization",
                           "EmitX64::", "Dynarmic::IR::", "Xbyak", "A64::Decode",
                           "TranslatorVisitor", "RegAlloc", "PerformRequestedCacheInvalidation",
                           "EnsureMemoryCommitted"});
    add(SplitMemory, {"DynarmicCallbacks64::Memory", "DynarmicCallbacks64::CheckMemoryAccess",
                      "Core::Memory::Memory::", "DeviceMemoryManager"});
    add(SplitRunLoop, {"PhysicalCore::RunThread", "ArmDynarmic64::RunThread",
                       "Dynarmic::A64::Jit::Run", "Jit::Impl::Run", "BlockOfCode::RunCode",
                       "Nx2Bottleneck", "CpuManager::", "KScheduler::"});
    return bits;
}

std::string CpuSplit(Symbolizer& symbolizer, const std::vector<Sample>& samples,
                     const std::vector<JitFrame>& jit, unsigned thread, unsigned total) {
    unsigned guest = 0;
    unsigned runtime = 0;
    unsigned unattributed = 0;
    std::array<unsigned, SplitCount> split{};
    std::unordered_map<DWORD64, unsigned> bits_of;
    const auto bits = [&](DWORD64 address) {
        const auto [it, added] = bits_of.try_emplace(address, 0U);
        if (added) {
            it->second = SplitBits(symbolizer.Name(address));
        }
        return it->second;
    };
    for (size_t s = 0; s < samples.size(); ++s) {
        const Sample& sample = samples[s];
        if (sample.thread != thread || sample.depth == 0) {
            continue;
        }
        const JitFrame& info = jit[s];
        if (info.frame == 0) {
            ++(info.pc != NoBlock ? guest : runtime);
            continue;
        }
        if (info.frame < 0 && symbolizer.Name(sample.frames[0]) == "(jit)") {
            ++unattributed;
            continue;
        }
        // Only the frames below the JIT frame belong to the host call; the ones above it come
        // from an unwind through generated code.
        unsigned anywhere = 0;
        unsigned innermost = 0;
        for (unsigned i = 0; i < sample.depth; ++i) {
            if (static_cast<int>(i) == info.frame || symbolizer.Name(sample.frames[i]) == "(jit)") {
                break;
            }
            const unsigned frame_bits = bits(sample.frames[i]);
            anywhere |= frame_bits;
            if (i < 4) {
                innermost |= frame_bits;
            }
        }
        Split category = SplitOther;
        for (const Split candidate :
             {SplitFault, SplitIdle, SplitSvc, SplitTranslation, SplitMemory}) {
            if (anywhere & (1U << candidate)) {
                category = candidate;
                break;
            }
        }
        if (category == SplitOther && (innermost & (1U << SplitRunLoop))) {
            category = SplitRunLoop;
        }
        ++split[category];
    }
    const auto share = [&](unsigned n) { return 100.0 * n / (std::max)(total, 1U); };
    return fmt::format(
        "cpu split: guest code {:.1f}% | jit runtime {:.1f}% | jit unattributed {:.1f}% | "
        "run loop {:.1f}% | svc {:.1f}% | memory callbacks {:.1f}% | faults {:.1f}% | "
        "translation {:.1f}% | idle {:.1f}% | other {:.1f}%\n",
        share(guest), share(runtime), share(unattributed), share(split[SplitRunLoop]),
        share(split[SplitSvc]), share(split[SplitMemory]), share(split[SplitFault]),
        share(split[SplitTranslation]), share(split[SplitIdle]), share(split[SplitOther]));
}

std::string Report(const std::vector<Target>& targets, const std::vector<Sample>& samples,
                   const char* path) {
    Symbolizer symbolizer;
    std::string text;
    JitStats jit_stats;
    const std::vector<JitFrame> jit = ResolveJit(samples, jit_stats);
    const GuestTables guest = BuildGuest(samples, jit, targets.size());
    if (GuestTracking()) {
        text += fmt::format("guest attribution: {} samples in JIT code, {} mapped to a block, "
                            "{} JIT log entries, {} dropped, {} samples refused\n",
                            jit_stats.in_jit, jit_stats.mapped, jit_stats.entries,
                            jit_stats.dropped, jit_stats.refused);
    }
    for (unsigned thread = 0; thread < targets.size(); ++thread) {
        std::map<std::string, unsigned> self;
        std::map<std::string, unsigned> inclusive;
        std::map<std::string, unsigned> self_caller;
        unsigned total = 0;
        for (const Sample& sample : samples) {
            if (sample.thread != thread || sample.depth == 0) {
                continue;
            }
            ++total;
            const std::string& top = symbolizer.Name(sample.frames[0]);
            ++self[top];
            if (sample.depth > 1) {
                ++self_caller[top + "  <-  " + symbolizer.Name(sample.frames[1])];
            }
            std::set<std::string> seen;
            for (unsigned i = 0; i < sample.depth; ++i) {
                const std::string& name = symbolizer.Name(sample.frames[i]);
                if (seen.insert(name).second) {
                    ++inclusive[name];
                }
            }
        }
        const auto print = [&](const char* title, const std::map<std::string, unsigned>& counts,
                               size_t limit) {
            std::vector<std::pair<unsigned, std::string>> sorted;
            for (const auto& [name, count] : counts) {
                sorted.emplace_back(count, name);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            text += fmt::format("-- {}\n", title);
            for (size_t i = 0; i < (std::min)(limit, sorted.size()); ++i) {
                text += fmt::format("{:6} {:5.1f}% {}\n", sorted[i].first,
                                    100.0 * sorted[i].first / (std::max)(total, 1U),
                                    sorted[i].second.c_str());
            }
        };
        text += fmt::format("=== thread \"{}\": {} samples\n", targets[thread].name, total);
        if (targets[thread].name.rfind("CPUCore", 0) == 0) {
            text += CpuSplit(symbolizer, samples, jit, thread, total);
        }
        if (!guest.threads[thread].blocks.empty() || guest.threads[thread].runtime != 0) {
            PrintGuest(text, guest, guest.threads[thread], total);
        }
        print("self (where the thread is)", self, 60);
        // The hottest instructions as module offsets, for llvm-symbolizer --relative-address
        // (which line of a function, inlined code included).
        std::map<DWORD64, unsigned> addresses;
        for (const Sample& sample : samples) {
            if (sample.thread == thread && sample.depth != 0) {
                ++addresses[sample.frames[0]];
            }
        }
        std::vector<std::pair<unsigned, DWORD64>> hot;
        for (const auto& [address, count] : addresses) {
            hot.emplace_back(count, address);
        }
        std::sort(hot.rbegin(), hot.rend());
        text += "-- self by instruction (module+offset)\n";
        for (size_t i = 0; i < (std::min<size_t>)(hot.size(), 60); ++i) {
            text += fmt::format("{:6} {} {}\n", hot[i].first, ModuleOffset(hot[i].second),
                                symbolizer.Name(hot[i].second));
        }
        print("self <- caller", self_caller, 50);
        print("inclusive (on the stack)", inclusive, 90);
        text += "\n";
    }
    if (std::FILE* const file = std::fopen(path, "w")) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }

    // <out>.folded: every distinct stack with its count, root first ("thread;a;b;c count").
    std::map<std::string, unsigned> folded;
    for (size_t s = 0; s < samples.size(); ++s) {
        const Sample& sample = samples[s];
        std::string line = targets[sample.thread].name;
        for (unsigned i = sample.depth; i-- > 0;) {
            line += ';';
            if (static_cast<int>(i) == jit[s].frame) {
                line += JitLabel(guest, jit[s]);
            } else {
                line += symbolizer.Name(sample.frames[i]);
            }
        }
        ++folded[line];
    }
    if (std::FILE* const out = std::fopen((std::string{path} + ".folded").c_str(), "w")) {
        for (const auto& [line, count] : folded) {
            std::fprintf(out, "%s %u\n", line.c_str(), count);
        }
        std::fclose(out);
    }

    // <out>.timeline: "P <qpc>" per present and "S <qpc>\t<thread>\t<root;...;leaf>" per sample.
    if (std::FILE* const out = std::fopen((std::string{path} + ".timeline").c_str(), "w")) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        std::fprintf(out, "F %lld\n", static_cast<long long>(frequency.QuadPart));
        const size_t count = (std::min)(present_count.load(), presents.size());
        for (size_t i = 0; i < count; ++i) {
            std::fprintf(out, "P %lld\n", presents[i]);
        }
        for (const Sample& sample : samples) {
            std::string stack;
            for (unsigned i = sample.depth; i-- > 0;) {
                stack += symbolizer.Name(sample.frames[i]);
                if (i != 0) {
                    stack += ';';
                }
            }
            std::fprintf(out, "S %lld\t%s\t%s\n", sample.qpc, targets[sample.thread].name.c_str(),
                         stack.c_str());
        }
        std::fclose(out);
    }
    return text;
}

std::string SampleThreads(const std::vector<std::string>& patterns, double seconds,
                          const std::string& out) {
    const std::string completion_path = out + ".complete";
    std::remove(completion_path.c_str());
    const std::vector<Target> targets = FindThreads(patterns);
    std::fprintf(stderr, "nx2 sampler: %zu threads match, sampling %.0f s\n", targets.size(),
                 seconds);
    if (targets.empty()) {
        return "no thread matches";
    }
    std::vector<Sample> samples;
    samples.reserve(static_cast<size_t>(seconds * 1100.0) * targets.size() + 16);
    LARGE_INTEGER start_qpc{}, end_qpc{}, qpc_frequency{};
    QueryPerformanceFrequency(&qpc_frequency);
    QueryPerformanceCounter(&start_qpc);
    // Sleep(1) lasts a whole 15.6 ms scheduler tick unless something raised the timer
    // resolution; a high resolution timer keeps the interval near 1 ms either way.
    constexpr DWORD HighResolutionTimer = 0x2; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
    const HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, HighResolutionTimer, TIMER_ALL_ACCESS);
    present_count.store(0);
    recording_presents.store(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    const bool track_guest = GuestTracking();
    while (std::chrono::steady_clock::now() < deadline &&
           samples.size() + targets.size() <= samples.capacity()) {
        for (unsigned i = 0; i < targets.size(); ++i) {
            const HANDLE handle = targets[i].handle;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(handle, &context)) {
                Sample& sample = samples.emplace_back();
                sample.qpc = now.QuadPart;
                sample.thread = i;
                if (track_guest) {
                    sample.epoch = jit_log->epoch.load(std::memory_order_relaxed);
                }
                sample.depth = Walk(context, sample.frames);
            }
            ResumeThread(handle);
        }
        LARGE_INTEGER due{};
        due.QuadPart = -10000; // 1 ms, relative
        if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
    }
    recording_presents.store(false);
    if (timer != nullptr) {
        CloseHandle(timer);
    }
    QueryPerformanceCounter(&end_qpc);
    std::string text = Report(targets, samples, out.c_str());
    if (std::FILE* complete = std::fopen(completion_path.c_str(), "w")) {
        std::fprintf(complete,
                     "{\"start_qpc\":%lld,\"end_qpc\":%lld,\"qpc_frequency\":%lld,"
                     "\"samples\":%zu,\"threads\":%zu}\n",
                     static_cast<long long>(start_qpc.QuadPart),
                     static_cast<long long>(end_qpc.QuadPart),
                     static_cast<long long>(qpc_frequency.QuadPart), samples.size(), targets.size());
        std::fclose(complete);
    }
    std::fprintf(stderr, "nx2 sampler: %zu samples written to %s\n", samples.size(), out.c_str());
    for (const auto& target : targets) {
        CloseHandle(target.handle);
    }
    return text;
}

std::string Stacks() {
    const std::vector<Target> targets = FindThreads({"*"});
    // Sized before any thread is paused: see Walk.
    std::vector<Sample> samples(targets.size());
    for (unsigned i = 0; i < targets.size(); ++i) {
        const HANDLE handle = targets[i].handle;
        samples[i].depth = 0;
        if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(handle, &context)) {
            samples[i].depth = Walk(context, samples[i].frames);
        }
        ResumeThread(handle);
    }
    Symbolizer symbolizer;
    // Threads at the same place (a pool of idle workers) are listed together.
    std::map<std::string, std::string> threads_by_stack;
    for (unsigned i = 0; i < targets.size(); ++i) {
        std::string stack;
        for (unsigned frame = 0; frame < samples[i].depth; ++frame) {
            stack += fmt::format("     {}\n", symbolizer.Where(samples[i].frames[frame]));
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        GetThreadTimes(targets[i].handle, &created, &exited, &kernel, &user);
        const auto ticks = [](const FILETIME& time) {
            return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        threads_by_stack[stack] +=
            fmt::format("== \"{}\" thread {}, {:.1f} s of CPU\n", targets[i].name,
                        GetThreadId(targets[i].handle),
                        static_cast<double>(ticks(kernel) + ticks(user)) / 1e7);
        CloseHandle(targets[i].handle);
    }
    std::string text = fmt::format("{} threads\n\n", targets.size());
    for (const auto& [stack, threads] : threads_by_stack) {
        text += threads + stack + "\n";
    }
    return text;
}

std::atomic<u64> guest_frames{};
std::atomic_bool guest_running{};

void WatchStalls(unsigned limit_seconds) {
    Common::SetCurrentThreadName("StallWatch");
    u64 seen = 0;
    // Counted in one second polls, not clock time: a sleeping PC is not a stalled guest.
    unsigned idle_seconds = 0;
    for (unsigned reports = 0; reports < 3;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const u64 frames = guest_frames.load(std::memory_order_relaxed);
        if (frames != seen || !guest_running.load(std::memory_order_relaxed)) {
            seen = frames;
            idle_seconds = 0;
            continue;
        }
        if (++idle_seconds != limit_seconds) {
            continue;
        }
        ++reports;
        char stamp[32]{};
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
        const std::string path = fmt::format(
            "{}/stall_{}.txt", Common::FS::GetEdenPathString(Common::FS::EdenPath::LogDir), stamp);
        const std::string text =
            fmt::format("No guest frame for {} s (after {} frames).\n", limit_seconds, frames) +
            Stacks();
        if (std::FILE* const file = std::fopen(path.c_str(), "w")) {
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
        }
        LOG_CRITICAL(Common, "No guest frame for {} s, every thread's call stack is in {}",
                     limit_seconds, path);
    }
}

} // Anonymous namespace

std::string Sample(const char* names, double seconds, const std::string& out) {
    return SampleThreads(SplitNames(names), seconds, out);
}

void NotePresent() {
    if (!recording_presents.load(std::memory_order_relaxed)) {
        return;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const size_t index = present_count.fetch_add(1, std::memory_order_relaxed);
    if (index < presents.size()) {
        presents[index] = now.QuadPart;
    }
}

void NoteJitCode(const void* begin, std::size_t size) {
    AppendJit(begin, size, RegionMark);
}

void NoteJitBlock(const void* host, std::size_t size, std::uint64_t guest_pc) {
    AppendJit(host, size, guest_pc);
}

void NoteJitClear(const void* begin, std::size_t size) {
    AppendJit(begin, size, ClearMark);
}

void SetGuestModules(std::vector<GuestModule> modules) {
    std::scoped_lock lock{modules_mutex};
    guest_modules = std::move(modules);
}

void StartFromEnvironment() {
    const char* names = std::getenv("EDEN_NX2_SAMPLE");
    const char* guest = std::getenv("EDEN_NX2_SAMPLE_GUEST");
    const bool sampling = names != nullptr && *names != '\0';
    // Decided here, before any JIT exists: blocks emitted earlier cannot be attributed.
    g_track_guest.store(guest != nullptr ? *guest != '\0' && *guest != '0' : sampling,
                        std::memory_order_relaxed);
    if (!sampling) {
        return;
    }
    const char* out = std::getenv("EDEN_NX2_SAMPLE_OUT");
    std::thread{[patterns = SplitNames(names), start = EnvSeconds("EDEN_NX2_SAMPLE_START", 60.0),
                 seconds = EnvSeconds("EDEN_NX2_SAMPLE_SECONDS", 30.0),
                 out = std::string{out != nullptr ? out : "nx2_samples.txt"}] {
        std::this_thread::sleep_for(std::chrono::duration<double>(start));
        SampleThreads(patterns, seconds, out);
    }}.detach();
}

void NoteGuestFrame() {
    guest_frames.fetch_add(1, std::memory_order_relaxed);
}

void SetGuestRunning(bool running) {
    guest_running.store(running, std::memory_order_relaxed);
    static std::once_flag started;
    if (!running) {
        return;
    }
    std::call_once(started, [] {
        const double limit = EnvSeconds("EDEN_NX2_STALL_SECONDS", 60.0);
        if (limit >= 1.0) {
            std::thread{WatchStalls, static_cast<unsigned>(limit)}.detach();
        }
    });
}

std::string CurrentStack(unsigned skip) {
    void* frames[MaxDepth];
    const USHORT count = RtlCaptureStackBackTrace(skip + 1, MaxDepth, frames, nullptr);
    Symbolizer symbolizer;
    std::string text;
    for (USHORT i = 0; i < count; ++i) {
        text += "\n  ";
        text += symbolizer.Where(reinterpret_cast<DWORD64>(frames[i]));
    }
    return text;
}

} // namespace Nx2Sampler

#else

namespace Nx2Sampler {
void StartFromEnvironment() {}
std::string Sample(const char*, double, const std::string&) {
    return "the sampler is Windows only";
}
void NotePresent() {}
void NoteGuestFrame() {}
void NoteJitCode(const void*, std::size_t) {}
void NoteJitBlock(const void*, std::size_t, std::uint64_t) {}
void NoteJitClear(const void*, std::size_t) {}
void SetGuestModules(std::vector<GuestModule>) {}
void SetGuestRunning(bool) {}
std::string CurrentStack(unsigned) {
    return {};
}
} // namespace Nx2Sampler

#endif
