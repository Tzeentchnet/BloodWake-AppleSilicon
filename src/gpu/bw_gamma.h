/* bw_gamma.h - the title's DAC gamma ramp. See bw_gamma.c. */
#ifndef BW_GAMMA_H
#define BW_GAMMA_H

#include <stddef.h>
#include <stdint.h>

/* From the D3DDevice_SetGammaRamp hook: 768 bytes, red, green, blue planes. */
void bw_gamma_from_title(const uint8_t *ramp);
/* The newest ramp into out; returns its sequence number (0: none yet). */
uint32_t bw_gamma_get(uint8_t out[768]);
/* BW_GPU_GAMMA (default on): whether the picture goes through the ramp. */
int bw_gamma_display_on(void);
/* The ramp to apply now: 1 and lut filled if on and not identity, else 0. */
int bw_gamma_lut(uint8_t lut[768]);
/* Apply it in place to n pixels 0x??RRGGBB (no-op when off or identity). */
void bw_gamma_apply_xrgb(uint32_t *px, size_t n);

#endif
