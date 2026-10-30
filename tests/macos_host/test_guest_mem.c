#include "bw_posix_guest_mem.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct ProtectStress {
    void *page;
    size_t size;
} ProtectStress;

static void *change_protection(void *arg)
{
    ProtectStress *stress = arg;
    for (unsigned i = 0; i < 2000; i++) {
        int prot = i % 3 == 0 ? PROT_NONE : i % 3 == 1 ? PROT_READ
                                                          : PROT_READ | PROT_WRITE;
        if (mprotect(stress->page, stress->size, prot) != 0)
            return (void *)(uintptr_t)1;
    }
    return NULL;
}

int main(void)
{
    long page_long = sysconf(_SC_PAGESIZE);
    uint8_t *base;
    size_t page;
    BwPosixGuestMem mem;
    uint32_t value = 0, replacement = UINT32_C(0x87654321);
    pthread_t thread;
    void *result;

    CHECK(page_long > 0);
    page = (size_t)page_long;
    base = mmap(NULL, page * 4, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(base != MAP_FAILED);
    mem = (BwPosixGuestMem){(uintptr_t)base, page * 2,
                            (uintptr_t)(base + page * 3),
                            (uint32_t)(page * 3), page};
    memcpy(base + page, &replacement, sizeof replacement);
    CHECK(bw_posix_guest_read(&mem, (uint32_t)page, &value, sizeof value));
    CHECK(value == replacement);
    value = UINT32_C(0x12345678);
    CHECK(bw_posix_guest_write(&mem, (uint32_t)page, &value, sizeof value));
    CHECK(!memcmp(base + page, &value, sizeof value));

    CHECK(!bw_posix_guest_read(&mem, 0, &value, sizeof value));
    CHECK(!bw_posix_guest_read(&mem, UINT32_MAX - 1, &value, sizeof value));
    CHECK(!bw_posix_guest_read(&mem, (uint32_t)(page * 2 - 2), &value, sizeof value));
    CHECK(!bw_posix_guest_read(&mem, (uint32_t)(page * 2), &value, sizeof value));
    CHECK(bw_posix_guest_write(&mem, (uint32_t)(page * 3), &replacement, sizeof replacement));
    CHECK(bw_posix_guest_read(&mem, (uint32_t)(page * 3), &value, sizeof value));
    CHECK(value == replacement);
    mem.contig_base++;
    CHECK(!bw_posix_guest_read(&mem, (uint32_t)(page * 3), &value, sizeof value));
    mem.contig_base--;
    mem.contig_size = SIZE_MAX;
    CHECK(!bw_posix_guest_read(&mem, (uint32_t)(page * 3), &value, sizeof value));
    mem.contig_size = page;

    CHECK(mprotect(base + page, page, PROT_READ) == 0);
    CHECK(bw_posix_guest_read(&mem, (uint32_t)page, &value, sizeof value));
    CHECK(!bw_posix_guest_write(&mem, (uint32_t)page, &replacement, sizeof replacement));
    CHECK(mprotect(base + page, page, PROT_NONE) == 0);
    CHECK(!bw_posix_guest_read(&mem, (uint32_t)page, &value, sizeof value));
    CHECK(!bw_posix_guest_write(&mem, (uint32_t)page, &replacement, sizeof replacement));

    ProtectStress stress = {base + page, page};
    CHECK(pthread_create(&thread, NULL, change_protection, &stress) == 0);
    for (unsigned i = 0; i < 2000; i++) {
        (void)bw_posix_guest_read(&mem, (uint32_t)page, &value, sizeof value);
        (void)bw_posix_guest_write(&mem, (uint32_t)page, &replacement, sizeof replacement);
    }
    CHECK(pthread_join(thread, &result) == 0);
    CHECK(!result);
    CHECK(munmap(base, page * 4) == 0);
    return 0;
}
