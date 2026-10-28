/*
 * bw_gamma.c - the gamma ramp Blood Wake gives the video DAC.
 *
 * On the console, D3DDevice_SetGammaRamp (0x00124F80) copies a 3 x 256-byte
 * ramp (red, green, blue planes) into the miniport and D3D_WriteGammaRampToDac
 * (0x0012A014) writes it to the DAC palette (NV_USER_DAC 0x6813C8/9), which
 * the video output applies to every scanned-out pixel. The toolkit's NV2A
 * registers are plain memory, so those byte writes land on one address and
 * the ramp is lost; the port showed every frame as if the ramp were identity.
 *
 * Blood Wake uses the ramp for more than calibration (seen in code,
 * 2026-10-27): Render_SetWorldStates (0x0002C740) calls Graphics_SetGamma
 * (0x00037340) every 3D frame with the options gamma slider ([0x1E7B30]) and
 * a scale ([0x1A9AAC]) that Game_InitLevel (0x00090EA0) sets to 0 and
 * Game_Update (0x00091380) moves toward a target over time: the missions
 * fade in from black through the DAC, and the gamma slider works the same
 * way. The ramp is pow(i/255, 1/g) * scale.
 *
 * The D-011 hook on 0x00124F80 (src/recomp_manual.c) hands each ramp here;
 * the GPU back end's present applies the newest one (bw_d3d11.c, BW_GPU_GAMMA).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_gamma.h"
#include "bw_gpu_audit.h"

int bw_ab_off(const char *fix);      /* BW_GPU_AB (bw_ab.c) */

static uint8_t s_ramp[2][768];
static volatile LONG s_seq;

void bw_gamma_from_title(const uint8_t *ramp)
{
    /* The game's thread writes the slot the reader is not on; the reader
     * takes whichever the sequence number names (a torn read can only mix
     * two of the game's own ramps, one frame, and never happens at the
     * game's once-per-frame rate). */
    LONG next = s_seq + 1;
    memcpy(s_ramp[next & 1], ramp, 768);
    InterlockedExchange(&s_seq, next);
    if (bw_audit_on()) {
        bw_audit_gamma(ramp);
        /* The ramp's timeline: entries 64, 128 and 255 of red, green, blue. */
        fprintf(stderr, "[GPU-AUDIT] gamma ramp %ld at %llu ms: R %u %u %u G %u %u %u B %u %u %u\n",
                (long)next, (unsigned long long)GetTickCount64(), ramp[64], ramp[128], ramp[255],
                ramp[256 + 64], ramp[256 + 128], ramp[256 + 255], ramp[512 + 64], ramp[512 + 128],
                ramp[512 + 255]);
    }
}

uint32_t bw_gamma_get(uint8_t out[768])
{
    LONG seq = s_seq;
    if (!seq)
        return 0;
    memcpy(out, s_ramp[seq & 1], 768);
    return (uint32_t)seq;
}

/* BW_GPU_GAMMA=0: show frames without the ramp, as before 2026-10-27. On by
 * default: the ramp is part of what the console showed (a port fix, every
 * edition). */
int bw_gamma_display_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_GPU_GAMMA");
        on = !(e && e[0] == '0');
        fprintf(stderr, "[BW] DAC gamma ramp on the picture: %s\n",
                on ? "on (BW_GPU_GAMMA=0 turns it off)" : "off (BW_GPU_GAMMA=0)");
    }
    return on;
}

/* The ramp to apply to the picture now, as 768 bytes (red, green, blue
 * planes): 1 if there is one and it is not identity, else 0. Called once per
 * presented or dumped frame, from the NV2A and window threads. */
static int ramp_now(uint8_t lut[768])
{
    LONG seq = s_seq;
    int i;
    if (!bw_gamma_display_on() || !seq || bw_ab_off("gamma"))
        return 0;
    memcpy(lut, s_ramp[seq & 1], 768);
    for (i = 0; i < 768; i++)
        if (lut[i] != (uint8_t)(i & 0xFF))
            return 1;
    return 0;
}

int bw_gamma_lut(uint8_t lut[768])
{
    return ramp_now(lut);
}

/* In place, n pixels 0x??RRGGBB (the top byte is kept). */
void bw_gamma_apply_xrgb(uint32_t *px, size_t n)
{
    uint8_t lut[768];
    size_t i;
    if (!ramp_now(lut))
        return;
    for (i = 0; i < n; i++) {
        uint32_t v = px[i];
        px[i] = (v & 0xFF000000u) | ((uint32_t)lut[(v >> 16) & 0xFF] << 16)
              | ((uint32_t)lut[256 + ((v >> 8) & 0xFF)] << 8) | lut[512 + (v & 0xFF)];
    }
}
