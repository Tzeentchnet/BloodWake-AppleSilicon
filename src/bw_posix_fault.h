#ifndef BW_POSIX_FAULT_H
#define BW_POSIX_FAULT_H

#include <stdint.h>

/* Handles the toolkit's signal-safe AC'97 alias and, when explicitly enabled,
 * defers trapped APU accesses onto guarded ordinary-thread stacks before
 * invoking model callbacks. Install before memory initialization;
 * clear/uninstall only after all guest/device threads stop. Installation
 * refuses incompatible existing handlers; a previous handler that nonlocally
 * exits instead of returning makes later faults fail closed. */
int bw_posix_fault_install(void);
void bw_posix_fault_set_base(uintptr_t base);
void bw_posix_fault_enable_apu(int enabled);
void bw_posix_fault_uninstall(void);

#endif
