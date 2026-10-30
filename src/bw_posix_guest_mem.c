#include "bw_posix_guest_mem.h"

#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

static int guest_address(const BwPosixGuestMem *mem, uint32_t va, size_t size,
                         mach_vm_address_t *host)
{
    uint64_t end;
    uintptr_t address;

    if (!mem || !host || !mem->base || va < 0x1000u ||
        !size || size > UINT32_MAX || (uint64_t)size > UINT64_C(0x100000000) - va)
        return 0;
    end = (uint64_t)va + size;
    if (mem->base > UINTPTR_MAX - va)
        return 0;
    address = mem->base + va;
    if (address > UINTPTR_MAX - (size - 1))
        return 0;
    if (end > mem->low_size) {
        uint64_t contig_end;
        if (!mem->contig_base || mem->contig_va > UINTPTR_MAX - mem->base ||
            mem->contig_base != mem->base + mem->contig_va ||
            (uint64_t)mem->contig_size > UINT64_C(0x100000000) - mem->contig_va)
            return 0;
        contig_end = (uint64_t)mem->contig_va + mem->contig_size;
        if (va < mem->contig_va || end > contig_end)
            return 0;
    }
    *host = (mach_vm_address_t)address;
    return 1;
}

int bw_posix_guest_read(const BwPosixGuestMem *mem, uint32_t va,
                        void *out, size_t size)
{
    mach_vm_address_t host;
    mach_vm_size_t copied = 0;

    if (!out || !guest_address(mem, va, size, &host))
        return 0;
    /* A mapped-range check alone cannot prove current page protections. */
    return mach_vm_read_overwrite(mach_task_self(), host, size,
                                  (mach_vm_address_t)(uintptr_t)out,
                                  &copied) == KERN_SUCCESS && copied == size;
}

int bw_posix_guest_write(const BwPosixGuestMem *mem, uint32_t va,
                         const void *data, size_t size)
{
    mach_vm_address_t host;

    if (!data || !guest_address(mem, va, size, &host))
        return 0;
    return mach_vm_write(mach_task_self(), host, (vm_offset_t)(uintptr_t)data,
                         (mach_msg_type_number_t)size) == KERN_SUCCESS;
}
