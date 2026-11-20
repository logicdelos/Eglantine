// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/guest_call/nvn_census.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/svc.h"

namespace Kernel::Svc {
namespace {

constexpr bool IsValidAddressRange(u64 address, u64 size) {
    return address + size > address;
}

constexpr bool IsValidProcessMemoryPermission(Svc::MemoryPermission perm) {
    switch (perm) {
    case Svc::MemoryPermission::None:
    case Svc::MemoryPermission::Read:
    case Svc::MemoryPermission::ReadWrite:
    case Svc::MemoryPermission::ReadExecute:
        return true;
    default:
        return false;
    }
}

/// A permission change counts only when it grants or removes execute, or rewrites a code region.
/// Querying first keeps a data mprotect (heap, stack, read-only) from looking like a new module.
bool CountsAsCodeChange(KProcess& process, u64 address, u64 size, MemoryPermission perm) {
    if (!Core::NvnCensus::IsActive()) {
        return false; // only the census reads the generation: no page-table walk when it is off
    }
    if (perm == MemoryPermission::ReadExecute || perm == MemoryPermission::Execute) {
        return true;
    }
    auto& page_table = process.GetPageTable();
    u64 cursor = address;
    const u64 end = address + size;
    while (cursor < end) {
        KMemoryInfo info{};
        PageInfo page_info{};
        if (R_FAILED(page_table.QueryInfo(&info, &page_info, cursor))) {
            return true;
        }
        const auto state = info.GetSvcState();
        const bool code_region = state == MemoryState::Code || state == MemoryState::AliasCode ||
                                 state == MemoryState::SharedCode || state == MemoryState::GeneratedCode;
        const auto old_bits = static_cast<u32>(info.GetSvcMemoryInfo().permission);
        const bool was_executable = (old_bits & static_cast<u32>(MemoryPermission::Execute)) != 0;
        if (code_region || was_executable) {
            return true;
        }
        const u64 next = info.GetEndAddress();
        if (next <= cursor) {
            return true;
        }
        cursor = next;
    }
    return false;
}

} // namespace

Result SetProcessMemoryPermission(Core::System& system, Handle process_handle, u64 address,
                                  u64 size, Svc::MemoryPermission perm) {
    LOG_TRACE(Kernel_SVC,
              "called, process_handle={:#x}, addr={:#x}, size={:#x}, permissions={:#08x}",
              process_handle, address, size, perm);

    // Validate the address/size.
    R_UNLESS(Common::IsAligned(address, PageSize), ResultInvalidAddress);
    R_UNLESS(Common::IsAligned(size, PageSize), ResultInvalidSize);
    R_UNLESS(size > 0, ResultInvalidSize);
    R_UNLESS((address < address + size), ResultInvalidCurrentMemory);
    R_UNLESS(address == static_cast<uint64_t>(address), ResultInvalidCurrentMemory);
    R_UNLESS(size == static_cast<uint64_t>(size), ResultInvalidCurrentMemory);

    // Validate the memory permission.
    R_UNLESS(IsValidProcessMemoryPermission(perm), ResultInvalidNewMemoryPermission);

    // Get the process from its handle.
    KScopedAutoObject process =
        GetCurrentProcess(system.Kernel()).GetHandleTable().GetObject<KProcess>(system.Kernel(), process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    // Validate that the address is in range.
    auto& page_table = process->GetPageTable();
    R_UNLESS(page_table.Contains(address, size), ResultInvalidCurrentMemory);

    // Set the memory permission. Sample the old region first: a later query would see the new bits.
    const bool code_change = CountsAsCodeChange(*process, address, size, perm);
    const Result result = page_table.SetProcessMemoryPermission(address, size, perm);
    if (R_SUCCEEDED(result) && code_change) {
        process->NoteCodeChange();
    }
    R_RETURN(result);
}

Result MapProcessMemory(Core::System& system, u64 dst_address, Handle process_handle,
                        u64 src_address, u64 size) {
    LOG_TRACE(Kernel_SVC,
              "called, dst_address={:#x}, process_handle={:#x}, src_address={:#x}, size={:#x}",
              dst_address, process_handle, src_address, size);

    // Validate the address/size.
    R_UNLESS(Common::IsAligned(dst_address, PageSize), ResultInvalidAddress);
    R_UNLESS(Common::IsAligned(src_address, PageSize), ResultInvalidAddress);
    R_UNLESS(Common::IsAligned(size, PageSize), ResultInvalidSize);
    R_UNLESS(size > 0, ResultInvalidSize);
    R_UNLESS((dst_address < dst_address + size), ResultInvalidCurrentMemory);
    R_UNLESS((src_address < src_address + size), ResultInvalidCurrentMemory);

    // Get the processes.
    KProcess* dst_process = GetCurrentProcessPointer(system.Kernel());
    KScopedAutoObject src_process =
        dst_process->GetHandleTable().GetObjectWithoutPseudoHandle<KProcess>(system.Kernel(), process_handle);
    R_UNLESS(src_process.IsNotNull(), ResultInvalidHandle);

    // Get the page tables.
    auto& dst_pt = dst_process->GetPageTable();
    auto& src_pt = src_process->GetPageTable();

    // Validate that the mapping is in range.
    R_UNLESS(src_pt.Contains(src_address, size), ResultInvalidCurrentMemory);
    R_UNLESS(dst_pt.CanContain(dst_address, size, KMemoryState::SharedCode),
             ResultInvalidMemoryRegion);

    // Create a new page group.
    KPageGroup pg{system.Kernel(), dst_pt.GetBlockInfoManager()};
    R_TRY(src_pt.MakeAndOpenPageGroup(
        std::addressof(pg), src_address, size / PageSize, KMemoryState::FlagCanMapProcess,
        KMemoryState::FlagCanMapProcess, KMemoryPermission::None, KMemoryPermission::None,
        KMemoryAttribute::All, KMemoryAttribute::None));

    // Map the group.
    const Result mapped = dst_pt.MapPageGroup(dst_address, pg, KMemoryState::SharedCode,
                                              KMemoryPermission::UserReadWrite);
    if (R_SUCCEEDED(mapped)) {
        dst_process->NoteCodeChange();
    }
    R_RETURN(mapped);
}

Result UnmapProcessMemory(Core::System& system, u64 dst_address, Handle process_handle,
                          u64 src_address, u64 size) {
    LOG_TRACE(Kernel_SVC,
              "called, dst_address={:#x}, process_handle={:#x}, src_address={:#x}, size={:#x}",
              dst_address, process_handle, src_address, size);

    // Validate the address/size.
    R_UNLESS(Common::IsAligned(dst_address, PageSize), ResultInvalidAddress);
    R_UNLESS(Common::IsAligned(src_address, PageSize), ResultInvalidAddress);
    R_UNLESS(Common::IsAligned(size, PageSize), ResultInvalidSize);
    R_UNLESS(size > 0, ResultInvalidSize);
    R_UNLESS((dst_address < dst_address + size), ResultInvalidCurrentMemory);
    R_UNLESS((src_address < src_address + size), ResultInvalidCurrentMemory);

    // Get the processes.
    KProcess* dst_process = GetCurrentProcessPointer(system.Kernel());
    KScopedAutoObject src_process =
        dst_process->GetHandleTable().GetObjectWithoutPseudoHandle<KProcess>(system.Kernel(), process_handle);
    R_UNLESS(src_process.IsNotNull(), ResultInvalidHandle);

    // Get the page tables.
    auto& dst_pt = dst_process->GetPageTable();
    auto& src_pt = src_process->GetPageTable();

    // Validate that the mapping is in range.
    R_UNLESS(src_pt.Contains(src_address, size), ResultInvalidCurrentMemory);
    R_UNLESS(dst_pt.CanContain(dst_address, size, KMemoryState::SharedCode),
             ResultInvalidMemoryRegion);

    // Unmap the memory.
    const Result unmapped = dst_pt.UnmapProcessMemory(dst_address, size, src_pt, src_address);
    if (R_SUCCEEDED(unmapped)) {
        dst_process->NoteCodeChange();
    }
    R_RETURN(unmapped);
}

Result MapProcessCodeMemory(Core::System& system, Handle process_handle, u64 dst_address,
                            u64 src_address, u64 size) {
    LOG_DEBUG(Kernel_SVC,
              "called. process_handle={:#08x}, dst_address={:#016x}, "
              "src_address={:#016x}, size={:#016x}",
              process_handle, dst_address, src_address, size);

    if (!Common::IsAligned(src_address, Core::Memory::YUZU_PAGESIZE)) {
        LOG_ERROR(Kernel_SVC, "src_address is not page-aligned (src_address={:#016X}).",
                  src_address);
        R_THROW(ResultInvalidAddress);
    }

    if (!Common::IsAligned(dst_address, Core::Memory::YUZU_PAGESIZE)) {
        LOG_ERROR(Kernel_SVC, "dst_address is not page-aligned (dst_address={:#016X}).",
                  dst_address);
        R_THROW(ResultInvalidAddress);
    }

    if (size == 0 || !Common::IsAligned(size, Core::Memory::YUZU_PAGESIZE)) {
        LOG_ERROR(Kernel_SVC, "Size is zero or not page-aligned (size={:#016X})", size);
        R_THROW(ResultInvalidSize);
    }

    if (!IsValidAddressRange(dst_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Destination address range overflows the address space (dst_address={:#016x}, "
                  "size={:#016x}).",
                  dst_address, size);
        R_THROW(ResultInvalidCurrentMemory);
    }

    if (!IsValidAddressRange(src_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Source address range overflows the address space (src_address={:#016x}, "
                  "size={:#016x}).",
                  src_address, size);
        R_THROW(ResultInvalidCurrentMemory);
    }

    const auto& handle_table = GetCurrentProcess(system.Kernel()).GetHandleTable();
    KScopedAutoObject process = handle_table.GetObject<KProcess>(system.Kernel(), process_handle);
    if (process.IsNull()) {
        LOG_ERROR(Kernel_SVC, "Invalid process handle specified (handle={:#08x}).",
                  process_handle);
        R_THROW(ResultInvalidHandle);
    }

    auto& page_table = process->GetPageTable();
    if (!page_table.Contains(src_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Source address range is not within the address space (src_address={:#016x}, "
                  "size={:#016x}).",
                  src_address, size);
        R_THROW(ResultInvalidCurrentMemory);
    }

    R_UNLESS(page_table.CanContain(dst_address, size, KMemoryState::AliasCode), ResultInvalidCurrentMemory);
    const Result mapped = page_table.MapCodeMemory(dst_address, src_address, size);
    if (R_SUCCEEDED(mapped)) {
        process->NoteCodeChange();
    }
    R_RETURN(mapped);
}

Result UnmapProcessCodeMemory(Core::System& system, Handle process_handle, u64 dst_address,
                              u64 src_address, u64 size) {
    LOG_DEBUG(Kernel_SVC,
              "called. process_handle={:#08x}, dst_address={:#016x}, src_address={:#016x}, "
              "size={:#016x}",
              process_handle, dst_address, src_address, size);

    if (!Common::IsAligned(dst_address, Core::Memory::YUZU_PAGESIZE)) {
        LOG_ERROR(Kernel_SVC, "dst_address is not page-aligned (dst_address={:#016X}).",
                  dst_address);
        R_THROW(ResultInvalidAddress);
    }

    if (!Common::IsAligned(src_address, Core::Memory::YUZU_PAGESIZE)) {
        LOG_ERROR(Kernel_SVC, "src_address is not page-aligned (src_address={:#016X}).",
                  src_address);
        R_THROW(ResultInvalidAddress);
    }

    if (size == 0 || !Common::IsAligned(size, Core::Memory::YUZU_PAGESIZE)) {
        LOG_ERROR(Kernel_SVC, "Size is zero or not page-aligned (size={:#016X}).", size);
        R_THROW(ResultInvalidSize);
    }

    if (!IsValidAddressRange(dst_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Destination address range overflows the address space (dst_address={:#016x}, "
                  "size={:#016x}).",
                  dst_address, size);
        R_THROW(ResultInvalidCurrentMemory);
    }

    if (!IsValidAddressRange(src_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Source address range overflows the address space (src_address={:#016x}, "
                  "size={:#016x}).",
                  src_address, size);
        R_THROW(ResultInvalidCurrentMemory);
    }

    const auto& handle_table = GetCurrentProcess(system.Kernel()).GetHandleTable();
    KScopedAutoObject process = handle_table.GetObject<KProcess>(system.Kernel(), process_handle);
    if (process.IsNull()) {
        LOG_ERROR(Kernel_SVC, "Invalid process handle specified (handle={:#08x}).",
                  process_handle);
        R_THROW(ResultInvalidHandle);
    }

    auto& page_table = process->GetPageTable();
    if (!page_table.Contains(src_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Source address range is not within the address space (src_address={:#016x}, "
                  "size={:#016x}).",
                  src_address, size);
        R_THROW(ResultInvalidCurrentMemory);
    }

    R_UNLESS(page_table.CanContain(dst_address, size, KMemoryState::AliasCode), ResultInvalidCurrentMemory);
    const Result unmapped = page_table.UnmapCodeMemory(dst_address, src_address, size);
    if (R_SUCCEEDED(unmapped)) {
        process->NoteCodeChange();
    }
    R_RETURN(unmapped);
}

Result SetProcessMemoryPermission64(Core::System& system, Handle process_handle, uint64_t address,
                                    uint64_t size, MemoryPermission perm) {
    R_RETURN(SetProcessMemoryPermission(system, process_handle, address, size, perm));
}

Result MapProcessMemory64(Core::System& system, uint64_t dst_address, Handle process_handle,
                          uint64_t src_address, uint64_t size) {
    R_RETURN(MapProcessMemory(system, dst_address, process_handle, src_address, size));
}

Result UnmapProcessMemory64(Core::System& system, uint64_t dst_address, Handle process_handle,
                            uint64_t src_address, uint64_t size) {
    R_RETURN(UnmapProcessMemory(system, dst_address, process_handle, src_address, size));
}

Result MapProcessCodeMemory64(Core::System& system, Handle process_handle, uint64_t dst_address,
                              uint64_t src_address, uint64_t size) {
    R_RETURN(MapProcessCodeMemory(system, process_handle, dst_address, src_address, size));
}

Result UnmapProcessCodeMemory64(Core::System& system, Handle process_handle, uint64_t dst_address,
                                uint64_t src_address, uint64_t size) {
    R_RETURN(UnmapProcessCodeMemory(system, process_handle, dst_address, src_address, size));
}

Result SetProcessMemoryPermission64From32(Core::System& system, Handle process_handle,
                                          uint64_t address, uint64_t size, MemoryPermission perm) {
    R_RETURN(SetProcessMemoryPermission(system, process_handle, address, size, perm));
}

Result MapProcessMemory64From32(Core::System& system, uint32_t dst_address, Handle process_handle,
                                uint64_t src_address, uint32_t size) {
    R_RETURN(MapProcessMemory(system, dst_address, process_handle, src_address, size));
}

Result UnmapProcessMemory64From32(Core::System& system, uint32_t dst_address, Handle process_handle,
                                  uint64_t src_address, uint32_t size) {
    R_RETURN(UnmapProcessMemory(system, dst_address, process_handle, src_address, size));
}

Result MapProcessCodeMemory64From32(Core::System& system, Handle process_handle,
                                    uint64_t dst_address, uint64_t src_address, uint64_t size) {
    R_RETURN(MapProcessCodeMemory(system, process_handle, dst_address, src_address, size));
}

Result UnmapProcessCodeMemory64From32(Core::System& system, Handle process_handle,
                                      uint64_t dst_address, uint64_t src_address, uint64_t size) {
    R_RETURN(UnmapProcessCodeMemory(system, process_handle, dst_address, src_address, size));
}

} // namespace Kernel::Svc
