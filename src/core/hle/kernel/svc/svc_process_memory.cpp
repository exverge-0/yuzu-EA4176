// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/svc.h"

namespace Kernel::Svc {
namespace {

constexpr bool IsValidAddressRange(VAddr address, u64 size) {
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

} // namespace

Result SetProcessMemoryPermission(Core::System& system, Handle process_handle, VAddr address,
                                  u64 size, Svc::MemoryPermission perm) {
    LOG_TRACE(Kernel_SVC,
              "called, process_handle=0x{:X}, addr=0x{:X}, size=0x{:X}, permissions=0x{:08X}",
              process_handle, address, size, perm);

    // Validate the address/size.
    R_UNLESS(Common::IsAligned(address, PageSize), ResultInvalidAddress);
    R_UNLESS(Common::IsAligned(size, PageSize), ResultInvalidSize);
    R_UNLESS(size > 0, ResultInvalidSize);
    R_UNLESS((address < address + size), ResultInvalidCurrentMemory);
    R_UNLESS(address == static_cast<uintptr_t>(address), ResultInvalidCurrentMemory);
    R_UNLESS(size == static_cast<size_t>(size), ResultInvalidCurrentMemory);

    // Validate the memory permission.
    R_UNLESS(IsValidProcessMemoryPermission(perm), ResultInvalidNewMemoryPermission);

    // Get the process from its handle.
    KScopedAutoObject process =
        system.CurrentProcess()->GetHandleTable().GetObject<KProcess>(process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    // Validate that the address is in range.
    auto& page_table = process->PageTable();
    R_UNLESS(page_table.Contains(address, size), ResultInvalidCurrentMemory);

    // Set the memory permission.
    return page_table.SetProcessMemoryPermission(address, size, perm);
}

Result MapProcessMemory(Core::System& system, VAddr dst_address, Handle process_handle,
                        VAddr src_address, u64 size) {
    LOG_TRACE(Kernel_SVC,
              "called, dst_address=0x{:X}, process_handle=0x{:X}, src_address=0x{:X}, size=0x{:X}",
              dst_address, process_handle, src_address, size);

    // Validate the address/size.
    R_UNLESS(Common::IsAligned(dst_address, PageSize), ResultInvalidAddress);
    R_UNLESS(Common::IsAligned(src_address, PageSize), ResultInvalidAddress);
    R_UNLESS(Common::IsAligned(size, PageSize), ResultInvalidSize);
    R_UNLESS(size > 0, ResultInvalidSize);
    R_UNLESS((dst_address < dst_address + size), ResultInvalidCurrentMemory);
    R_UNLESS((src_address < src_address + size), ResultInvalidCurrentMemory);

    // Get the processes.
    KProcess* dst_process = system.CurrentProcess();
    KScopedAutoObject src_process =
        dst_process->GetHandleTable().GetObjectWithoutPseudoHandle<KProcess>(process_handle);
    R_UNLESS(src_process.IsNotNull(), ResultInvalidHandle);

    // Get the page tables.
    auto& dst_pt = dst_process->PageTable();
    auto& src_pt = src_process->PageTable();

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
    R_TRY(dst_pt.MapPageGroup(dst_address, pg, KMemoryState::SharedCode,
                              KMemoryPermission::UserReadWrite));

    return ResultSuccess;
}

Result UnmapProcessMemory(Core::System& system, VAddr dst_address, Handle process_handle,
                          VAddr src_address, u64 size) {
    LOG_TRACE(Kernel_SVC,
              "called, dst_address=0x{:X}, process_handle=0x{:X}, src_address=0x{:X}, size=0x{:X}",
              dst_address, process_handle, src_address, size);

    // Validate the address/size.
    R_UNLESS(Common::IsAligned(dst_address, PageSize), ResultInvalidAddress);
    R_UNLESS(Common::IsAligned(src_address, PageSize), ResultInvalidAddress);
    R_UNLESS(Common::IsAligned(size, PageSize), ResultInvalidSize);
    R_UNLESS(size > 0, ResultInvalidSize);
    R_UNLESS((dst_address < dst_address + size), ResultInvalidCurrentMemory);
    R_UNLESS((src_address < src_address + size), ResultInvalidCurrentMemory);

    // Get the processes.
    KProcess* dst_process = system.CurrentProcess();
    KScopedAutoObject src_process =
        dst_process->GetHandleTable().GetObjectWithoutPseudoHandle<KProcess>(process_handle);
    R_UNLESS(src_process.IsNotNull(), ResultInvalidHandle);

    // Get the page tables.
    auto& dst_pt = dst_process->PageTable();
    auto& src_pt = src_process->PageTable();

    // Validate that the mapping is in range.
    R_UNLESS(src_pt.Contains(src_address, size), ResultInvalidCurrentMemory);
    R_UNLESS(dst_pt.CanContain(dst_address, size, KMemoryState::SharedCode),
             ResultInvalidMemoryRegion);

    // Unmap the memory.
    R_TRY(dst_pt.UnmapProcessMemory(dst_address, size, src_pt, src_address));

    return ResultSuccess;
}

Result MapProcessCodeMemory(Core::System& system, Handle process_handle, u64 dst_address,
                            u64 src_address, u64 size) {
    LOG_DEBUG(Kernel_SVC,
              "called. process_handle=0x{:08X}, dst_address=0x{:016X}, "
              "src_address=0x{:016X}, size=0x{:016X}",
              process_handle, dst_address, src_address, size);

    if (!Common::Is4KBAligned(src_address)) {
        LOG_ERROR(Kernel_SVC, "src_address is not page-aligned (src_address=0x{:016X}).",
                  src_address);
        return ResultInvalidAddress;
    }

    if (!Common::Is4KBAligned(dst_address)) {
        LOG_ERROR(Kernel_SVC, "dst_address is not page-aligned (dst_address=0x{:016X}).",
                  dst_address);
        return ResultInvalidAddress;
    }

    if (size == 0 || !Common::Is4KBAligned(size)) {
        LOG_ERROR(Kernel_SVC, "Size is zero or not page-aligned (size=0x{:016X})", size);
        return ResultInvalidSize;
    }

    if (!IsValidAddressRange(dst_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Destination address range overflows the address space (dst_address=0x{:016X}, "
                  "size=0x{:016X}).",
                  dst_address, size);
        return ResultInvalidCurrentMemory;
    }

    if (!IsValidAddressRange(src_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Source address range overflows the address space (src_address=0x{:016X}, "
                  "size=0x{:016X}).",
                  src_address, size);
        return ResultInvalidCurrentMemory;
    }

    const auto& handle_table = system.Kernel().CurrentProcess()->GetHandleTable();
    KScopedAutoObject process = handle_table.GetObject<KProcess>(process_handle);
    if (process.IsNull()) {
        LOG_ERROR(Kernel_SVC, "Invalid process handle specified (handle=0x{:08X}).",
                  process_handle);
        return ResultInvalidHandle;
    }

    auto& page_table = process->PageTable();
    if (!page_table.IsInsideAddressSpace(src_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Source address range is not within the address space (src_address=0x{:016X}, "
                  "size=0x{:016X}).",
                  src_address, size);
        return ResultInvalidCurrentMemory;
    }

    if (!page_table.IsInsideASLRRegion(dst_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Destination address range is not within the ASLR region (dst_address=0x{:016X}, "
                  "size=0x{:016X}).",
                  dst_address, size);
        return ResultInvalidMemoryRegion;
    }

    return page_table.MapCodeMemory(dst_address, src_address, size);
}

Result UnmapProcessCodeMemory(Core::System& system, Handle process_handle, u64 dst_address,
                              u64 src_address, u64 size) {
    LOG_DEBUG(Kernel_SVC,
              "called. process_handle=0x{:08X}, dst_address=0x{:016X}, src_address=0x{:016X}, "
              "size=0x{:016X}",
              process_handle, dst_address, src_address, size);

    if (!Common::Is4KBAligned(dst_address)) {
        LOG_ERROR(Kernel_SVC, "dst_address is not page-aligned (dst_address=0x{:016X}).",
                  dst_address);
        return ResultInvalidAddress;
    }

    if (!Common::Is4KBAligned(src_address)) {
        LOG_ERROR(Kernel_SVC, "src_address is not page-aligned (src_address=0x{:016X}).",
                  src_address);
        return ResultInvalidAddress;
    }

    if (size == 0 || !Common::Is4KBAligned(size)) {
        LOG_ERROR(Kernel_SVC, "Size is zero or not page-aligned (size=0x{:016X}).", size);
        return ResultInvalidSize;
    }

    if (!IsValidAddressRange(dst_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Destination address range overflows the address space (dst_address=0x{:016X}, "
                  "size=0x{:016X}).",
                  dst_address, size);
        return ResultInvalidCurrentMemory;
    }

    if (!IsValidAddressRange(src_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Source address range overflows the address space (src_address=0x{:016X}, "
                  "size=0x{:016X}).",
                  src_address, size);
        return ResultInvalidCurrentMemory;
    }

    const auto& handle_table = system.Kernel().CurrentProcess()->GetHandleTable();
    KScopedAutoObject process = handle_table.GetObject<KProcess>(process_handle);
    if (process.IsNull()) {
        LOG_ERROR(Kernel_SVC, "Invalid process handle specified (handle=0x{:08X}).",
                  process_handle);
        return ResultInvalidHandle;
    }

    auto& page_table = process->PageTable();
    if (!page_table.IsInsideAddressSpace(src_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Source address range is not within the address space (src_address=0x{:016X}, "
                  "size=0x{:016X}).",
                  src_address, size);
        return ResultInvalidCurrentMemory;
    }

    if (!page_table.IsInsideASLRRegion(dst_address, size)) {
        LOG_ERROR(Kernel_SVC,
                  "Destination address range is not within the ASLR region (dst_address=0x{:016X}, "
                  "size=0x{:016X}).",
                  dst_address, size);
        return ResultInvalidMemoryRegion;
    }

    return page_table.UnmapCodeMemory(dst_address, src_address, size,
                                      KPageTable::ICacheInvalidationStrategy::InvalidateAll);
}

} // namespace Kernel::Svc