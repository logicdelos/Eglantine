// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <functional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/query_cache/query_cache.h"
#include "video_core/rasterizer_interface.h"

namespace {

class QueryRasterizer final : public VideoCore::RasterizerInterface {
public:
    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&& operation, bool) override {
        ++fences;
        if (defer) {
            pending.push_back(std::move(operation));
        } else {
            operation();
        }
    }
    void SyncOperation(std::function<void()>&& operation) override {
        pending.push_back(std::move(operation));
    }
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}

    bool defer{};
    u32 fences{};
    std::vector<std::function<void()>> pending;
};

class PayloadStreamer final : public VideoCommon::StreamerInterface {
public:
    PayloadStreamer() : StreamerInterface{0} {
        amend_value = 7;
    }

    size_t WriteCounter(VAddr address, bool timestamp, u32 value,
                        std::optional<u32>) override {
        query = VideoCommon::GuestQuery{timestamp, address, value};
        query.flags |= extra_flags;
        return 0;
    }
    VideoCommon::QueryBase* GetQuery(size_t) override {
        return &query;
    }
    void Free(size_t) override {
        ++freed;
    }
    void SetAmend(u64 value) {
        amend_value = value;
    }
    u64 Accumulation() const {
        return accumulation_value;
    }

    VideoCommon::GuestQuery query{false, 0, 0};
    VideoCommon::QueryFlagBits extra_flags{};
    u32 freed{};
};

struct Runtime {
    VideoCommon::StreamerInterface* GetStreamerInterface(VideoCommon::QueryType type) {
        return type == VideoCommon::QueryType::Payload ? &streamer : nullptr;
    }
    void Bind3DEngine(Tegra::Engines::Maxwell3D*) {}

    template <typename T>
    void SyncValues(std::span<T> values) {
        synced.assign(values.begin(), values.end());
        ++syncs;
    }

    PayloadStreamer streamer;
    std::vector<VideoCommon::SyncValuesStruct> synced;
    u32 syncs{};
};

struct Traits {
    using RuntimeType = Runtime;
};

class QueryCache final : public VideoCommon::QueryCacheBase<Traits> {
public:
    using QueryCacheBase::QueryCacheBase;
    using QueryCacheBase::UnregisterPending;

    void UseMemory(Tegra::MemoryManager& memory) {
        gpu_memory = &memory;
    }
};

} // namespace

TEST_CASE("Nx2Perf compute: reused payload upload scratch preserves query filtering",
          "[video_core][nx2_perf][compute_path]") {
    Runtime runtime;
    VideoCommon::GuestStreamer<Traits> streamer{0, runtime};
    VideoCommon::StreamerInterface& interface = streamer;
    for (u32 iteration = 0; iteration < 64; ++iteration) {
        std::vector<size_t> ids;
        std::vector<VideoCommon::SyncValuesStruct> expected;
        const u32 count = iteration % 13;
        for (u32 index = 0; index < count; ++index) {
            const u64 address = 0x1000 + index * 16;
            const u32 value = iteration * 100 + index;
            const bool timestamp = (index & 1) != 0;
            const size_t id = interface.WriteCounter(address, timestamp, value);
            ids.push_back(id);
            if (index % 4 == 0) {
                interface.GetQuery(id)->flags |= VideoCommon::QueryFlagBits::IsInvalidated;
            } else if (index % 4 == 1) {
                interface.GetQuery(id)->flags |= VideoCommon::QueryFlagBits::IsRewritten;
            } else {
                expected.push_back({address, value, timestamp ? 8U : 4U});
            }
        }
        REQUIRE(interface.HasPendingSync() == (count != 0));
        const u32 syncs = runtime.syncs;
        streamer.SyncWrites();
        REQUIRE_FALSE(interface.HasPendingSync());
        REQUIRE(runtime.syncs == syncs + (expected.empty() ? 0U : 1U));
        if (!expected.empty()) {
            REQUIRE(runtime.synced.size() == expected.size());
            for (size_t index = 0; index < expected.size(); ++index) {
                REQUIRE(runtime.synced[index].address == expected[index].address);
                REQUIRE(runtime.synced[index].value == expected[index].value);
                REQUIRE(runtime.synced[index].size == expected[index].size);
            }
        }
        for (size_t index = 0; index < ids.size(); ++index) {
            REQUIRE(True(interface.GetQuery(ids[index])->flags &
                         VideoCommon::QueryFlagBits::IsHostSynced) == (index % 4 >= 2));
            interface.Free(ids[index]);
        }
        streamer.SyncWrites();
        REQUIRE(runtime.syncs == syncs + (expected.empty() ? 0U : 1U));
    }
}

TEST_CASE("Nx2Perf compute: query callbacks preserve immediate and deferred writes",
          "[video_core][nx2_perf][compute_path]") {
    using VideoCommon::QueryFlagBits;
    using VideoCommon::QueryPropertiesFlags;
    using VideoCommon::QueryType;
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    u8* page = system.DeviceMemory().buffer.BackingBasePointer() + 0x10000;
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(page) - 0x4000);
    process.GetMemory().SetCurrentPageTable(process);
    Tegra::MaxwellDeviceMemoryManager device_memory{system.DeviceMemory()};
    const auto asid = device_memory.RegisterProcess(&process.GetMemory());
    device_memory.Map(0x1000, 0x4000, 0x1000, asid);
    QueryRasterizer rasterizer;
    device_memory.BindInterface(&rasterizer);
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    memory.BindRasterizer(&rasterizer);
    memory.Map(0x10000, 0x1000, 0x1000, Tegra::PTEKind::PITCH, false);
    Tegra::GPU gpu{system, false, false};
    Runtime runtime;
    QueryCache cache{gpu, rasterizer, device_memory, runtime};
    cache.UseMemory(memory);

    const auto old_behavior = Settings::values.gpu_fence_behavior.GetValue();
    const auto old_accuracy = Settings::values.current_gpu_accuracy;
    SCOPE_EXIT {
        Settings::values.gpu_fence_behavior.SetValue(old_behavior);
        Settings::values.current_gpu_accuracy = old_accuracy;
    };
    for (const auto accuracy : {Settings::GpuAccuracy::Low, Settings::GpuAccuracy::High}) {
        Settings::values.current_gpu_accuracy = accuracy;
        for (const auto behavior : {Settings::GpuFenceBehavior::Default,
                                    Settings::GpuFenceBehavior::Immediate,
                                    Settings::GpuFenceBehavior::Balanced,
                                    Settings::GpuFenceBehavior::Accurate}) {
            Settings::values.gpu_fence_behavior.SetValue(behavior);
            rasterizer.defer = Settings::IsGPUFenceBehaviorDefault()
                                   ? Settings::IsGPULevelHigh()
                                   : Settings::IsGPUFenceBehaviorBalanced() ||
                                         Settings::IsGPUFenceBehaviorAccurate();
            for (const bool invalidated : {false, true}) {
                for (u32 payload : {0U, 1U, 0xfffffff9U, 0xffffffffU}) {
                    CAPTURE(accuracy, behavior, invalidated, payload);
                    std::memset(page, 0x5a, 16);
                    runtime.streamer.extra_flags = invalidated ? QueryFlagBits::IsInvalidated
                                                              : QueryFlagBits{};
                    const u32 freed_before = runtime.streamer.freed;
                    const u32 fences_before = rasterizer.fences;
                    cache.CounterReport(0x10000, QueryType::Payload,
                                        QueryPropertiesFlags::IsAFence, payload, 0);
                    REQUIRE(rasterizer.fences == fences_before + 1);
                    if (rasterizer.defer) {
                        REQUIRE(device_memory.Read<u32>(0x1000) == 0x5a5a5a5aU);
                        REQUIRE(runtime.streamer.freed == freed_before);
                        REQUIRE(rasterizer.pending.size() == 1);
                        rasterizer.pending.front()();
                        rasterizer.pending.clear();
                        cache.UnregisterPending();
                    } else {
                        REQUIRE(rasterizer.pending.empty());
                    }
                    REQUIRE(runtime.streamer.freed == freed_before + 1);
                    REQUIRE(device_memory.Read<u32>(0x1000) ==
                            (invalidated ? 0x5a5a5a5aU : payload + 7U));
                    REQUIRE(device_memory.Read<u32>(0x1004) == 0x5a5a5a5aU);
                    if (!invalidated) {
                        REQUIRE(runtime.streamer.Accumulation() == u64{payload} + 7);
                    }
                }
            }
        }
    }
}

namespace {

// A few queue words in one device page (device 0x1000, one every 16 bytes) and the cache that
// writes them. The streamer adds nothing to a payload, so a word holds exactly what was released.
struct QueueWords {
    u8* page;
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    QueryRasterizer& rasterizer;
    Runtime& runtime;
    QueryCache& cache;
};

class FenceMode {
public:
    explicit FenceMode(Settings::GpuAccuracy accuracy)
        : old_behavior{Settings::values.gpu_fence_behavior.GetValue()},
          old_accuracy{Settings::values.current_gpu_accuracy} {
        Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Default);
        Settings::values.current_gpu_accuracy = accuracy;
    }
    ~FenceMode() {
        Settings::values.gpu_fence_behavior.SetValue(old_behavior);
        Settings::values.current_gpu_accuracy = old_accuracy;
    }
    FenceMode(const FenceMode&) = delete;
    FenceMode& operator=(const FenceMode&) = delete;

private:
    Settings::GpuFenceBehavior old_behavior;
    Settings::GpuAccuracy old_accuracy;
};

template <typename Body>
void WithQueueWords(Settings::GpuAccuracy accuracy, Body&& body) {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    u8* page = system.DeviceMemory().buffer.BackingBasePointer() + 0x10000;
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(page) - 0x4000);
    process.GetMemory().SetCurrentPageTable(process);
    Tegra::MaxwellDeviceMemoryManager device_memory{system.DeviceMemory()};
    const auto asid = device_memory.RegisterProcess(&process.GetMemory());
    device_memory.Map(0x1000, 0x4000, 0x1000, asid);
    QueryRasterizer rasterizer;
    device_memory.BindInterface(&rasterizer);
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    memory.BindRasterizer(&rasterizer);
    memory.Map(0x10000, 0x1000, 0x1000, Tegra::PTEKind::PITCH, false);
    Tegra::GPU gpu{system, false, false};
    Runtime runtime;
    runtime.streamer.SetAmend(0);
    QueryCache cache{gpu, rasterizer, device_memory, runtime};
    cache.UseMemory(memory);
    const FenceMode mode{accuracy};
    rasterizer.defer = true;
    QueueWords words{page, device_memory, rasterizer, runtime, cache};
    body(words);
}

GPUVAddr WordAddress(u32 index) {
    return 0x10000 + index * 16;
}

u32 ReadWord(QueueWords& words, u32 index = 0) {
    return words.device_memory.Read<u32>(0x1000 + index * 16);
}

void StoreWord(QueueWords& words, u32 value, u32 index = 0) {
    std::memcpy(words.page + index * 16, &value, sizeof(value));
}

void IssueReport(QueueWords& words, u32 index, VideoCommon::QueryPropertiesFlags flags,
                 u32 payload) {
    words.cache.CounterReport(WordAddress(index), VideoCommon::QueryType::Payload, flags, payload,
                              0);
    REQUIRE(words.rasterizer.pending.size() == 1);
}

void LandReports(QueueWords& words) {
    const auto operations = std::move(words.rasterizer.pending);
    words.rasterizer.pending.clear();
    for (const auto& operation : operations) {
        operation();
    }
}

// A report whose fence operation runs, and whose cache entry is then released.
void Report(QueueWords& words, u32 index, VideoCommon::QueryPropertiesFlags flags, u32 payload) {
    const u32 freed = words.runtime.streamer.freed;
    IssueReport(words, index, flags, payload);
    LandReports(words);
    words.cache.UnregisterPending();
    REQUIRE(words.runtime.streamer.freed == freed + 1);
}

constexpr auto Publication =
    VideoCommon::QueryPropertiesFlags::IsAFence | VideoCommon::QueryPropertiesFlags::AdvanceOnly;
constexpr auto GuestReport = VideoCommon::QueryPropertiesFlags::IsAFence;
constexpr u32 AcquireEqual = Tegra::Nx2SemaphoreWaiters::AcquireEqual;
constexpr u32 AcquireGequal = Tegra::Nx2SemaphoreWaiters::AcquireGequal;

} // namespace

TEST_CASE("Nx2 queue word: a syncpoint publication never lowers a word the guest ran ahead",
          "[video_core][nx2_queue_word]") {
    WithQueueWords(Settings::GpuAccuracy::High, [](QueueWords& words) {
        auto& waiters = Tegra::Nx2SemaphoreWaiters::Get();
        int owner{};
        SCOPE_EXIT { waiters.Remove(&owner); };
        // The order of the trace: compute completions had counted the word up to 0x72, then the
        // syncpoint (counting slower) was published into it, once per list.
        for (u32 publication = 0x50; publication <= 0x71; ++publication) {
            CAPTURE(publication);
            StoreWord(words, 0x72);
            waiters.Add(&owner, 0x1000, publication, AcquireEqual);
            Report(words, 0, Publication, publication);
            REQUIRE(ReadWord(words) == 0x72U);
            // The acquire that the lowered word had stalled (>= 0x64) is met.
            REQUIRE(Tegra::Nx2SemaphoreWaiters::Satisfies(AcquireGequal, 0x64, ReadWord(words)));
            // A write that changed nothing wakes nobody.
            REQUIRE_FALSE(waiters.Latched(&owner));
        }
    });
}

TEST_CASE("Nx2 queue word: a syncpoint publication still advances a word that is behind or equal",
          "[video_core][nx2_queue_word]") {
    WithQueueWords(Settings::GpuAccuracy::High, [](QueueWords& words) {
        auto& waiters = Tegra::Nx2SemaphoreWaiters::Get();
        int owner{};
        SCOPE_EXIT { waiters.Remove(&owner); };
        for (const u32 before : {0x00U, 0x10U, 0x5bU, 0x5cU, 0xffffffffU}) {
            CAPTURE(before);
            StoreWord(words, before);
            waiters.Add(&owner, 0x1000, 0x5c, AcquireEqual);
            Report(words, 0, Publication, 0x5c);
            REQUIRE(ReadWord(words) == 0x5cU);
            REQUIRE(waiters.Latched(&owner));
        }
    });
}

TEST_CASE("Nx2 queue word: the advance test is a signed distance",
          "[video_core][nx2_queue_word]") {
    struct Case {
        u32 word;
        u32 publication;
        u32 expected;
    };
    const Case cases[] = {
        {0xfffffff0U, 0x00000005U, 0x00000005U}, // the counter wrapped: newer
        {0x00000005U, 0xfffffff0U, 0x00000005U}, // an old value from before the wrap: older
        {0xffffffffU, 0x0000005cU, 0x0000005cU}, // a word reset to all ones takes any value
        {0x00000000U, 0x7fffffffU, 0x7fffffffU}, // largest step forward
        {0x00000000U, 0x80000000U, 0x00000000U}, // smallest step back
        {0x00000072U, 0x0000005cU, 0x00000072U},
        {0x00000072U, 0x00000072U, 0x00000072U},
    };
    WithQueueWords(Settings::GpuAccuracy::High, [&cases](QueueWords& words) {
        for (const Case& test : cases) {
            CAPTURE(test.word, test.publication);
            StoreWord(words, test.word);
            Report(words, 0, Publication, test.publication);
            REQUIRE(ReadWord(words) == test.expected);
        }
    });
}

TEST_CASE("Nx2 queue word: a guest release writes what it carries, a lower value included",
          "[video_core][nx2_queue_word]") {
    WithQueueWords(Settings::GpuAccuracy::High, [](QueueWords& words) {
        for (const u32 payload : {0x00U, 0x10U, 0x5cU, 0x71U, 0x73U, 0xffffffffU}) {
            CAPTURE(payload);
            StoreWord(words, 0x72);
            Report(words, 0, GuestReport, payload);
            REQUIRE(ReadWord(words) == payload);
        }
    });
}

TEST_CASE("Nx2 queue word: a flush of the cached publication keeps a word that moved ahead",
          "[video_core][nx2_queue_word]") {
    WithQueueWords(Settings::GpuAccuracy::High, [](QueueWords& words) {
        StoreWord(words, 0x10);
        IssueReport(words, 0, Publication, 0x5c);
        LandReports(words);
        REQUIRE(ReadWord(words) == 0x5cU);
        // The report stays cached until a later fence frees it. A compute completion counts the
        // word on in the meantime, and a flush of the address writes the cached value again.
        StoreWord(words, 0x72);
        REQUIRE_FALSE(words.cache.SemiFlushRegion(0x1000, 4));
        REQUIRE(ReadWord(words) == 0x72U);
        // A word that fell behind the cached value (a reset) is written forward.
        StoreWord(words, 0x10);
        REQUIRE_FALSE(words.cache.SemiFlushRegion(0x1000, 4));
        REQUIRE(ReadWord(words) == 0x5cU);
        words.cache.UnregisterPending();
    });
}

TEST_CASE("Nx2 queue word: a publication outside a fence keeps the newer word too",
          "[video_core][nx2_queue_word]") {
    WithQueueWords(Settings::GpuAccuracy::Low, [](QueueWords& words) {
        auto& waiters = Tegra::Nx2SemaphoreWaiters::Get();
        int owner{};
        SCOPE_EXIT { waiters.Remove(&owner); };
        StoreWord(words, 0x72);
        waiters.Add(&owner, 0x1000, 0x5c, AcquireEqual);
        words.cache.CounterReport(WordAddress(0), VideoCommon::QueryType::Payload,
                                  VideoCommon::QueryPropertiesFlags::AdvanceOnly, 0x5c, 0);
        REQUIRE(words.rasterizer.pending.empty());
        REQUIRE(ReadWord(words) == 0x72U);
        REQUIRE_FALSE(waiters.Latched(&owner));
        words.cache.CounterReport(WordAddress(0), VideoCommon::QueryType::Payload,
                                  VideoCommon::QueryPropertiesFlags::AdvanceOnly, 0x80, 0);
        REQUIRE(ReadWord(words) == 0x80U);
        // A plain report of a lower value lands.
        words.cache.CounterReport(WordAddress(0), VideoCommon::QueryType::Payload,
                                  VideoCommon::QueryPropertiesFlags{}, 0x10, 0);
        REQUIRE(ReadWord(words) == 0x10U);
    });
}

TEST_CASE("Nx2 queue word: each word is judged on its own value",
          "[video_core][nx2_queue_word]") {
    WithQueueWords(Settings::GpuAccuracy::High, [](QueueWords& words) {
        StoreWord(words, 0x72, 0);
        StoreWord(words, 0x10, 1);
        StoreWord(words, 0x90, 2);
        Report(words, 0, Publication, 0x5c);
        Report(words, 1, Publication, 0x5c);
        Report(words, 2, Publication, 0x5c);
        REQUIRE(ReadWord(words, 0) == 0x72U);
        REQUIRE(ReadWord(words, 1) == 0x5cU);
        REQUIRE(ReadWord(words, 2) == 0x90U);
        REQUIRE(ReadWord(words, 3) == 0U);
    });
}

TEST_CASE("Nx2 queue word: issued releases predict an advance-only report as a keep-newer store",
          "[video_core][nx2_queue_word]") {
    auto& issued = Tegra::Nx2IssuedReleases::Get();
    issued.Clear();
    SCOPE_EXIT { issued.Clear(); };
    constexpr u64 address = 0x1000;
    issued.IssueAdvance(address, 0x5c);
    REQUIRE(issued.HasPendingReport(address));
    REQUIRE_FALSE(issued.HasReductionIn(address, 4));
    // The word is ahead: it stays, so the acquire is met and an equal wait for the report is not.
    REQUIRE(issued.Meets(address, 0x64, AcquireGequal, [] { return 0x72U; }));
    REQUIRE(issued.Meets(address, 0x72, AcquireEqual, [] { return 0x72U; }));
    REQUIRE_FALSE(issued.Meets(address, 0x5c, AcquireEqual, [] { return 0x72U; }));
    // The word is behind: the report advances it.
    REQUIRE(issued.Meets(address, 0x5c, AcquireEqual, [] { return 0x10U; }));
    REQUIRE_FALSE(issued.Meets(address, 0x64, AcquireGequal, [] { return 0x10U; }));
    // The word wrapped.
    REQUIRE(issued.Meets(address, 0x05, AcquireEqual, [] { return 0xfffffff0U; }));
    issued.Settle(address);
    REQUIRE_FALSE(issued.Any());
}

TEST_CASE("Nx2 queue word: issued releases apply a publication after a max release in order",
          "[video_core][nx2_queue_word]") {
    auto& issued = Tegra::Nx2IssuedReleases::Get();
    issued.Clear();
    SCOPE_EXIT { issued.Clear(); };
    constexpr u64 address = 0x1000;
    constexpr u32 reduce_max = 2;
    issued.IssueReduction(address, reduce_max, false, 0x72);
    issued.IssueAdvance(address, 0x5c);
    REQUIRE(issued.HasReductionIn(address, 4));
    REQUIRE(issued.HasPendingReport(address));
    // The word reaches 0x72 and stays there: it never reads 0x5c at any step.
    REQUIRE(issued.Meets(address, 0x64, AcquireGequal, [] { return 0x10U; }));
    REQUIRE(issued.Meets(address, 0x72, AcquireEqual, [] { return 0x10U; }));
    REQUIRE_FALSE(issued.Meets(address, 0x5c, AcquireEqual, [] { return 0x10U; }));
    // After the reduction settled only the publication is pending.
    issued.Settle(address);
    REQUIRE_FALSE(issued.HasReductionIn(address, 4));
    REQUIRE(issued.HasPendingReport(address));
    REQUIRE(issued.Meets(address, 0x72, AcquireEqual, [] { return 0x72U; }));
    REQUIRE_FALSE(issued.Meets(address, 0x5c, AcquireEqual, [] { return 0x72U; }));
}
