#ifndef BW_POSIX_KE_EVENT_H
#define BW_POSIX_KE_EVENT_H

#include "bw_posix_guest_mem.h"

#include <stddef.h>
#include <stdint.h>

typedef enum BwKeResult {
    BW_KE_OK = 0,
    BW_KE_NOT_EVENT,
    BW_KE_INACCESSIBLE,
    BW_KE_INVALID,
    BW_KE_EXHAUSTED,
    BW_KE_CHANGED,
    BW_KE_TIMEOUT,
    BW_KE_ALERTED
} BwKeResult;

typedef struct BwKeHeader {
    uint8_t type;
    uint8_t size;
    uint32_t state;
} BwKeHeader;

typedef enum BwKeOperation {
    BW_KE_SET,
    BW_KE_RESET,
    BW_KE_PULSE
} BwKeOperation;

typedef int (*BwKeAlertPoll)(void *context);

uint64_t bw_posix_ke_monotonic_ns(void);
uint64_t bw_posix_ke_deadline_ns(const int64_t *timeout);
BwKeResult bw_posix_ke_probe(const BwPosixGuestMem *mem, uint32_t va,
                             BwKeHeader *header);
BwKeResult bw_posix_ke_initialize(const BwPosixGuestMem *mem, uint32_t va,
                                  uint32_t type, uint32_t state);
void bw_posix_ke_invalidate(uint32_t va);
BwKeResult bw_posix_ke_modify(const BwPosixGuestMem *mem, uint32_t va,
                              BwKeOperation operation, uint32_t *previous);
BwKeResult bw_posix_ke_wait(const BwPosixGuestMem *mem, const uint32_t *objects,
                            size_t count, int wait_all, const int64_t *timeout,
                            BwKeAlertPoll alert_poll, void *alert_context,
                            uint32_t *selected);

#if defined(BW_KE_EVENT_TESTING)
unsigned bw_posix_ke_test_waiters(uint32_t va);
#endif

#endif
