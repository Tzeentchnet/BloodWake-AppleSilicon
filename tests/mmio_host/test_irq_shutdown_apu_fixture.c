/* Expose the actual static handler only to the synthetic counterexample. */
#include "../../src/toolkit_patches/apu/apu_core.c"

void probe_register_real_apu_isr(void)
{
    xbox_IrqSetHandler(XBOX_IRQ_APU, apu_isr_on_holder);
}

int probe_shutdown_started(MCPXAPUState *d)
{
    int started;
    bw_apu_mutex_lock(&g_instance_lock);
    started = g_state != d && g_instance_busy;
    bw_apu_mutex_unlock(&g_instance_lock);
    return started;
}
