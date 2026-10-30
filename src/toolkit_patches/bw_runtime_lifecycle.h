#ifndef BW_RUNTIME_LIFECYCLE_H
#define BW_RUNTIME_LIFECYCLE_H

#include <stddef.h>
#include <stdint.h>

/* Call after guest execution has stopped, before tearing down the backend or
 * guest memory. Return 0 only when the worker is quiescent; both are
 * idempotent and must be called from a thread other than the worker itself. */
int bw_xbox_ohci_stop(void);
int bw_xbox_nv2a_stop(void);

/* The actual contiguous-window mapping and its guest range, if mapped. */
int bw_xbox_contiguous_memory_region(void **host, uint32_t *guest_va, size_t *bytes);

#endif
