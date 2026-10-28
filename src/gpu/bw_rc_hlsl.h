/* bw_rc_hlsl.h - HLSL pixel shaders from NV2A register-combiner state.
 * See bw_rc_hlsl.c. */
#ifndef BW_RC_HLSL_H
#define BW_RC_HLSL_H

#include <stddef.h>
#include <stdint.h>

/* Everything that changes the shader's code. Constants (c0/c1, the final
 * combiner's constants, fog colour, texture sizes, alpha test) live in the
 * constant buffer instead, so a material colour change does not compile. */
typedef struct {
    uint32_t color_icw[8], alpha_icw[8], color_ocw[8], alpha_ocw[8];
    uint32_t final0, final1, control, stage_program, clip_plane_mode;
    uint8_t tex_valid[4], tex_normalized[4], tex_cube[4];
    uint8_t vfx[4];     /* VFX (D9): per-stage sampling enhancements (BW_RC_VFX_*); 0 in Original */
    uint8_t tex_conv[4];        /* CONVOLUTION_2D (min 7 / mag 4): the kernel, 1 quincunx 2 gaussian */
    uint8_t flags[4];           /* [0] BW_RC_PERSP: colours and fog as xemu interpolates them */
    uint32_t dot_rgbmapping, other_stage_input;   /* dot-product stages only */
} BwRcKey;

/* VFX (D9), Remastered only: stage samples a glyph sheet; drawn with the
 * contour filter when magnified (needs a linear sampler at s4). */
#define BW_RC_VFX_GLYPH 1
/* VFX (D9): an overlay (HUD) draw's stage samples a 1-bit-alpha texture:
 * the same contour, with an alpha-weighted colour. */
#define BW_RC_VFX_OVERLAY 2
/* VFX (D9): any other overlay (HUD) texture: Catmull-Rom on premultiplied
 * texels, clamped to the 2x2 neighbourhood, when magnified. */
#define BW_RC_VFX_OVERLAY_CR 4
/* VFX (V1, VFX_HUD_EDGES): an overlay (HUD) texture whose alpha is hard-edged
 * (opaque shapes with a one-texel anti-aliased rim, e.g. the radar's compass
 * ring): a smooth contour at alpha 0.5 and colour weighted towards the opaque
 * texels, when magnified. */
#define BW_RC_VFX_OVERLAY_HARD 8
/* T1 (VFX_TEX_RESTORE, menu photos): an overlay draw of a JPEG screen
 * texture (title, menus, results backgrounds): Lanczos-2 on premultiplied
 * texels, clamped to the 2x2 neighbourhood, when magnified (instead of
 * BW_RC_VFX_OVERLAY_CR's Catmull-Rom). */
#define BW_RC_VFX_OVERLAY_L2 0x10
/* T1 (BW_TEXRESTORE_DEBUG=1): the stage samples a restored chain; tint the
 * pixel green where it reads the enlarged levels (LOD below the chain's copy
 * of the game's level 0), blue where it reads the game's own levels. */
#define BW_RC_VFX_TR_DEBUG 0x20
/* VFX (D9), on vfx[0] only: the alpha test becomes alpha-to-coverage (MSAA):
 * coverage ramps over one pixel around the game's own threshold. */
#define BW_RC_VFX_A2C 0x80

/* flags[0]: colours and fog interpolated perspective-correctly, fog clamped
 * per pixel (FOG1, unclamped per vertex), as xemu; else the CPU rasteriser's
 * old rule: affine colours, fog clamped per vertex (FOG). */
#define BW_RC_PERSP 1

/* The constant buffer both extended shaders read (register b0). */
#define BW_RC_CBUFFER \
    "cbuffer C : register(b0) {\n" \
    "  float4 surf; float4 depth; float4 alpha; float4 fogc;\n" \
    "  float4 C0[8]; float4 C1[8]; float4 FC0; float4 FC1;\n" \
    "  float4 texinfo[4];\n" \
    "#ifdef BW_WS\n" \
    "  float4 wsx;\n" \
    "#endif\n" \
    "};\n"
/* wsx (Remastered widescreen only, compiled in with the BW_WS define, so
 * Original's shaders preprocess to the same text as before): the vertex
 * shaders' x remap, x' = x * wsx.x + wsx.y in NDC. (1, 0) stretches over the
 * wide surface (3D); (1/k, c(1 - 1/k)) pillarboxes around the view centre c
 * (2D). bw_d3d11.c, ws_remap. */
#define BW_WS_REMAP_HLSL \
    "#ifdef BW_WS\n" \
    "  x = x * wsx.x + wsx.y;\n" \
    "#endif\n"

/* Write the pixel shader (entry point ps_main) for `k` into out. Returns the
 * length, or 0 if it did not fit. */
size_t bw_rc_hlsl(const BwRcKey *k, char *out, size_t cap);

#endif
