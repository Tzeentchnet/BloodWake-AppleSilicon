#ifndef BW_POSIX_GUEST_MEM_H
#define BW_POSIX_GUEST_MEM_H

#include <stddef.h>
#include <stdint.h>

typedef struct BwPosixGuestMem {
    uintptr_t base;
    size_t low_size;
    uintptr_t contig_base;
    uint32_t contig_va;
    size_t contig_size;
} BwPosixGuestMem;

int bw_posix_guest_read(const BwPosixGuestMem *mem, uint32_t va,
                        void *out, size_t size);
int bw_posix_guest_write(const BwPosixGuestMem *mem, uint32_t va,
                         const void *data, size_t size);

#endif
