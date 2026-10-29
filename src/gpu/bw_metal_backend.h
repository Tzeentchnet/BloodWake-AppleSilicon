#ifndef BW_METAL_BACKEND_H
#define BW_METAL_BACKEND_H

#include <stddef.h>

/* Synthetic harness only. All calls and executor callbacks use one thread.
 * The memory must remain valid until stop and match xbox_GetMemoryOffset(). */
int bw_metal_backend_start_for_test(void *guest_memory, size_t guest_size);
const char *bw_metal_backend_error(void);
void bw_metal_backend_stop(void);

#endif
