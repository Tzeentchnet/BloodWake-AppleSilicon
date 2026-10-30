/* Keep the synthetic interrupt connection while using the real DPC bridge. */
#define xbox_GetConnectedInterrupt bw_irq_probe_unused_connected_interrupt
#include "../../src/toolkit_patches/kernel/kernel_bridge.c"
