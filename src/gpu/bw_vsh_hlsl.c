/*
 * bw_vsh_hlsl.c - HLSL vertex shaders from NV2A vertex programs.
 *
 * The toolkit runs a title's vertex programs on the CPU, one vertex at a
 * time (xboxrecomp src/kernel/nv2a_vsh_interp.c). In a Blood Wake mission
 * that interpreter was 42% of the GPU thread's time, and the reason one core
 * sat near 100%. This writes, for one program, the vertex shader that does
 * what the interpreter does, so the graphics card runs it instead.
 *
 * Semantics follow the interpreter rule for rule, not xemu's GLSL generator
 * (hw/xbox/nv2a/pgraph/glsl/vsh-prog.c), wherever the two differ, so the GPU
 * path draws what the CPU path draws -- except that mul and mad keep the
 * hardware's "anything times zero is zero" (vmul below):
 *   - sources read before any write; mux 1 is a temp (12 is oPos, 13-15
 *     read R0), 2 an input, 0 or 3 a constant (+a0.x if relative; outside
 *     c[0..191] reads c[0]);
 *   - MAC ops 1-13 and ILU ops 1-7 as run_mac / run_ilu, including rcp of 0
 *     as +inf, rcc's clamp, arl as floor(x + 0.001), dph adding b.w;
 *   - writes in the interpreter's order: MAC temp, ILU temp (R1 whenever a
 *     MAC op is present), then the output register; constant writes are
 *     left out (the executor keeps CXT_WRITE programs on the CPU);
 *   - outputs default as there: colours (0,0,0,1), texture coordinates
 *     (0,0,0,1), d0/d1 clamped to 0..1 at the end.
 * After the program, the executor's own steps (transform_vertex,
 * raster_xf_clipped, fog_factor in nv2a_pb_exec.c): w = +0 becomes 1, x and y
 * scale by the anti-aliasing factor, clipping at w = 1e-3 and at the near
 * plane (screen z = c[59].z), and the fog unit on oFog.x.
 *
 * Xbox D3D's programs end in its screen-space epilogue, so oPos arrives in
 * surface pixels with w the clip-space w. The shader maps it as the back
 * end's existing vertex shader does and outputs (x*w, y*w, z*w, w), an affine
 * image of clip space, so the GPU's clipper and interpolation are exact.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_vsh_hlsl.h"
#include "bw_rc_hlsl.h"                         /* BW_RC_CBUFFER */
#include "toolkit_patches/kernel/bw_backend_ext.h"   /* bw_vsh_field */

typedef struct {
    char *p;
    size_t n, cap;
    int overflow;
} Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int w;
    if (b->overflow)
        return;
    va_start(ap, fmt);
    w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->cap - b->n) {
        b->overflow = 1;
        return;
    }
    b->n += (size_t)w;
}

#define F bw_vsh_field

static const char *mask_str(uint32_t m, char out[6])
{
    int k = 0;
    out[k++] = '.';
    if (m & 8) out[k++] = 'x';
    if (m & 4) out[k++] = 'y';
    if (m & 2) out[k++] = 'z';
    if (m & 1) out[k++] = 'w';
    out[k] = 0;
    return out;
}

static const char *temp_name(uint32_t t, char out[8])
{
    if (t == 12)
        return "OP";
    snprintf(out, 8, "R%u", t);
    return out;
}

static unsigned popcount(uint32_t v)
{
    unsigned c = 0;
    for (; v; v &= v - 1)
        c++;
    return c;
}

/* read_src: one source operand, as an HLSL float4 expression. */
/* Blood Wake P1: the shader-side conversion of input k (bw_backend_ext.h,
 * BwXVsInput.conv); 0 when the input arrives as floats (gathered, a float
 * format, a constant) and is read as it is. */
static uint8_t in_conv(const uint8_t *conv, uint32_t k)
{
    uint8_t c;
    if (!conv)
        return 0;
    c = conv[k];
    switch (c >> 4) {
    case BW_XVS_COLOR: case BW_XVS_UBYTE: case BW_XVS_SHORT: case BW_XVS_CMP:
        return c;
    default:
        return 0;
    }
}

static void put_src(Buf *b, const char *var, const uint32_t *ins, int which,
                    uint32_t input_mask, const uint8_t *conv)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char base[48];

    if (which == 0) {
        neg = F(ins, 1, 8, 1); swz = F(ins, 1, 0, 8);
        mux = F(ins, 2, 26, 2); reg = F(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = F(ins, 2, 25, 1); swz = F(ins, 2, 17, 8);
        mux = F(ins, 2, 11, 2); reg = F(ins, 2, 13, 4);
    } else {
        neg = F(ins, 2, 10, 1); swz = F(ins, 2, 2, 8);
        mux = F(ins, 3, 28, 2);
        reg = (F(ins, 2, 0, 2) << 2) | F(ins, 3, 30, 2);
    }
    if (mux == 1) {
        if (reg == 12)
            snprintf(base, sizeof base, "OP");
        else
            snprintf(base, sizeof base, "R%u", reg < 12 ? reg : 0u);
    } else if (mux == 2) {
        uint32_t in = F(ins, 1, 9, 4), k = popcount(input_mask & ((1u << in) - 1u));
        snprintf(base, sizeof base, in_conv(conv, k) ? "c%u" : "I.i%u", k);
    } else {
        uint32_t ci = F(ins, 1, 13, 8);
        if (F(ins, 3, 1, 1))
            snprintf(base, sizeof base, "K[kix(%u + a0)]", ci);
        else
            snprintf(base, sizeof base, "K[%u]", ci < 192 ? ci : 0u);
    }
    put(b, "    float4 %s = %s%s", var, neg ? "-" : "", base);
    if (swz != 0x1B)                                       /* not .xyzw */
        put(b, ".%c%c%c%c", sw[(swz >> 6) & 3], sw[(swz >> 4) & 3],
            sw[(swz >> 2) & 3], sw[swz & 3]);
    put(b, ";\n");
}

#define BW_VSH_OUT \
    "struct VSOut { float4 p : SV_Position; noperspective float4 d0 : COLOR0;\n" \
    "  noperspective float4 d1 : COLOR1; noperspective float fog : FOG;\n" \
    "  float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;\n" \
    "  float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; float fogp : FOG1;\n" \
    "  float2 clip : SV_ClipDistance0; };\n"

/* raster_xf_clipped drops a whole triangle when a vertex's clip distances
 * are not finite. The game does send such vertices: w around 1e30 with
 * z * w overflowing (BW_GPU_VSH_VERIFY found 79 in 5.6 million, mission 1).
 * The GPU clipper, handed infinities, drew them as screen-wide streaks. This
 * geometry shader keeps the CPU path's rule. */
const char bw_vsh_gs_hlsl[] =
    BW_VSH_OUT
    "[maxvertexcount(3)]\n"
    "void gs_main(triangle VSOut v[3], inout TriangleStream<VSOut> s) {\n"
    "  [unroll] for (int i = 0; i < 3; i++)\n"
    "    if (any(!isfinite(v[i].p)) || any(!isfinite(v[i].clip))) return;\n"
    "  s.Append(v[0]); s.Append(v[1]); s.Append(v[2]);\n"
    "}\n";

static const char s_head[] =
    BW_RC_CBUFFER
    "cbuffer V : register(b1) { float4 K[192]; float4 vsinfo; float4 foginfo; };\n"
    BW_VSH_OUT
    "uint kix(int i) { return (i < 0 || i >= 192) ? 0u : (uint)i; }\n"
    /* run_mac's comparisons, per component, NaN behaving as in C */
    /* mul and mad's product: anything times zero is zero, infinity and NaN
     * included -- the hardware's rule as xemu's vsh-prog.c _MUL keeps it
     * (inferred; not checked on a console). The toolkit's interpreter
     * multiplies plainly (NaN * 0 = NaN); this is the one place the GPU path
     * departs from it. */
    "float4 vmul(float4 a, float4 b) { float4 r = a * b;\n"
    "  if (a.x == 0.0 || b.x == 0.0) r.x = 0.0; if (a.y == 0.0 || b.y == 0.0) r.y = 0.0;\n"
    "  if (a.z == 0.0 || b.z == 0.0) r.z = 0.0; if (a.w == 0.0 || b.w == 0.0) r.w = 0.0;\n"
    "  return r; }\n"
    "float4 vmin(float4 a, float4 b) { return float4(a.x < b.x ? a.x : b.x,\n"
    "  a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n"
    "float4 vmax(float4 a, float4 b) { return float4(a.x >= b.x ? a.x : b.x,\n"
    "  a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w); }\n"
    "float4 vslt(float4 a, float4 b) { return float4(a.x < b.x ? 1.0 : 0.0,\n"
    "  a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0); }\n"
    "float4 vsge(float4 a, float4 b) { return float4(a.x >= b.x ? 1.0 : 0.0,\n"
    "  a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0); }\n"
    /* run_ilu */
    "float4 ircp(float x) { return (x == 0.0 ? asfloat(0x7f800000) : 1.0 / x).xxxx; }\n"
    "float4 ircc(float x) { float f = 1.0 / x;\n"
    "  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
    "  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
    "  return f.xxxx; }\n"
    "float4 irsq(float x) { return (1.0 / sqrt(abs(x))).xxxx; }\n"
    "float4 iexp(float x) { float fl = floor(x);\n"
    "  return float4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
    "float4 ilog(float x) { float ax = abs(x);\n"
    "  if (ax == 0.0) return float4(-asfloat(0x7f800000), 1.0, -asfloat(0x7f800000), 1.0);\n"
    "  float e = floor(log2(ax)); return float4(e, ax / exp2(e), log2(ax), 1.0); }\n"
    "float4 ilit(float4 c) { float nl = c.x > 0.0 ? c.x : 0.0, nh = c.y > 0.0 ? c.y : 0.0;\n"
    "  float p = c.w < -127.9961 ? -127.9961 : (c.w > 127.9961 ? 127.9961 : c.w);\n"
    "  float s = nh == 0.0 ? (p == 0.0 ? 1.0 : (p > 0.0 ? 0.0 : asfloat(0x7f800000)))\n"
    "                      : pow(nh, p);\n"
    "  return float4(1.0, nl, c.x > 0.0 ? s : 0.0, 1.0); }\n"
    /* fog_factor */
    "float fogf(float d) {\n"
    "  if (foginfo.x < 0.5) return 1.0;\n"
    "  int m = (int)foginfo.y; float px = foginfo.z, py = foginfo.w, f;\n"
    "  if (m == 0x800 || m == 0x802) f = px + exp2(d * py * 16.0) - 1.5;\n"
    "  else if (m == 0x801 || m == 0x803) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
    "  else f = px + d * py - 1.0;\n"
    "  if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n"
    "  if (f != f) f = 1.0;\n"
    "  return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f); }\n"
    /* The same factor unclamped, for FOG1 (BW_RC_PERSP): as xemu vsh.c, an
     * infinite distance gives 1, NaN 1, and the rest stays within the float
     * range; the pixel shader clamps it to 0..1. */
    "float fogr(float d) {\n"
    "  if (foginfo.x < 0.5) return 1.0;\n"
    "  int m = (int)foginfo.y; float px = foginfo.z, py = foginfo.w, f;\n"
    "  if (isinf(d)) return 1.0;\n"
    "  if (m == 0x800 || m == 0x802) f = px + exp2(d * py * 16.0) - 1.5;\n"
    "  else if (m == 0x801 || m == 0x803) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
    "  else f = px + d * py - 1.0;\n"
    "  if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n"
    "  if (f != f) f = 1.0;\n"
    "  return clamp(f, -3.4e38, 3.4e38); }\n";

/* Blood Wake P1: the integer formats' conversion, through the table the back
 * end fills with fetch_attr's own expressions (bw_d3d11.c, vfetch_table):
 * x/1023 for x in -1024..1023 at x + 1024, z/511 for z in -512..511 at
 * z + 2560, c/255 at c + 3072. Only shaders with such an input carry it. */
static const char s_conv_head[] =
    "Buffer<float> bwT : register(t0);\n"
    "float4 bwcmp(uint v) {\n"
    "  int x = asint(v << 21) >> 21, y = asint(v << 10) >> 21, z = asint(v) >> 22;\n"
    "  return float4(bwT.Load(x + 1024), bwT.Load(y + 1024), bwT.Load(z + 2560), 1.0); }\n";

size_t bw_vsh_hlsl(const uint32_t (*prog)[4], uint32_t n, uint32_t input_mask,
                   const uint8_t *conv, char *out, size_t cap)
{
    Buf b = { out, 0, cap, 0 };
    uint32_t s, nin = popcount(input_mask), any_conv = 0;
    char m1[6], tn[8];

    for (s = 0; s < nin; s++)
        any_conv |= in_conv(conv, s);
    put(&b, "%s", s_head);
    if (any_conv)
        put(&b, "%s", s_conv_head);
    {
        /* BW_GPU_VSH_MULZERO=0: plain multiplies, exactly as the interpreter
         * (to tell the hardware rule's differences from real ones). */
        static int plain = -1;
        if (plain < 0) {
            const char *e = getenv("BW_GPU_VSH_MULZERO");
            plain = e && e[0] == '0';
        }
        if (plain)
            put(&b, "#define vmul(a, b) ((a) * (b))%c", 10);
    }
    /* The inputs the program reads, in ascending order, as vertex attributes
     * ATTR0..: the back end's input layout for this shader matches. */
    put(&b, "struct VSIn { uint vid : SV_VertexID;");
    for (s = 0; s < nin; s++) {
        uint8_t c = in_conv(conv, s);
        if (!c)
            put(&b, " float4 i%u : ATTR%u;", s, s);
        else
            put(&b, " %s r%u : ATTR%u;", (c >> 4) == BW_XVS_SHORT ? "int4"
                                         : (c >> 4) == BW_XVS_CMP ? "uint" : "uint4", s, s);
    }
    put(&b, " };\n");
    put(&b,
        "VSOut vs_main(VSIn I) {\n"
        "  float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0;\n"
        "  float4 R8 = 0, R9 = 0, R10 = 0, R11 = 0, OP = 0, O5 = 0;\n"
        "  float4 O3 = float4(0, 0, 0, 1), O4 = float4(0, 0, 0, 1);\n"
        "  float4 O9 = float4(0, 0, 0, 1), O10 = float4(0, 0, 0, 1);\n"
        "  float4 O11 = float4(0, 0, 0, 1), O12 = float4(0, 0, 0, 1);\n"
        "  int a0 = 0;\n");
    /* P1: each converted input once, as fetch_attr returns it (components
     * past the size read 0, 0, 0, 1) */
    for (s = 0; s < nin; s++) {
        static const char sw[] = "xyzw";
        uint8_t c = in_conv(conv, s);
        uint32_t j, sz = c & 15u;
        if (!c)
            continue;
        if ((c >> 4) == BW_XVS_CMP) {
            put(&b, "  float4 c%u = bwcmp(I.r%u);\n", s, s);
            continue;
        }
        put(&b, "  float4 c%u = float4(", s);
        for (j = 0; j < 4; j++) {
            const char *sep = j < 3 ? ", " : ");\n";
            if ((c >> 4) == BW_XVS_COLOR)          /* bytes B, G, R, A */
                put(&b, "bwT.Load(int(I.r%u.%c) + 3072)%s", s, "zyxw"[j], sep);
            else if (j >= sz)
                put(&b, "%s%s", j == 3 ? "1.0" : "0.0", sep);
            else if ((c >> 4) == BW_XVS_UBYTE)
                put(&b, "bwT.Load(int(I.r%u.%c) + 3072)%s", s, sw[j], sep);
            else
                put(&b, "(float)I.r%u.%c%s", s, sw[j], sep);
        }
    }

    for (s = 0; s < n; s++) {
        const uint32_t *ins = prog[s];
        uint32_t mac = F(ins, 1, 21, 4), ilu = F(ins, 1, 25, 3);
        uint32_t mac_mask = F(ins, 3, 24, 4), ilu_mask = F(ins, 3, 16, 4);
        uint32_t tdst = F(ins, 3, 20, 4);
        uint32_t omask = F(ins, 3, 12, 4), oaddr = F(ins, 3, 3, 8);
        int ua = mac != 0;
        int ub = mac == 2 || (mac >= 4 && mac <= 12);
        int uc = mac == 3 || mac == 4 || ilu != 0;
        const char *msrc = mac ? "m" : "float4(0, 0, 0, 0)";
        const char *isrc = ilu ? "u" : "float4(0, 0, 0, 0)";

        put(&b, "  { /* %u */\n", s);
        if (ua) put_src(&b, "a", ins, 0, input_mask, conv);
        if (ub) put_src(&b, "b", ins, 1, input_mask, conv);
        if (uc) put_src(&b, "c", ins, 2, input_mask, conv);

        switch (mac) {
        case 0:  break;
        case 2:  put(&b, "    float4 m = vmul(a, b);\n"); break;
        case 3:  put(&b, "    float4 m = a + c;\n"); break;
        case 4:  put(&b, "    float4 m = vmul(a, b) + c;\n"); break;
        case 5:  put(&b, "    float4 m = (a.x * b.x + a.y * b.y + a.z * b.z).xxxx;\n"); break;
        case 6:  put(&b, "    float4 m = (a.x * b.x + a.y * b.y + a.z * b.z + b.w).xxxx;\n"); break;
        case 7:  put(&b, "    float4 m = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w).xxxx;\n"); break;
        case 8:  put(&b, "    float4 m = float4(1.0, a.y * b.y, a.z, b.w);\n"); break;
        case 9:  put(&b, "    float4 m = vmin(a, b);\n"); break;
        case 10: put(&b, "    float4 m = vmax(a, b);\n"); break;
        case 11: put(&b, "    float4 m = vslt(a, b);\n"); break;
        case 12: put(&b, "    float4 m = vsge(a, b);\n"); break;
        case 13: put(&b, "    float4 m = a;\n"); break;
        default: put(&b, "    float4 m = a;\n"); break;        /* mov, 14, 15 */
        }
        switch (ilu) {
        case 0: break;
        case 1: put(&b, "    float4 u = c;\n"); break;
        case 2: put(&b, "    float4 u = ircp(c.x);\n"); break;
        case 3: put(&b, "    float4 u = ircc(c.x);\n"); break;
        case 4: put(&b, "    float4 u = irsq(c.x);\n"); break;
        case 5: put(&b, "    float4 u = iexp(c.x);\n"); break;
        case 6: put(&b, "    float4 u = ilog(c.x);\n"); break;
        default: put(&b, "    float4 u = ilit(c);\n"); break;
        }
        if (mac == 13)                                  /* arl */
            put(&b, "    a0 = (int)floor(a.x + 0.001);\n");

        if (mac && mac != 13 && mac_mask && tdst <= 12) {
            mask_str(mac_mask, m1);
            put(&b, "    %s%s = m%s;\n", temp_name(tdst, tn), m1, m1);
        }
        if (ilu && ilu_mask) {
            uint32_t it = mac ? 1u : tdst;          /* paired: always R1 */
            if (it <= 12) {
                mask_str(ilu_mask, m1);
                put(&b, "    %s%s = u%s;\n", temp_name(it, tn), m1, m1);
            }
        }
        if (omask && F(ins, 3, 11, 1)) {
            const char *src = F(ins, 3, 2, 1) ? isrc : msrc;
            char ms[6];
            mask_str(omask, ms);
            if (oaddr == 0)
                put(&b, "    OP%s = (%s)%s;\n", ms, src, ms);
            else if (oaddr == 3 || oaddr == 4 || oaddr == 5
                     || (oaddr >= 9 && oaddr <= 12))
                put(&b, "    O%u%s = (%s)%s;\n", oaddr, ms, src, ms);
        }
        put(&b, "  }\n");
        if (F(ins, 3, 0, 1))
            break;
    }

    put(&b,
        "  VSOut o;\n"
        "  float4 P = OP;\n"
        "  if (asuint(P.w) == 0u) P.w = 1.0;\n"         /* +0 only; -0 stays behind */
        /* An infinite w (rcp of a 0 rhw: the intro movies' full-screen quad)
         * is 2^64, as xemu's clampAwayZeroInf keeps it; see transform_vertex. */
        "  if (isinf(P.w)) P.w = P.w > 0.0 ? 18446744073709551616.0 : -18446744073709551616.0;\n"
        "  P.x *= vsinfo.x; P.y *= vsinfo.y;\n"
        "  float w = P.w;\n"
        "  float x = P.x * surf.z * 2.0 - 1.0;\n"
        BW_WS_REMAP_HLSL                                /* widescreen only (BW_WS) */
        "  float y = 1.0 - P.y * surf.w * 2.0;\n"
        "  float zr = (P.z - depth.x) * depth.y;\n"
        "  float z = depth.z > 0.5 ? saturate(zr) : zr;\n"
        "  o.p = float4(x * w, y * w, z * w, w);\n"
        "  o.clip.x = w - 1e-3;\n"
        "  o.clip.y = vsinfo.z > 0.5 ? (P.z - K[59].z) * w : 1.0;\n"
        "  o.d0 = saturate(O3); o.d1 = saturate(O4);\n"
        "  o.fog = fogf(O5.x); o.fogp = fogr(O5.x);\n"
        "  o.tc0 = O9; o.tc1 = O10; o.tc2 = O11; o.tc3 = O12;\n"
        "  return o;\n"
        "}\n");
    if (b.overflow)
        return 0;
    return b.n;
}
