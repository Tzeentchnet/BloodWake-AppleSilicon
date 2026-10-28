/* bw_vsh_hlsl.h - HLSL vertex shaders from NV2A vertex programs.
 * See bw_vsh_hlsl.c. */
#ifndef BW_VSH_HLSL_H
#define BW_VSH_HLSL_H

#include <stddef.h>
#include <stdint.h>

/* The second constant buffer the generated shader reads (register b1).
 * K is the program's c[0..191]. */
typedef struct {
    float k[192][4];
    float vsinfo[4];    /* aa scale x, aa scale y, near-plane clip on, - */
    float foginfo[4];   /* fog enabled, fog mode (0x800..), param 0, param 1 */
} BwVshConsts;

/* Write the vertex shader (entry point vs_main) for the `n` instructions at
 * `prog` (the last one FINAL). The inputs in `input_mask` arrive in
 * ascending order as float4 attributes ATTR0, ATTR1, ... Returns the length,
 * or 0 if it did not fit. */
/* The geometry shader (entry point gs_main) that drops triangles with a
 * non-finite vertex, as the CPU path's clipper does. */
extern const char bw_vsh_gs_hlsl[];

/* conv (Blood Wake P1; NULL = all gathered floats): per input in that order,
 * its BwXVsInput.conv (bw_backend_ext.h). The integer formats then arrive
 * raw as ATTRk and the shader converts them as fetch_attr does. */
size_t bw_vsh_hlsl(const uint32_t (*prog)[4], uint32_t n, uint32_t input_mask,
                   const uint8_t *conv, char *out, size_t cap);

#endif
