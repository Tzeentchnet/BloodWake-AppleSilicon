/*
 * bw_backend_ext.h - Blood Wake extension of the toolkit's render back-end
 * interface (nv2a_backend.h), carried with the patched nv2a_pb_exec.c.
 *
 * The toolkit's interface hands a back end stage 0 and the diffuse colour, so
 * a GPU back end cannot reproduce the register combiners the CPU rasteriser
 * evaluates (nv2a_combiner.c): the water's sunset reflection, lit walls. For
 * transformed (vertex-program / fixed-function) batches this hands over
 * everything the CPU path's per-pixel loop reads instead:
 *   - per vertex: position (surface pixels, rhw), both colours, the fog
 *     factor after the fog unit, and all four texture coordinate sets;
 *   - per batch: all four texture stages, the stage programs, the combiner
 *     registers, the fog colour and the clip-plane mode.
 * Screen-space (pre-transformed) batches still use the toolkit's draw().
 */
#ifndef BW_BACKEND_EXT_H
#define BW_BACKEND_EXT_H

#include <stdint.h>
#include "nv2a_backend.h"
#include "nv2a_combiner.h"

typedef struct {
    float x, y, z, rhw;         /* surface pixels, z as produced, 1/w */
    float d0[4], d1[4];         /* diffuse, specular: r, g, b, a in [0,1] */
    float fog;                  /* fog factor (1 = no fog) */
    float pad[3];
    float tc[4][4];             /* texture coordinates, stages 0-3 (s, t, r, q) */
} BwXVertex;                    /* 32 floats */

typedef struct {
    const BwXVertex *vertices;  /* triangle list */
    uint32_t count;
    const Nv2aRenderState *state;
    const Nv2aCombiner *rc;
    Nv2aTexture tex[4];         /* valid only where tex_valid[i] */
    uint8_t tex_valid[4];
    uint8_t tex_normalized[4];  /* coordinates are 0..1 (swizzled, DXT); else texels */
    uint8_t tex_cube[4];
    float fogc[4];              /* fog colour r, g, b (a unused) */
    uint32_t clip_plane_mode;   /* SET_SHADER_CLIP_PLANE_MODE */
    uint32_t dot_rgbmapping;    /* SET_DOT_RGBMAPPING: stage i's input mapping in bits 4(i-1)..+3 */
    uint32_t other_stage_input; /* SET_SHADER_OTHER_STAGE_INPUT: stage 2's source in 16-19, 3's in 20-23 */
    uint32_t tex_border[4];     /* SET_TEXTURE_BORDER_COLOR (0xAARRGGBB) per stage */
    uint8_t tex_border_src[4];  /* FORMAT's border source: 1 = that colour, 0 = the texture's edge */
} BwXBatch;

/* Vertex programs on the GPU. Instead of running the title's vertex program
 * on the CPU once per vertex (nv2a_vsh_interp.c), the executor hands the
 * batch over untransformed: the program, the constants, each distinct
 * vertex's raw inputs as floats, and a triangle list indexing them. The back
 * end runs the program as a vertex shader with the interpreter's semantics,
 * then does what the executor does after it: w = +0 taken as 1, the
 * anti-aliasing scale, near-plane clipping and the fog unit. */
/* Blood Wake P1: GPU-side vertex fetch (BW_GPU_VFETCH). Instead of `inputs`
 * (each distinct vertex's attributes, read and converted to floats on the CPU
 * by fetch_attr), the batch's index range of each vertex stream as the
 * title's own bytes. The back end's input layout and shader prologue convert
 * each format exactly as fetch_attr does: floats are copied, the integer
 * formats go through a table the CPU filled with fetch_attr's expressions. */
#define BW_XVS_MAX_STREAMS 16
enum {                            /* BwXVsInput.conv >> 4; the low nibble is the size */
    BW_XVS_GATHERED = 0,          /* `inputs` (no streams) */
    BW_XVS_FLOAT = 1,             /* type 2, 1-4 floats */
    BW_XVS_COLOR = 2,             /* type 0, D3DCOLOR: bytes B,G,R,A -> r,g,b,a / 255 */
    BW_XVS_UBYTE = 3,             /* type 4, 1-4 bytes / 255 */
    BW_XVS_SHORT = 4,             /* type 5, 1-4 shorts as they are */
    BW_XVS_CMP = 5,               /* type 6, packed 11:11:10 normal */
    BW_XVS_CONST = 6              /* no array: the current value (4 floats, stride 0) */
};
typedef struct {
    const void *data;             /* the span: vertex `first` of the stream's lowest attribute */
    uint32_t bytes;
    uint32_t stride;              /* 0: one value for every vertex */
} BwXVsStream;
typedef struct {
    uint8_t stream;               /* index into streams */
    uint8_t conv;                 /* BW_XVS_* << 4 | components */
    uint16_t offset;              /* the attribute's byte offset in a vertex of its stream */
} BwXVsInput;

typedef struct {
    BwXBatch xb;                  /* everything except vertices and count */
    const uint32_t (*program)[4]; /* from the start slot ... */
    uint32_t ninstr;              /* ... to the FINAL instruction, inclusive */
    const float (*consts)[4];     /* c[0..191] */
    const float *inputs;          /* nverts x ninputs float4s; NULL with streams */
    uint32_t input_mask;          /* bit i: v<i> is read; ascending order */
    uint32_t ninputs, nverts;
    const uint32_t *indices;      /* triangle list into the vertices */
    uint32_t nindices;
    float aa[2];                  /* surface pixels per logical pixel */
    int near_clip;                /* also clip at the near plane */
    uint32_t fog_enable, fog_mode;
    float fog_param[2];
    /* Blood Wake P1: with nstreams > 0 the vertices are the streams' spans
     * (vertex i at data + i * stride), and in[k] says where input k is. */
    uint32_t nstreams;
    BwXVsStream streams[BW_XVS_MAX_STREAMS];
    BwXVsInput in[16];
    /* P1: bumped by every write to c[0..191] (nv2a_vsh_constants_gen); the
     * same value as the last batch's means `consts` did not change */
    uint32_t consts_gen;
} BwXVsBatch;

/* What the back end's vertex shader produced for one vertex, mapped back to
 * the executor's terms (pos as transform_vertex leaves it: surface pixels,
 * z as produced, w), for BW_GPU_VSH_VERIFY. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog, clip[2], pad;    /* fog factor; clip distances (w, near plane) */
    float tc[4][4];
} BwXVsOut;                     /* 32 floats */

typedef struct {
    void (*draw)(const Nv2aSurface *s, const BwXBatch *b);
    /* Optional. Returns 1 if it drew the batch, 0 to have the executor
     * transform it on the CPU instead. */
    int (*draw_vsh)(const Nv2aSurface *s, const BwXVsBatch *b);
    /* Optional, for checking: re-run the last draw_vsh batch's vertices
     * through its shader and read the results back (slow). Returns 1 on
     * success. */
    int (*vsh_readback)(BwXVsOut *out, uint32_t nverts);
    /* Optional (Blood Wake P1): 1 if the back end takes the next batch as
     * raw streams, 0 while it needs the gathered floats (it reads them on
     * the CPU). Without it, streams are never sent. */
    int (*vsh_raw_ok)(void);
} BwXBackend;

/* One instruction field, (dword, lowest bit, width), as nv2a_vsh_interp.c. */
static inline uint32_t bw_vsh_field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* The inputs a program reads (bit i for v<i>), counting only the sources
 * each op uses: A for any MAC op, B for mul, mad, dp3/dph/dp4, dst, min, max,
 * slt, sge; C for add, mad and every ILU op. All three share one input index.
 * The executor fetches exactly these and the shader reads them in this order,
 * so both sides call this. */
static inline uint32_t bw_vsh_inputs_used(const uint32_t (*p)[4], uint32_t n)
{
    uint32_t m = 0, i;
    for (i = 0; i < n; i++) {
        const uint32_t *ins = p[i];
        uint32_t mac = bw_vsh_field(ins, 1, 21, 4), ilu = bw_vsh_field(ins, 1, 25, 3);
        int ua = mac != 0;
        int ub = mac == 2 || (mac >= 4 && mac <= 12);
        int uc = mac == 3 || mac == 4 || ilu != 0;
        if ((ua && bw_vsh_field(ins, 2, 26, 2) == 2)
         || (ub && bw_vsh_field(ins, 2, 11, 2) == 2)
         || (uc && bw_vsh_field(ins, 3, 28, 2) == 2))
            m |= 1u << bw_vsh_field(ins, 1, 9, 4);
    }
    return m;
}

/* Register (or remove) the extended entry. The executor uses it for
 * transformed batches while a toolkit back end is also registered. */
void bw_backend_ext_register(const BwXBackend *backend);

/* During clear(): the title's clear rectangle (SET_CLEAR_RECT_HORIZONTAL /
 * _VERTICAL) as x0, y0, x1, y1 with x1/y1 exclusive, in the title's
 * (logical, non-anti-aliased) pixels. Returns 0 if it never sent both. */
int bw_backend_clear_rect(uint32_t r[4]);

/* The last value the title wrote to NV097 method `method` (the executor's
 * register mirror), e.g. 0x1D78 SET_ZMIN_MAX_CONTROL. */
uint32_t bw_backend_reg(uint32_t method);

#endif
