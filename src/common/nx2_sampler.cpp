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
#include <map>
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

struct Symbolizer {
    using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
    using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    using SymGetLineFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);

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
        if (!initialize(GetCurrentProcess(), nullptr, TRUE)) {
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

std::string Report(const std::vector<Target>& targets, const std::vector<Sample>& samples,
                   const char* path) {
    Symbolizer symbolizer;
    std::string text;
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
    for (const Sample& sample : samples) {
        std::string line = targets[sample.thread].name;
        for (unsigned i = sample.depth; i-- > 0;) {
            line += ';';
            line += symbolizer.Name(sample.frames[i]);
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

void StartFromEnvironment() {
    const char* names = std::getenv("EDEN_NX2_SAMPLE");
    if (names == nullptr || *names == '\0') {
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

} // namespace Nx2Sampler

#else

namespace Nx2Sampler {
void StartFromEnvironment() {}
std::string Sample(const char*, double, const std::string&) {
    return "the sampler is Windows only";
}
void NotePresent() {}
void NoteGuestFrame() {}
void SetGuestRunning(bool) {}
} // namespace Nx2Sampler

#endif
