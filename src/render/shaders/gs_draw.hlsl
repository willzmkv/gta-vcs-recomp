// GS primitive drawing for the HW GS renderer (gs_hw.cpp): HLSL port of its former GLSL shaders.
// Render targets hold PS2 rows top-down (texture row = PS2 row); colours and alpha are 0..255 values / 255.
// DUAL_SOURCE=1 also outputs the blend factor C as the second colour (Src1Alpha blending).
// SAMPLE_MODE: the pixel shader built for one way of reading the texture, picked per draw by gs_hw.cpp (drawSampleMode):
// only that path's code, so fewer registers and no branches between paths (docs/PERFORMANCE.md 18). The same code as the
// per-draw choice below, so the same pixels.
//   0 every way, chosen per draw from the constants (PS2X_HW_SPECIALISE=0; PS2X_HW_GATHER=verify)
//   1 no texture   2 nearest (fetchTexel)   3 the GS's bilinear by four loads   4 the hardware sampler (anisotropic option)
//   5 the GS's bilinear by Gather   6 a render target read 1:1 at full resolution (sampleRtFull)
#ifndef SAMPLE_MODE
#define SAMPLE_MODE 0
#endif
// SHADER_DEPTH: 1 the pixel shader writes the GS depth (SV_Depth), 0 the rasterised depth is used - the same value
// (both the vertex depth interpolated linearly in screen space) and early depth rejection stays on (PERFORMANCE.md 26)
#ifndef SHADER_DEPTH
#define SHADER_DEPTH 1
#endif

cbuffer Draw : register(b0)
{
    float4 targetRtFix;    // target size (PS2 pixels), render-target texel scale x (y: rtOrigin.w), FIX
    int4 tmeTfxTccFst;
    int4 linearIipFgeAte;
    int4 atstArefAfailRt;  // ATST, AREF, pass (0: alpha-test passing pixels, 1: failing ones), texture is a render target
    int4 texSizeWrap;      // texture w, h, WMS, WMT
    int4 region;           // MINU, MAXU, MINV, MAXV
    float4 rtOrigin;       // xy: texture origin inside the render target (PS2 pixels); z: pixel-centre offset (0.5: the
                           // GS samples pixel X at X, D3D at X + 0.5 - geometry moves by z to sample where the GS does);
                           // w: render-target texel scale y
    int4 blendCPremul;     // C (0 As, 1 Ad, 2 FIX), premultiply (0 none, 1 rgb *= C, 2 rgb *= 1 + C)
    float4 fogColor;       // rgb 0..255
    float4 filterSize;     // a filtered read (atstArefAfailRt.w == 3, sampleRtFiltered): xy the footprint of one target
                           // PS2 pixel in source texels (the tent's half-width), zw the source's size in texels (clamp)
    float4 filterDest;     // a filtered read: xy the target's own scale (target pixels per PS2 pixel)
};

struct VSIn
{
    float3 pos : POSITION;  // PS2 pixels; depth 0..1
    float3 stq : TEXCOORD0; // STQ, or U/V texels with q = 1 (FST)
    float4 col : COLOR0;
    float fog : TEXCOORD1;  // 0..1 (1 = no fog)
};

struct VSOut
{
    float4 pos : SV_Position;
    noperspective float3 stq : TEXCOORD0;
    noperspective float4 col : COLOR0;
    nointerpolation float4 colFlat : COLOR1; // the recorder gives flat primitives one colour on every vertex
    noperspective float fog : TEXCOORD1;
    noperspective float depth : TEXCOORD2;
    float2 clip : SV_ClipDistance0; // native draws: the PS2 clip test's near / far planes
#if VCS_SPIRV
    [[vk::builtin("PointSize")]] float psize : PSIZE; // Vulkan points need one (D3D11's are always 1 pixel)
#endif
};

VSOut vs_main(VSIn i)
{
    VSOut o;
    const float2 p = (i.pos.xy + rtOrigin.z) / targetRtFix.xy;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, i.pos.z, 1.0);
    o.depth = i.pos.z;
    o.stq = i.stq;
    o.col = i.col;
    o.colFlat = i.col;
    o.fog = i.fog;
    o.clip = float2(1.0, 1.0);
#if VCS_SPIRV
    o.psize = 1.0;
#endif
    return o;
}

// Native draws (packet-level HLE, docs/NATIVE_RENDERER.md stage 4+): the VU1 work on the GPU. One constant block
// for every kind; nat[31] = GS XYOFFSET in pixels (xy), Z format scale (z), filled by the renderer.
cbuffer Native : register(b1)
{
    float4 nat[32];
};
// VcsWorld: 0-3 GS window matrix rows (scale / offset folded in, VU data 28-31, 42, 43), 4-6 texture matrix rows * 1/255
//   (44-46), 7-9 prelit colour scale / offset / clamp (0x9c-0x9e), 10 fog (x on, y bias, z scale per w; 53),
//   11-14 clip-test matrix rows, folded (24-27)
// VcsLit: 0-3 GS window matrix (28-31), 4-6 texture matrix * 1/255, 7 fog, 8-11 clip-test matrix (24-27),
//   12-14 light direction rows (lanes = lights), 15-18 light colours * material, 19 ambient * material, 20 clamp,
//   21.x material alpha

// XYZF2 / STQ / clip planes from the GS window position S and the clip-test position C (shared by the native kinds)
void nativePosition(float4 S, float4 C, float3 st, inout VSOut o)
{
    const float q = 1.0 / S.w;
    const float3 s = S.xyz * q;
    // XYZF2 from FTOI4: X / Y in 12.4 fixed point, Z the integer part
    const float2 xy = trunc(s.xy * 16.0) / 16.0 - nat[31].xy;
    const float z = floor(trunc(s.z * 16.0) / 16.0);
    const float depth = min(z * nat[31].z, 1.0);
    const float2 p = (xy + rtOrigin.z) / targetRtFix.xy;
    o.pos = float4((p.x * 2.0 - 1.0) * S.w, (1.0 - p.y * 2.0) * S.w, depth * S.w, S.w);
    o.depth = depth;
    o.clip = float2(C.z + C.w, C.w - C.z); // the PS2 clip test's near / far planes
    o.stq = st * q;
#if VCS_SPIRV
    o.psize = 1.0;
#endif
}

float nativeFog(float4 fog, float w)
{
    float f = 0.0;
    if (fog.x > 0.5)
        f = clamp(fog.z * w + fog.y, 0.0, 255.0);
    return floor(trunc(f * 16.0) / 16.0) / 255.0;
}

struct WorldIn
{
    float3 pos : POSITION;  // VU ITOF0 of the unpacked V3-16 position
    float3 uv : TEXCOORD0;  // ITOF0 of the V2-8 texture coordinate qword (x, y, z lanes)
    uint col : COLOR0;      // the V4-5 colour's expanded bytes
};

// VCS prelit world (VU1 0x0030 with stages 0x0da / 0x4e0 / 0x2b6): see docs/game-internals.md
VSOut vs_world(WorldIn i)
{
    VSOut o = (VSOut)0;
    const float4 S = nat[0] * i.pos.x + nat[1] * i.pos.y + nat[2] * i.pos.z + nat[3];
    const float4 C = nat[11] * i.pos.x + nat[12] * i.pos.y + nat[13] * i.pos.z + nat[14];
    nativePosition(S, C, nat[4].xyz * i.uv.x + nat[5].xyz * i.uv.y + nat[6].xyz * i.uv.z, o);
    const float4 c = float4(i.col & 0xFF, (i.col >> 8) & 0xFF, (i.col >> 16) & 0xFF, i.col >> 24);
    o.col = trunc(min(nat[8] + nat[7] * c, nat[9])) / 255.0;
    o.colFlat = o.col;
    o.fog = nativeFog(nat[10], S.w);
    return o;
}

struct LitIn
{
    float3 pos : POSITION;   // skinned position (the VU's stage 0x5a5 output)
    float3 uv : TEXCOORD0;   // ITOF0 of the V2-8 texture coordinate qword
    float3 nrm : NORMAL;     // skinned normal
};

// VCS lit / skinned models (VU1 0x0030 with stages 0x5a5 / 0x0da / 0x4ff / 0x2b6): see docs/game-internals.md
VSOut vs_lit(LitIn i)
{
    VSOut o = (VSOut)0;
    const float4 S = nat[0] * i.pos.x + nat[1] * i.pos.y + nat[2] * i.pos.z + nat[3];
    const float4 C = nat[8] * i.pos.x + nat[9] * i.pos.y + nat[10] * i.pos.z + nat[11];
    nativePosition(S, C, nat[4].xyz * i.uv.x + nat[5].xyz * i.uv.y + nat[6].xyz * i.uv.z, o);
    const float4 d = max(nat[12] * i.nrm.x + nat[13] * i.nrm.y + nat[14] * i.nrm.z, 0.0);
    const float4 c = trunc(min(nat[20], nat[15] * d.x + nat[16] * d.y + nat[17] * d.z + nat[18] * d.w + nat[19]));
    o.col = float4(c.rgb, nat[21].x) / 255.0;
    o.colFlat = o.col;
    o.fog = nativeFog(nat[7], S.w);
    return o;
}

Texture2D<float4> tex : register(t0);
SamplerState hwSampler : register(s0); // anisotropic filtering (linearIipFgeAte.x == 2); the gather path's wrapping (3)

int wrapc(int c, int size, int mode, int mn, int mx)
{
    int r = (c & mn) | mx;                       // REGION_REPEAT
    // REPEAT. GS sizes are powers of two: a mask, the same result as the modulo for every c; native effect buffers need
    // not be. The modulo is two integer divisions per coordinate and tap (~2 ms of the helicopter's dust at 3x,
    // docs/PERFORMANCE.md section 15)
    [branch] if (mode == 0)
    {
        [branch] if ((size & (size - 1)) == 0) r = c & (size - 1);
        else r = ((c % size) + size) % size;
    }
    else if (mode == 1) r = clamp(c, 0, size - 1); // CLAMP
    else if (mode == 2) r = clamp(c, mn, mx);    // REGION_CLAMP
    return r;
}

// the render target read: its pixels per PS2 pixel (x, y)
float2 rtScale2()
{
    return float2(targetRtFix.z, rtOrigin.w);
}

float4 fetchTexel(int2 t)
{
    t.x = wrapc(t.x, texSizeWrap.x, texSizeWrap.z, region.x, region.y);
    t.y = wrapc(t.y, texSizeWrap.y, texSizeWrap.w, region.z, region.w);
    int2 at = t;
    if (atstArefAfailRt.w == 1)
        at = int2((float2(t) + rtOrigin.xy + 0.5) * rtScale2());
    return tex.Load(int3(at, 0)) * 255.0;
}

// A render target read 1:1 (a copy of the picture, texture texel = target pixel; atstArefAfailRt.w == 2): sampled at the
// target's own resolution instead of once per PS2 texel, so a copy keeps the internal resolution. The wrap / clamp
// still works on PS2 texels (a clamped coordinate takes the nearest sample inside the allowed texel). At scale 1 this
// fetches the same texels as fetchTexel.
// Texture packs use this path too: GS coordinates/wrap stay original, scale maps to the replacement's actual pixels.
float4 fetchRtFull(int2 c)
{
    const float2 s = rtScale2();
    const int2 t = int2(floor((float2(c) + 0.5) / s - rtOrigin.xy));
    int2 w;
    w.x = wrapc(t.x, texSizeWrap.x, texSizeWrap.z, region.x, region.y);
    w.y = wrapc(t.y, texSizeWrap.y, texSizeWrap.w, region.z, region.w);
    // the target pixels of texel w (non-integer scales: the native frame)
    const int2 lo = int2(ceil((float2(w) + rtOrigin.xy) * s - 0.5));
    const int2 hi = int2(ceil((float2(w) + rtOrigin.xy + 1.0) * s - 0.5)) - 1;
    const int2 at = clamp(c, lo, max(lo, hi));
    return tex.Load(int3(at, 0)) * 255.0;
}

float4 sampleRtFull(float2 uv)
{
    const float2 h = (uv + rtOrigin.xy) * rtScale2(); // target texels
    [branch] if (linearIipFgeAte.x == 0)
        return fetchRtFull(int2(floor(h)));
    const float2 p = h - 0.5;
    const int2 i = int2(floor(p));
    const float2 f = p - float2(i);
    const float4 c00 = fetchRtFull(i), c10 = fetchRtFull(i + int2(1, 0));
    const float4 c01 = fetchRtFull(i + int2(0, 1)), c11 = fetchRtFull(i + int2(1, 1));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

static bool g_gatherMismatch = false; // PS2X_HW_GATHER=verify (sampleTex mode 4)

// The GS's bilinear (its texels, its weights) with the 2x2 texels fetched by Gather (linearIipFgeAte.x 3 / 4): a
// power-of-two texture with plain REPEAT / CLAMP wrapping, bound with a nearest sampler of the same wrapping (gs_hw.cpp
// m_gatherSampler). The top-left texel is wrapped / clamped here, so the gather point is a texel corner inside the
// texture, where the hardware's footprint is exact; the right / bottom neighbour wraps or clamps in the sampler. The
// same values as fetchTexel's four loads, without their per-tap wrap arithmetic (the helicopter's dust: ~60 screens of
// textured overdraw a frame, docs/game-internals.md 6j).
float4 sampleGather(float2 uv)
{
    const float2 p = uv - 0.5;
    const int2 i = int2(floor(p));
    const float2 f = p - floor(p);
    const int2 size = texSizeWrap.xy;
    int2 w;
    w.x = texSizeWrap.z == 0 ? (i.x & (size.x - 1)) : clamp(i.x, -1, size.x - 1);
    w.y = texSizeWrap.w == 0 ? (i.y & (size.y - 1)) : clamp(i.y, -1, size.y - 1);
    const float2 at = (float2(w) + 1.0) / float2(size);
    // Gather order: x (u0, v1), y (u1, v1), z (u1, v0), w (u0, v0)
    const float4 gr = tex.GatherRed(hwSampler, at), gg = tex.GatherGreen(hwSampler, at);
    const float4 gb = tex.GatherBlue(hwSampler, at), ga = tex.GatherAlpha(hwSampler, at);
    const float4 c00 = float4(gr.w, gg.w, gb.w, ga.w) * 255.0, c10 = float4(gr.z, gg.z, gb.z, ga.z) * 255.0;
    const float4 c01 = float4(gr.x, gg.x, gb.x, ga.x) * 255.0, c11 = float4(gr.y, gg.y, gb.y, ga.y) * 255.0;
#if SAMPLE_MODE == 0
    // mode 4 (PS2X_HW_GATHER=verify, the full shader): the four loads as well; a texel that differs paints the pixel magenta
    [branch] if (linearIipFgeAte.x == 4)
        g_gatherMismatch = any(c00 != fetchTexel(i)) || any(c10 != fetchTexel(i + int2(1, 0))) ||
                           any(c01 != fetchTexel(i + int2(0, 1))) || any(c11 != fetchTexel(i + int2(1, 1)));
#endif
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

// the GS's bilinear by four loads (the wrap worked out per tap: region modes, render targets read per PS2 texel)
float4 sampleBilinear(float2 uv)
{
    const float2 p = uv - 0.5;
    const int2 i = int2(floor(p));
    const float2 f = p - float2(i);
    const float4 c00 = fetchTexel(i), c10 = fetchTexel(i + int2(1, 0));
    const float4 c01 = fetchTexel(i + int2(0, 1)), c11 = fetchTexel(i + int2(1, 1));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

// A filtered read of a render target (atstArefAfailRt.w == 3): the reflection map's copy of the frame with "Car
// reflections: Smooth" (gs_hw.cpp gsHwSetTargetFilter). Instead of the GS's four taps out of the ~5 x 3.5 frame pixels
// a map texel stands for, a tent filter over the texel's footprint and its neighbours' (half-width filterSize.xy,
// source texels) at the source's own resolution: a car panel's reflection, one map texel, follows the picture smoothly
// as the camera moves instead of jumping with whatever single pixel lands on the tap (docs/game-internals.md 6g). At
// most 16 taps per axis (every pixel up to ~1440p, a stride above).
// Only the target pixels that are ever read get the filter: the paint reads the map one target pixel per PS2 texel
// (fetchTexel: the pixel at the texel's centre), so a map drawn above 1x has ~16k such pixels of its 100k+; the others
// take the GS's four taps (filterDest.xy: the target's own scale). Filtering all of them cost ~1.1 ms per copy at 4x3
// (GTX 1080 Ti), this ~0.2 ms (the GS's own read: 0.04).
static float2 g_pixel; // SV_Position of the pixel being shaded (ps_main)

float4 sampleRtFiltered(float2 uv)
{
    const float2 ds = max(filterDest.xy, 1.0);
    const float2 P = floor(g_pixel);
    const float2 t = floor((P + 0.5) / ds); // the PS2 texel this target pixel is in
    [branch] if (any(floor((t + 0.5) * ds) != P))
        return sampleBilinear(uv); // not the pixel a texel read takes
    const float2 s = rtScale2();
    const float2 halfSize = max(filterSize.xy, 0.5);
    uint tw, th;
    tex.GetDimensions(tw, th);
    const int2 maxPx = min(int2(tw, th), int2(ceil(filterSize.zw * s))) - 1;
    const int2 p0 = clamp(int2(ceil((uv - halfSize + rtOrigin.xy) * s - 0.5)), int2(0, 0), maxPx);
    const int2 p1 = clamp(int2(floor((uv + halfSize + rtOrigin.xy) * s - 0.5)), p0, maxPx);
    const int2 step = max(int2(1, 1), (p1 - p0 + 16) / 16);
    float4 sum = 0.0;
    float weights = 0.0;
    [loop] for (int y = p0.y; y <= p1.y; y += step.y)
    {
        const float wy = max(0.0, 1.0 - abs((float(y) + 0.5) / s.y - rtOrigin.y - uv.y) / halfSize.y);
        [loop] for (int x = p0.x; x <= p1.x; x += step.x)
        {
            const float w = wy * max(0.0, 1.0 - abs((float(x) + 0.5) / s.x - rtOrigin.x - uv.x) / halfSize.x);
            sum += tex.Load(int3(x, y, 0)) * w;
            weights += w;
        }
    }
    return weights > 0.0 ? sum * (255.0 / weights) : fetchTexel(int2(floor(uv)));
}

float4 sampleTex(float2 uv)
{
#if SAMPLE_MODE == 2
    return fetchTexel(int2(floor(uv)));
#elif SAMPLE_MODE == 3
    return sampleBilinear(uv);
#elif SAMPLE_MODE == 4
    return tex.Sample(hwSampler, uv / float2(texSizeWrap.xy)) * 255.0;
#elif SAMPLE_MODE == 5
    return sampleGather(uv);
#elif SAMPLE_MODE == 6
    return sampleRtFull(uv);
#else
    float4 r;
    [branch] if (atstArefAfailRt.w == 2)
    {
        r = sampleRtFull(uv);
    }
    else if (atstArefAfailRt.w == 3)
    {
        r = sampleRtFiltered(uv); // only the full shader (SAMPLE_MODE 0) has it: drawSampleMode picks that one
    }
    else if (linearIipFgeAte.x == 2)
    {
        // a game texture with plain wrapping, sampled by the hardware (trilinear, anisotropic); the option
        r = tex.Sample(hwSampler, uv / float2(texSizeWrap.xy)) * 255.0;
    }
    else if (linearIipFgeAte.x >= 3)
    {
        r = sampleGather(uv);
    }
    else if (linearIipFgeAte.x == 0)
    {
        r = fetchTexel(int2(floor(uv)));
    }
    else
    {
        r = sampleBilinear(uv);
    }
    return r;
#endif
}

struct PSOut
{
    float4 color : SV_Target0;
#if DUAL_SOURCE
    float4 blend : SV_Target1;
#endif
#if SHADER_DEPTH
    float depth : SV_Depth;
#endif
};

PSOut ps_main(VSOut i)
{
    PSOut o;
    g_pixel = i.pos.xy;
    // GS Z is Z24/Z32 / 2^n: written directly (going through GL's NDC rounded near values and collapsed far ones; with
    // D3D / Vulkan's 0..1 depth the rasterised value is the same: SHADER_DEPTH 0)
#if SHADER_DEPTH
    o.depth = i.depth;
#endif
    const float4 vc = floor((linearIipFgeAte.y == 1 ? i.col : i.colFlat) * 255.0 + 0.5);
    float4 c = vc;
#if SAMPLE_MODE != 1
    if (tmeTfxTccFst.x == 1)
    {
        const float2 uv = (tmeTfxTccFst.w == 1) ? i.stq.xy : (i.stq.xy / i.stq.z) * float2(texSizeWrap.xy);
        const float4 t = sampleTex(uv);
        const int tfx = tmeTfxTccFst.y;
        const bool tcc = tmeTfxTccFst.z == 1;
        if (tfx == 0) { c.rgb = t.rgb * vc.rgb / 128.0; c.a = tcc ? t.a * vc.a / 128.0 : vc.a; }
        else if (tfx == 1) { c.rgb = t.rgb; c.a = tcc ? t.a : vc.a; }
        else if (tfx == 2) { c.rgb = t.rgb * vc.rgb / 128.0 + vc.a; c.a = tcc ? t.a + vc.a : vc.a; }
        else { c.rgb = t.rgb * vc.rgb / 128.0 + vc.a; c.a = tcc ? t.a : vc.a; }
    }
#endif
    c = clamp(floor(c), 0.0, 255.0);
    if (linearIipFgeAte.z == 1)
    {
        const float f = floor(i.fog * 255.0 + 0.5);
        c.rgb = floor((c.rgb * f + fogColor.rgb * (255.0 - f)) / 255.0);
    }
    bool passed = true;
    if (linearIipFgeAte.w == 1)
    {
        const float a = c.a, r = float(atstArefAfailRt.y);
        const int atst = atstArefAfailRt.x;
        if (atst == 0) passed = false;
        else if (atst == 2) passed = a < r;
        else if (atst == 3) passed = a <= r;
        else if (atst == 4) passed = a == r;
        else if (atst == 5) passed = a >= r;
        else if (atst == 6) passed = a > r;
        else if (atst == 7) passed = a != r;
    }
    if (atstArefAfailRt.z == 0 && !passed) discard;
    if (atstArefAfailRt.z == 1 && passed) discard;
    const int bc = blendCPremul.x;
    const float cf = (bc == 0) ? c.a / 128.0 : ((bc == 2) ? targetRtFix.w / 128.0 : 1.0);
    float3 rgb = c.rgb;
    if (blendCPremul.y == 1) rgb *= cf;
    else if (blendCPremul.y == 2) rgb *= (1.0 + cf);
    o.color = float4(rgb / 255.0, c.a / 255.0);
    if (g_gatherMismatch)
        o.color = float4(1.0, 0.0, 1.0, 1.0);
#if DUAL_SOURCE
    o.blend = min(cf, 1.0).xxxx;
#endif
    return o;
}
