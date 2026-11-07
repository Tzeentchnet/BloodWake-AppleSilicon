#include <metal_stdlib>
using namespace metal;
struct Vertex { float4 position; float4 color; float4 texcoord; };
struct Uniforms { float4 surface_texture; float4 depth; float4 alpha; };
struct Varying { float4 position [[position]]; float4 color; float2 uv; };
vertex Varying vertex_main(uint i [[vertex_id]],
    const device Vertex *vertices [[buffer(0)]],
    constant Uniforms &u [[buffer(1)]]) {
  Vertex v = vertices[i]; Varying out;
  float w = 1.0 / v.position.w;
  float x = 2.0 * v.position.x / u.surface_texture.x - 1.0;
  float y = 1.0 - 2.0 * v.position.y / u.surface_texture.y;
  float z = (v.position.z - u.depth.x) / (u.depth.y - u.depth.x);
  out.position = float4(x * w, y * w, z * w, w);
  out.color = v.color;
  out.uv = v.texcoord.xy / u.surface_texture.zw;
  return out;
}
float4 alpha_test(float4 color, constant Uniforms &u) {
  if (u.alpha.z > 0.5) {
    float a = color.a, r = u.alpha.y; int f = int(u.alpha.x);
    bool ok = (f == 7) || (f == 1 && a < r) || (f == 2 && abs(a-r) < 0.002)
           || (f == 3 && a <= r) || (f == 4 && a > r)
           || (f == 5 && abs(a-r) >= 0.002) || (f == 6 && a >= r);
    if (!ok) discard_fragment();
  }
  return color;
}
fragment float4 color_main(Varying in [[stage_in]],
    constant Uniforms &u [[buffer(1)]]) { return alpha_test(in.color, u); }
fragment float4 texture_main(Varying in [[stage_in]],
    constant Uniforms &u [[buffer(1)]],
    texture2d<float> tex [[texture(0)]], sampler samp [[sampler(0)]]) {
  return alpha_test(in.color * tex.sample(samp, in.uv), u);
}
struct XVertex { float4 position, d0, d1, fog, tc0, tc1, tc2, tc3; };
struct XUniforms { float4 surface_texture, depth, alpha, fog_color; uint4 combiner[8], factors[8], final, texture, stages, dot_state; };
constant uint sx0 [[function_constant(0)]], sx1 [[function_constant(1)]], sx2 [[function_constant(2)]], sx3 [[function_constant(3)]];
constant uint ss0 [[function_constant(4)]], ss1 [[function_constant(5)]], ss2 [[function_constant(6)]], ss3 [[function_constant(7)]];
constant uint sd0 [[function_constant(8)]], sd1 [[function_constant(9)]], sd2 [[function_constant(10)]], sd3 [[function_constant(11)]];
constant uint sc00 [[function_constant(12)]], sc01 [[function_constant(13)]], sc02 [[function_constant(14)]], sc03 [[function_constant(15)]];
constant uint sc10 [[function_constant(16)]], sc11 [[function_constant(17)]], sc12 [[function_constant(18)]], sc13 [[function_constant(19)]];
constant uint sc20 [[function_constant(20)]], sc21 [[function_constant(21)]], sc22 [[function_constant(22)]], sc23 [[function_constant(23)]];
constant uint sc30 [[function_constant(24)]], sc31 [[function_constant(25)]], sc32 [[function_constant(26)]], sc33 [[function_constant(27)]];
constant uint sc40 [[function_constant(28)]], sc41 [[function_constant(29)]], sc42 [[function_constant(30)]], sc43 [[function_constant(31)]];
constant uint sc50 [[function_constant(32)]], sc51 [[function_constant(33)]], sc52 [[function_constant(34)]], sc53 [[function_constant(35)]];
constant uint sc60 [[function_constant(36)]], sc61 [[function_constant(37)]], sc62 [[function_constant(38)]], sc63 [[function_constant(39)]];
constant uint sc70 [[function_constant(40)]], sc71 [[function_constant(41)]], sc72 [[function_constant(42)]], sc73 [[function_constant(43)]];
constant uint sf0 [[function_constant(44)]], sf1 [[function_constant(45)]];
constant uint sa0 [[function_constant(46)]], sa1 [[function_constant(47)]];
struct XVarying { float4 position [[position]]; float4 d0; float4 d1; float fog; float fogp; float4 tc0, tc1, tc2, tc3; };
vertex XVarying extended_vertex(uint i [[vertex_id]],
    const device XVertex *vertices [[buffer(0)]], constant XUniforms &u [[buffer(1)]]) {
  XVertex v = vertices[i]; XVarying out; float w = 1.0 / v.position.w;
  float x = 2.0 * v.position.x / u.surface_texture.x - 1.0;
  float y = 1.0 - 2.0 * v.position.y / u.surface_texture.y;
  float z = (v.position.z - u.depth.x) / (u.depth.y - u.depth.x);
  out.position = float4(x * w, y * w, z * w, w);
  out.d0 = v.d0; out.d1 = v.d1; out.fog = v.fog.x; out.fogp = v.fog.y;
  out.tc0 = v.tc0; out.tc1 = v.tc1;
  out.tc2 = v.tc2; out.tc3 = v.tc3; return out;
}
float xmap(float x, uint m) {
  float p = max(x, 0.0);
  if (m == 0u) return p; if (m == 1u) return 1.0 - saturate(x);
  if (m == 2u) return 2.0 * p - 1.0; if (m == 3u) return 1.0 - 2.0 * p;
  if (m == 4u) return p - 0.5; if (m == 5u) return 0.5 - p;
  return m == 6u ? x : -x;
}
float3 xrgb(thread const float4 *r, uint b) {
  float4 v = r[b & 15u]; float3 c = (b & 16u) ? v.aaa : v.rgb;
  uint m = (b >> 5) & 7u;
  return float3(xmap(c.r, m), xmap(c.g, m), xmap(c.b, m));
}
float xa(thread const float4 *r, uint b) {
  float4 v = r[b & 15u]; return xmap((b & 16u) ? v.a : v.b, (b >> 5) & 7u);
}
float3 xout(float3 x, uint m) {
  if (m == 0x08u) return x - 0.5; if (m == 0x10u) return x * 2.0;
  if (m == 0x18u) return (x - 0.5) * 2.0; if (m == 0x20u) return x * 4.0;
  if (m == 0x30u) return x * 0.5; return x;
}
float4 xunpack(uint c) {
  return float4((c >> 16) & 255u, (c >> 8) & 255u, c & 255u, c >> 24) / 255.0;
}
float2 finite_uv(float2 uv) { return all(isfinite(uv)) ? uv : float2(0.0); }
float4 finite_tc(float4 tc) { return select(float4(0.0), tc, isfinite(tc)); }
float3 dot_map(float4 c, uint m) {
  float3 x = c.rgb * 255.0;
  if (m == 1u) return (x - 128.0) / 127.0;
  if (m == 2u) return select((x + 0.5) / 127.5, (x - 255.5) / 127.5, x >= 128.0);
  if (m == 3u) return select(x / 127.0, (x - 256.0) / 127.0, x >= 128.0);
  return c.rgb;
}
float4 cube_sample(texture2d_array<float> tex, sampler samp, float3 d) {
  float3 a = abs(d); float ma, sc, tc; uint face;
  if (a.x >= a.y && a.x >= a.z) {
    face = d.x > 0.0 ? 0u : 1u; ma = a.x; sc = d.x > 0.0 ? -d.z : d.z; tc = -d.y;
  } else if (a.y >= a.z) {
    face = d.y > 0.0 ? 2u : 3u; ma = a.y; sc = d.x; tc = d.y > 0.0 ? d.z : -d.z;
  } else {
    face = d.z > 0.0 ? 4u : 5u; ma = a.z; sc = d.z > 0.0 ? d.x : -d.x; tc = -d.y;
  }
  if (ma == 0.0) ma = 1.0;
  return tex.sample(samp, float2(sc / ma + 1.0, tc / ma + 1.0) * 0.5, face);
}
float4 legacy_linear(texture2d<float> tex, float2 uv) {
  float2 size = float2(tex.get_width(), tex.get_height());
  float2 p = clamp(uv, float2(-1.0), size + 1.0) - 0.5;
  float2 base = floor(p), f = p - base;
  int2 lo = clamp(int2(base), int2(0), int2(size) - 1);
  int2 hi = clamp(int2(base) + 1, int2(0), int2(size) - 1);
  float4 a = tex.read(uint2(lo.x, lo.y)), b = tex.read(uint2(hi.x, lo.y));
  float4 c = tex.read(uint2(lo.x, hi.y)), d = tex.read(uint2(hi.x, hi.y));
  return (a * (1.0 - f.x) + b * f.x) * (1.0 - f.y)
       + (c * (1.0 - f.x) + d * f.x) * f.y;
}
float4 extended_shade(XVarying in, constant XUniforms &u,
    texture2d<float> tex, texture2d<float> tex1,
    texture2d<float> tex2, texture2d<float> tex3,
    texture2d_array<float> cube0, texture2d_array<float> cube1,
    texture2d_array<float> cube2, texture2d_array<float> cube3,
    sampler samp, sampler samp1, sampler samp2, sampler samp3,
    bool realistic, uint4 texture_state, uint4 stages_state,
    uint4 dot_state, uint4 c0, uint4 c1, uint4 c2, uint4 c3,
    uint4 c4, uint4 c5, uint4 c6, uint4 c7,
    uint2 final_state, uint2 alpha_state) {
  float4 r[16]; for (uint i = 0; i < 16; ++i) r[i] = 0.0;
  uint4 combiner_state[8] = { c0, c1, c2, c3, c4, c5, c6, c7 };
  r[3] = float4(u.fog_color.rgb, saturate(in.fogp));
  r[4] = in.d0; r[5] = in.d1;
  float4 tc[4] = { finite_tc(in.tc0), finite_tc(in.tc1), finite_tc(in.tc2), finite_tc(in.tc3) };
  float q0 = tc[0].w != 0.0 ? tc[0].w : 1.0;
  float q1 = tc[1].w != 0.0 ? tc[1].w : 1.0;
  float q2 = tc[2].w != 0.0 ? tc[2].w : 1.0;
  float q3 = tc[3].w != 0.0 ? tc[3].w : 1.0;
  float2 uv0 = tc[0].xy / q0, uv1 = tc[1].xy / q1;
  float2 uv2 = tc[2].xy / q2, uv3 = tc[3].xy / q3;
  uv0 = finite_uv(uv0); uv1 = finite_uv(uv1);
  uv2 = finite_uv(uv2); uv3 = finite_uv(uv3);
  if (stages_state.y & 1u) uv0 /= float2(tex.get_width(), tex.get_height());
  if (stages_state.y & 2u) uv1 /= float2(tex1.get_width(), tex1.get_height());
  if (stages_state.y & 4u) uv2 /= float2(tex2.get_width(), tex2.get_height());
  if (stages_state.y & 8u) uv3 /= float2(tex3.get_width(), tex3.get_height());
  uint m0 = dot_state.x & 31u, m1 = (dot_state.x >> 5) & 31u;
  uint m2 = (dot_state.x >> 10) & 31u, m3 = (dot_state.x >> 15) & 31u;
  float dp1 = 0.0, dp2 = 0.0, dp3 = 0.0;
  if (m0 == 4u) r[8] = saturate(tc[0]);
  else if (!(stages_state.x & 1u)) r[8] = m0 == 1u ? float4(1.0) : float4(0, 0, 0, 1);
  else if (texture_state.x) {
    float2 uv = finite_uv(tc[0].xy / q0);
    r[8] = 0.0;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)
      r[8] += legacy_linear(tex, uv + float2(x, y))
            * (float((2 - abs(x)) * (2 - abs(y))) / 16.0);
  } else r[8] = tex.sample(samp, uv0);
  if (texture_state.x) r[8].a = 1.0;
  if (m1 == 17u) {
    r[9] = float4(0.0); dp1 = dot(tc[1].xyz, dot_map(r[8], dot_state.y & 15u));
  } else if (m1 == 4u) r[9] = saturate(tc[1]);
  else if (m1 == 1u) r[9] = (stages_state.x & 2u) ? tex1.sample(samp1, uv1) : float4(1.0);
  else r[9] = float4(0, 0, 0, 1);
  if (m2 == 11u) {
    r[10] = float4(0.0);
    uint input2 = (dot_state.z >> 16) & 15u; if (input2 >= 2u) input2 = 0u;
    dp2 = dot(tc[2].xyz, dot_map(r[8u + input2], (dot_state.y >> 4) & 15u));
    uint input3 = (dot_state.z >> 20) & 15u; if (input3 >= 3u) input3 = 0u;
    dp3 = dot(tc[3].xyz, dot_map(r[8u + input3], (dot_state.y >> 8) & 15u));
    if (stages_state.w & 4u) r[10] = cube_sample(cube2, samp2, float3(dp1, dp2, dp3));
  } else if (m2 == 4u) r[10] = saturate(tc[2]);
  else if (m2 == 1u) r[10] = (stages_state.x & 4u) ? tex2.sample(samp2, uv2) : float4(1.0);
  else r[10] = float4(0, 0, 0, 1);
  if (m3 == 12u) {
    r[11] = float4(0.0);
    uint input3 = (dot_state.z >> 20) & 15u; if (input3 >= 3u) input3 = 0u;
    dp3 = dot(tc[3].xyz, dot_map(r[8u + input3], (dot_state.y >> 8) & 15u));
    float3 n = float3(dp1, dp2, dp3), e = float3(tc[1].w, tc[2].w, tc[3].w);
    float nn = dot(n, n); float3 reflected = nn != 0.0 ? 2.0 * (dot(n, e) / nn) * n - e : e;
    if (stages_state.w & 8u) r[11] = cube_sample(cube3, samp3, reflected);
  } else if (m3 == 4u) r[11] = saturate(tc[3]);
  else if (m3 == 1u) r[11] = (stages_state.x & 8u) ? tex3.sample(samp3, uv3) : float4(1.0);
  else r[11] = float4(0, 0, 0, 1);
  r[12].a = r[8].a;
  for (uint s = 0; s < texture_state.y; ++s) {
    r[1] = xunpack(u.factors[(texture_state.w & 0x10u) ? s : 0].x);
    r[2] = xunpack(u.factors[(texture_state.w & 0x100u) ? s : 0].y);
    uint ci = combiner_state[s].x, ai = combiner_state[s].y;
    uint co = combiner_state[s].z, ao = combiner_state[s].w;
    float3 A = xrgb(r, ci >> 24), B = xrgb(r, ci >> 16);
    float3 C = xrgb(r, ci >> 8), D = xrgb(r, ci);
    float aA = xa(r, ai >> 24), aB = xa(r, ai >> 16);
    float aC = xa(r, ai >> 8), aD = xa(r, ai);
    uint fl = co >> 12, afl = ao >> 12;
    bool mux = texture_state.z ? r[12].a >= 0.5 : (int(r[12].a * 255.0) & 1) != 0;
    float3 pab = (fl & 2u) ? float3(dot(A, B)) : A * B;
    float3 pcd = (fl & 1u) ? float3(dot(C, D)) : C * D;
    float3 psum = (fl & 4u) ? (mux ? pcd : pab) : pab + pcd;
    float3 ab = clamp(xout(pab, fl & 0x38u), -1.0, 1.0);
    float3 cd = clamp(xout(pcd, fl & 0x38u), -1.0, 1.0);
    float3 sum = clamp(xout(psum, fl & 0x38u), -1.0, 1.0);
    float apab = aA * aB, apcd = aC * aD;
    float apsum = (afl & 4u) ? (mux ? apcd : apab) : apab + apcd;
    float aab = clamp(xout(float3(apab), afl & 0x38u).x, -1.0, 1.0);
    float acd = clamp(xout(float3(apcd), afl & 0x38u).x, -1.0, 1.0);
    float asum = clamp(xout(float3(apsum), afl & 0x38u).x, -1.0, 1.0);
    uint d = (co >> 4) & 15u; if (d) { r[d].rgb = ab; if (fl & 0x80u) r[d].a = ab.b; }
    d = co & 15u; if (d) { r[d].rgb = cd; if (fl & 0x40u) r[d].a = cd.b; }
    d = (co >> 8) & 15u; if (d) r[d].rgb = sum;
    d = (ao >> 4) & 15u; if (d) r[d].a = aab;
    d = ao & 15u; if (d) r[d].a = acd;
    d = (ao >> 8) & 15u; if (d) r[d].a = asum;
  }
  uint f0 = final_state.x, f1 = final_state.y;
  float4 o;
  if (f0 || f1) {
    r[1] = xunpack(u.final.z); r[2] = xunpack(u.final.w);
    float3 v1 = (f1 & 0x40u) ? 1.0 - r[5].rgb : r[5].rgb;
    float3 r0 = (f1 & 0x20u) ? 1.0 - r[12].rgb : r[12].rgb;
    float3 sum = v1 + r0; r[14] = float4((f1 & 0x80u) ? saturate(sum) : sum, 0.0);
    r[15] = float4(xrgb(r, f1 >> 24) * xrgb(r, f1 >> 16), 0.0);
    float3 A = xrgb(r, f0 >> 24), B = xrgb(r, f0 >> 16);
    float3 C = xrgb(r, f0 >> 8), D = xrgb(r, f0);
    o = saturate(float4(D + C * (1.0 - A) + B * A, xa(r, f1 >> 8)));
  } else o = saturate(r[12]);
  if (alpha_state.y) {
    int a = int(o.a * 255.0 + 0.5), ref = int(u.alpha.y * 255.0 + 0.5);
    int f = int(alpha_state.x);
    bool ok = f == 7 || (f == 1 && a < ref) || (f == 2 && a == ref)
           || (f == 3 && a <= ref) || (f == 4 && a > ref)
           || (f == 5 && a != ref) || (f == 6 && a >= ref);
    if (!ok) discard_fragment();
  }
  if (texture_state.x) o.rgb = floor(o.rgb * 255.0 + 0.5) / 255.0;
  if (realistic) {
    float3 n = tc[1].rgb * 2.0 - 1.0;
    float3 e = tc[2].rgb * 2.0 - 1.0;
    float nl = length(n), el = length(e);
    if (isfinite(nl) && isfinite(el) && nl > 0.0001 && el > 0.0001) {
      float ndv = saturate(abs(dot(n / nl, e / el)));
      float edge = 1.0 - ndv, edge2 = edge * edge;
      float fresnel = 0.02 + 0.98 * edge2 * edge2 * edge;
      float3 existing_reflection = saturate(r[8].rgb);
      float reflection_luma = dot(existing_reflection, float3(0.2126, 0.7152, 0.0722));
      float3 restrained_reflection = mix(float3(reflection_luma), existing_reflection, 0.72);
      float3 atmosphere = saturate(u.fog_color.rgb);
      float3 reflection = mix(restrained_reflection, atmosphere,
                              0.16 + 0.30 * fresnel);
      float strength = saturate((0.025 + 0.18 * fresnel) * saturate(in.fogp));
      o.rgb = saturate(mix(o.rgb, reflection, strength));
    }
  }
  return o;
}
fragment float4 extended_fragment(XVarying in [[stage_in]],
    constant XUniforms &u [[buffer(1)]],
    texture2d<float> tex [[texture(0)]], texture2d<float> tex1 [[texture(1)]],
    texture2d<float> tex2 [[texture(2)]], texture2d<float> tex3 [[texture(3)]],
    texture2d_array<float> cube0 [[texture(4)]], texture2d_array<float> cube1 [[texture(5)]],
    texture2d_array<float> cube2 [[texture(6)]], texture2d_array<float> cube3 [[texture(7)]],
    sampler samp [[sampler(0)]], sampler samp1 [[sampler(1)]],
    sampler samp2 [[sampler(2)]], sampler samp3 [[sampler(3)]]) {
  return extended_shade(in, u, tex, tex1, tex2, tex3,
                        cube0, cube1, cube2, cube3,
                        samp, samp1, samp2, samp3, false,
                        u.texture, u.stages, u.dot_state,
                        u.combiner[0], u.combiner[1], u.combiner[2], u.combiner[3],
                        u.combiner[4], u.combiner[5], u.combiner[6], u.combiner[7],
                        u.final.xy, uint2(uint(u.alpha.x), uint(u.alpha.z > 0.5)));
}
fragment float4 extended_specialized_fragment(XVarying in [[stage_in]],
    constant XUniforms &u [[buffer(1)]],
    texture2d<float> tex [[texture(0)]], texture2d<float> tex1 [[texture(1)]],
    texture2d<float> tex2 [[texture(2)]], texture2d<float> tex3 [[texture(3)]],
    texture2d_array<float> cube0 [[texture(4)]], texture2d_array<float> cube1 [[texture(5)]],
    texture2d_array<float> cube2 [[texture(6)]], texture2d_array<float> cube3 [[texture(7)]],
    sampler samp [[sampler(0)]], sampler samp1 [[sampler(1)]],
    sampler samp2 [[sampler(2)]], sampler samp3 [[sampler(3)]]) {
  return extended_shade(in, u, tex, tex1, tex2, tex3,
                        cube0, cube1, cube2, cube3,
                        samp, samp1, samp2, samp3, false,
                        uint4(sx0, sx1, sx2, sx3),
                        uint4(ss0, ss1, ss2, ss3),
                        uint4(sd0, sd1, sd2, sd3),
                        uint4(sc00, sc01, sc02, sc03),
                        uint4(sc10, sc11, sc12, sc13),
                        uint4(sc20, sc21, sc22, sc23),
                        uint4(sc30, sc31, sc32, sc33),
                        uint4(sc40, sc41, sc42, sc43),
                        uint4(sc50, sc51, sc52, sc53),
                        uint4(sc60, sc61, sc62, sc63),
                        uint4(sc70, sc71, sc72, sc73),
                        uint2(sf0, sf1), uint2(sa0, sa1));
}
fragment float4 realistic_water_fragment(XVarying in [[stage_in]],
    constant XUniforms &u [[buffer(1)]],
    texture2d<float> tex [[texture(0)]], texture2d<float> tex1 [[texture(1)]],
    texture2d<float> tex2 [[texture(2)]], texture2d<float> tex3 [[texture(3)]],
    texture2d_array<float> cube0 [[texture(4)]], texture2d_array<float> cube1 [[texture(5)]],
    texture2d_array<float> cube2 [[texture(6)]], texture2d_array<float> cube3 [[texture(7)]],
    sampler samp [[sampler(0)]], sampler samp1 [[sampler(1)]],
    sampler samp2 [[sampler(2)]], sampler samp3 [[sampler(3)]]) {
  return extended_shade(in, u, tex, tex1, tex2, tex3,
                        cube0, cube1, cube2, cube3,
                        samp, samp1, samp2, samp3, true,
                        u.texture, u.stages, u.dot_state,
                        u.combiner[0], u.combiner[1], u.combiner[2], u.combiner[3],
                        u.combiner[4], u.combiner[5], u.combiner[6], u.combiner[7],
                        u.final.xy, uint2(uint(u.alpha.x), uint(u.alpha.z > 0.5)));
}
struct PresentVarying { float4 position [[position]]; float2 uv; };
struct ResolveUniforms { uint width, height, aa_sx, aa_sy; };
struct ScaleUniforms { uint source_width, source_height, destination_width, destination_height; };
struct PresentUniforms { uint gamma_enabled; uint3 padding; };
vertex PresentVarying present_vertex(uint i [[vertex_id]]) {
  const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
  PresentVarying o; o.position = float4(p[i], 0, 1);
  o.uv = float2(p[i].x * 0.5 + 0.5, 0.5 - p[i].y * 0.5); return o;
}
fragment float4 present_resolve(PresentVarying in [[stage_in]],
    texture2d<float, access::read> source [[texture(0)]],
    constant ResolveUniforms &u [[buffer(0)]]) {
  uint2 logical = uint2(u.width, u.height);
  uint2 p = min(uint2(in.uv * float2(logical)), logical - 1u);
  float4 color = 0.0; uint count = u.aa_sx * u.aa_sy;
  for (uint y = 0; y < u.aa_sy; y++)
    for (uint x = 0; x < u.aa_sx; x++)
      color += source.read(p * uint2(u.aa_sx, u.aa_sy) + uint2(x, y));
  return color / float(count);
}
fragment float4 scale_fragment(PresentVarying in [[stage_in]],
    texture2d<float> source [[texture(0)]], sampler source_sampler [[sampler(0)]]) {
  return source.sample(source_sampler, in.uv);
}
fragment float4 box_fragment(PresentVarying in [[stage_in]],
    texture2d<float, access::read> source [[texture(0)]],
    constant ScaleUniforms &u [[buffer(0)]]) {
  uint2 destination = uint2(u.destination_width, u.destination_height);
  uint2 p = min(uint2(in.position.xy), destination - 1u);
  float2 ratio = float2(u.source_width, u.source_height) / float2(destination);
  float2 lo = float2(p) * ratio, hi = float2(p + 1u) * ratio;
  uint2 begin = uint2(floor(lo));
  uint2 end = min(uint2(ceil(hi)), uint2(u.source_width, u.source_height));
  float4 color = 0.0; float weight = 0.0;
  for (uint y = begin.y; y < end.y; y++) {
    float wy = max(0.0, min(hi.y, float(y + 1u)) - max(lo.y, float(y)));
    for (uint x = begin.x; x < end.x; x++) {
      float wx = max(0.0, min(hi.x, float(x + 1u)) - max(lo.x, float(x)));
      float w = wx * wy; color += source.read(uint2(x, y)) * w; weight += w;
    }
  }
  return color / weight;
}
fragment float4 present_fragment(PresentVarying in [[stage_in]],
    texture2d<float> source [[texture(0)]],
    texture2d<float, access::read> gamma_lut [[texture(1)]],
    sampler source_sampler [[sampler(0)]],
    constant PresentUniforms &u [[buffer(0)]]) {
  float4 color = source.sample(source_sampler, in.uv);
  if (u.gamma_enabled) {
    uint3 i = uint3(clamp(floor(color.rgb * 255.0 + 0.5), 0.0, 255.0));
    color.r = gamma_lut.read(uint2(i.r, 0)).r;
    color.g = gamma_lut.read(uint2(i.g, 0)).g;
    color.b = gamma_lut.read(uint2(i.b, 0)).b;
  }
  return float4(color.rgb, 1.0);
}
vertex float4 clear_vertex(uint i [[vertex_id]], constant float &z [[buffer(0)]]) {
  const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
  return float4(p[i], z, 1);
}
fragment float4 clear_color(constant float4 &c [[buffer(0)]]) { return c; }
fragment void clear_depth() {}
