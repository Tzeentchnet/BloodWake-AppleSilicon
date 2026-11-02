/* bw_gpu_audit.h - what the game asks of the graphics chip, counted
 * (BW_GPU_AUDIT=1). See bw_gpu_audit.c. */
#ifndef BW_GPU_AUDIT_H
#define BW_GPU_AUDIT_H

#include <stdint.h>

/* One draw (SET_BEGIN_END end), as the executor saw and routed it. */
typedef struct {
    const uint32_t *reg;        /* the executor's NV097 register mirror (2048 words) */
    uint32_t prim;              /* NV097 primitive, 1 points .. 10 polygon */
    uint32_t count;             /* indices / vertices in the batch */
    char path;                  /* G vertex program on the GPU, C program on the CPU,
                                   F fixed function, S screen space, U untransformed
                                   (dropped), ? not drawn (fewer than 3, points, lines) */
    char source;                /* A arrays (indexed or DRAW_ARRAYS), I inline, M immediate */
    const uint32_t (*prog)[4];  /* the vertex program from its start slot, or NULL */
    uint32_t prog_len;          /* instructions to FINAL inclusive */
    uint32_t prog_start;
    const uint32_t *vattr_fmt;  /* 16 SET_VERTEX_DATA_ARRAY_FORMAT words (mirror) */
    uint32_t aa;                /* surface AA mode (SURFACE_FORMAT bits 12-15) */
    uint32_t color_offset;      /* the colour surface this draw renders into */
} BwAuditDraw;

enum {
    BW_AUDIT_OCEAN_FINGERPRINT_VERSION = 1,
    BW_AUDIT_OCEAN_FINGERPRINT_SLOTS = 32
};

int  bw_audit_on(void);
void bw_audit_method(uint32_t subch, uint32_t method, uint32_t param);
void bw_audit_draw(const BwAuditDraw *d);
/* Stable metadata fingerprints for explicitly marked ocean batches. The
 * marker is the classifier; a hash is diagnostic evidence only.
 * scope_mismatch says this hash differs from the first one in its BEGIN/END. */
void bw_audit_ocean_fingerprint(uint64_t fingerprint, int scope_mismatch);
/* CLEAR_SURFACE: flags, the surface clip (w, h) and the clear rectangle
 * (x0, y0, x1, y1 exclusive; NULL if the title never set one). */
void bw_audit_clear(uint32_t flags, uint32_t surf_w, uint32_t surf_h, const uint32_t *rect);
/* FLIP_STALL: the surface presented (guest colour offset) and its format. */
void bw_audit_flip(uint32_t offset, uint32_t surface_format);
/* Display-chain events the executor never sees (D3D's DAC gamma ramp). */
void bw_audit_gamma(const uint8_t ramp[768]);

#endif
