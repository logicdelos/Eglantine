// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/read_attribution.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include <fmt/format.h>

#include "common/logging.h"

namespace Common::ReadAttribution {

namespace {

struct Meta {
    Kind kind{};
    std::string name;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::string, u32> ids;
    std::array<Meta, MaxSources> meta;
    u32 next{1};
    std::chrono::steady_clock::time_point window_begin{std::chrono::steady_clock::now()};
};

Registry& GetRegistry() {
    static Registry registry;
    return registry;
}

constexpr u32 OverflowId = MaxSources - 1;

std::string FormatBytes(u64 bytes) {
    constexpr double Kib = 1024.0;
    const double value = static_cast<double>(bytes);
    if (bytes >= (u64{1} << 30)) {
        return fmt::format("{:.2f} GiB", value / (Kib * Kib * Kib));
    }
    if (bytes >= (u64{1} << 20)) {
        return fmt::format("{:.1f} MiB", value / (Kib * Kib));
    }
    return fmt::format("{:.1f} KiB", value / Kib);
}

/// A long path keeps its end, where the file name is.
std::string ShortName(const std::string& name) {
    constexpr std::size_t Limit = 96;
    if (name.size() <= Limit) {
        return name;
    }
    return "..." + name.substr(name.size() - (Limit - 3));
}

std::vector<Row> TakeRows(Registry& registry, std::unique_lock<std::mutex>& lock) {
    static_cast<void>(lock);
    std::vector<Row> rows;
    for (u32 id = 1; id < MaxSources; ++id) {
        auto& slot = Detail::slots[id];
        const u64 reads = slot.reads.exchange(0, std::memory_order_relaxed);
        const u64 bytes = slot.bytes.exchange(0, std::memory_order_relaxed);
        if (reads == 0) {
            continue;
        }
        const Meta& meta = registry.meta[id];
        rows.push_back({meta.kind, meta.name, reads, bytes});
    }
    return rows;
}

} // Anonymous namespace

std::string_view KindName(Kind kind) noexcept {
    switch (kind) {
    case Kind::Storage:
        return "storage";
    case Kind::File:
        return "file";
    case Kind::NupMember:
        return "nup";
    }
    return "?";
}

u32 Register(Kind kind, std::string_view name) {
    Registry& registry = GetRegistry();
    std::string key;
    key.reserve(name.size() + 1);
    key.push_back(static_cast<char>('0' + static_cast<int>(kind)));
    key.append(name);

    const std::scoped_lock lock{registry.mutex};
    if (const auto it = registry.ids.find(key); it != registry.ids.end()) {
        return it->second;
    }
    if (registry.next >= OverflowId) {
        registry.meta[OverflowId] = {Kind::File, "(other sources)"};
        return OverflowId;
    }
    const u32 id = registry.next++;
    registry.meta[id] = {kind, std::string{name}};
    registry.ids.emplace(std::move(key), id);
    return id;
}

std::optional<std::string> FormatBurst(double seconds, std::span<const Row> rows) {
    std::array<u64, KindCount> reads{};
    std::array<u64, KindCount> bytes{};
    for (const Row& row : rows) {
        reads[static_cast<std::size_t>(row.kind)] += row.reads;
        bytes[static_cast<std::size_t>(row.kind)] += row.bytes;
    }
    bool burst = false;
    for (std::size_t kind = 0; kind < KindCount; ++kind) {
        burst = burst || reads[kind] >= BurstReads || bytes[kind] >= BurstBytes;
    }
    if (!burst) {
        return std::nullopt;
    }

    std::string text = fmt::format("Game reads in the last {:.1f} s:", seconds);
    const char* separator = " ";
    for (std::size_t kind = 0; kind < KindCount; ++kind) {
        if (reads[kind] == 0) {
            continue;
        }
        text += fmt::format("{}{} {} reads {}", separator, KindName(static_cast<Kind>(kind)),
                            reads[kind], FormatBytes(bytes[kind]));
        separator = ", ";
    }

    std::vector<const Row*> order;
    order.reserve(rows.size());
    for (const Row& row : rows) {
        order.push_back(&row);
    }
    const std::size_t top = std::min(TopSources, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(top), order.end(),
                      [](const Row* a, const Row* b) {
                          return a->bytes != b->bytes ? a->bytes > b->bytes : a->reads > b->reads;
                      });
    text += ". Top sources by bytes:";
    for (std::size_t i = 0; i < top; ++i) {
        const Row& row = *order[i];
        text += fmt::format("{} [{}] {} {} reads {}", i == 0 ? "" : ";", KindName(row.kind),
                            ShortName(row.name), row.reads, FormatBytes(row.bytes));
    }
    return text;
}

void EmitWindowReport() {
    Registry& registry = GetRegistry();
    std::unique_lock lock{registry.mutex};
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - registry.window_begin).count();
    if (seconds < WindowSeconds * 0.9) {
        return;
    }
    registry.window_begin = now;
    const std::vector<Row> rows = TakeRows(registry, lock);
    lock.unlock();
    if (const auto text = FormatBurst(seconds, rows)) {
        LOG_INFO(Service_FS, "{}", *text);
    }
}

std::vector<Row> TakeWindowForTests() {
    Registry& registry = GetRegistry();
    std::unique_lock lock{registry.mutex};
    return TakeRows(registry, lock);
}

} // namespace Common::ReadAttribution
