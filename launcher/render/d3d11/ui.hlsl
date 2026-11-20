// The launcher's one pipeline: every primitive (rounded rectangle, border, gradient, soft shadow, glyph, line, arc) is
// one 96-byte instance expanded to a quad here, so a whole frame is a single instanced draw with no vertex or index
// buffer. After uppies' renderer (docs/LAUNCHER.md).
//
// Coordinates are physical pixels, origin top-left. Colours arrive as sRGB RGBA8 and are blended in sRGB like
// browsers and design tools do, so what is picked is what appears. Output is premultiplied alpha.
//
// Keep Instance in sync with render::Instance in render/draw.h.
//
// One source for both backends: fxc compiles it for Direct3D 11, glslang (tools/hlsl_spirv.py, VCS_SPIRV) for Vulkan,
// where the frame constants are push constants and the viewport is flipped (render/vulkan/renderer_vulkan.cpp).

#ifdef VCS_SPIRV
[[vk::push_constant]] cbuffer Frame // no register: a push constant block has no binding
#else
cbuffer Frame : register(b0)
#endif
{
    float2 viewport_size;  // pixels
    float2 atlas_inv_size; // 1 / glyph atlas size in texels
};

Texture2D<float> glyph_atlas : register(t0);
Texture2D<float4> image_atlas : register(t1);
SamplerState atlas_sampler : register(s0);

static const uint KIND_RECT = 0;
static const uint KIND_GLYPH = 1;
static const uint KIND_SHADOW = 2;
static const uint KIND_LINE = 3;
static const uint KIND_ARC = 4;
static const uint KIND_QUAD = 5;
static const uint KIND_IMAGE = 6;

struct Instance
{
    float4 rect : RECT;          // x0 y0 x1 y1
    float4 uv : UV;              // glyph: atlas texels; line: endpoints; arc: start, sweep, radius, thickness; quad: 2 corners
    float4 radii : RADII;        // tl tr br bl; quad: the other 2 corners
    float4 clip : CLIP;          // x0 y0 x1 y1
    float4 color0 : COLOR0;      // fill (gradient start), glyph, shadow, line, arc
    float4 color1 : COLOR1;      // gradient end
    float4 border_color : COLOR2;
    uint kind : KIND;
    float4 params : PARAMS;      // x border width, y shadow sigma / glyph blur radius, z line thickness, w gradient direction
};

struct VSOut
{
    float4 position : SV_Position;
    float2 pixel : PIXEL;
    float2 uv : TEXCOORD0;
    nointerpolation float4 rect : RECT;
    nointerpolation float4 radii : RADII;
    nointerpolation float4 color0 : COLOR0;
    nointerpolation float4 color1 : COLOR1;
    nointerpolation float4 border_color : COLOR2;
    nointerpolation uint kind : KIND;
    nointerpolation float4 params : PARAMS;
    nointerpolation float4 data : DATA; // the raw uv (lines, arcs)
    nointerpolation float4 clip : CLIP;
};

VSOut vs_main(uint vertex_id : SV_VertexID, Instance inst)
{
    // Grow the quad to hold the antialiased edge (a shadow: its blur), then clip it here: clipped pixels are never
    // shaded and clipping never splits the batch. The clip is taken out to whole pixels; the pixel shader covers the
    // pixels a clip edge cuts through by how much of them is inside, so a clip that moves by a fraction of a pixel
    // moves smoothly (the sun's slices on Home) and one on pixel edges is exactly as before.
    float2 grow = 1.0;
    if (inst.kind == KIND_GLYPH || inst.kind == KIND_LINE || inst.kind == KIND_ARC)
        grow = 0.0; // their rects already hold their edges
    if (inst.kind == KIND_SHADOW)
        grow = 3.0 * inst.params.y + 1.0;
    if (inst.kind == KIND_GLYPH && inst.params.y > 0.0)
        grow = inst.params.y + 1.0; // a blurred glyph spreads by its radius

    float2 lo = max(inst.rect.xy - grow, floor(inst.clip.xy));
    float2 hi = min(inst.rect.zw + grow, ceil(inst.clip.zw));
    hi = max(hi, lo); // fully clipped: a degenerate quad

    float2 corner = float2(vertex_id & 1, vertex_id >> 1); // triangle strip order
    float2 p = lerp(lo, hi, corner);

    VSOut o;
    o.position = float4(p * (2.0 / viewport_size) * float2(1, -1) + float2(-1, 1), 0, 1);
    o.pixel = p;
    float2 t = (p - inst.rect.xy) / max(inst.rect.zw - inst.rect.xy, 1e-5);
    o.uv = lerp(inst.uv.xy, inst.uv.zw, t) * (inst.kind == KIND_IMAGE ? float2(1.0, 1.0) : atlas_inv_size);
    o.rect = inst.rect;
    o.radii = inst.radii;
    o.color0 = inst.color0;
    o.color1 = inst.color1;
    o.border_color = inst.border_color;
    o.kind = inst.kind;
    o.params = inst.params;
    o.data = inst.uv;
    o.clip = inst.clip;
    return o;
}

// Signed distance to a rounded box centred at the origin, a radius per corner. Negative inside, in pixels.
float sd_round_box(float2 p, float2 half_size, float4 radii)
{
    float r = p.x < 0 ? (p.y < 0 ? radii.x : radii.w) : (p.y < 0 ? radii.y : radii.z);
    r = min(r, min(half_size.x, half_size.y));
    float2 q = abs(p) - half_size + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// The shadow of a rounded box blurred by a Gaussian: exact along x (erf), integrated along y in a few samples.
// After Evan Wallace's derivation.
float2 erf2(float2 x)
{
    float2 s = sign(x), a = abs(x);
    x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    x *= x;
    return s - s / (x * x);
}

float gaussian(float x, float sigma)
{
    return exp(-(x * x) / (2.0 * sigma * sigma)) / (2.50662827 * sigma);
}

float shadow_x(float x, float y, float sigma, float corner, float2 half_size)
{
    float delta = min(half_size.y - corner - abs(y), 0.0);
    float curved = half_size.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    float2 integral = 0.5 + 0.5 * erf2((x + float2(-curved, curved)) * (0.70710678 / sigma));
    return integral.y - integral.x;
}

float rounded_box_shadow(float2 p, float2 half_size, float corner, float sigma)
{
    float low = p.y - half_size.y;
    float high = p.y + half_size.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end = clamp(3.0 * sigma, low, high);
    float step = (end - start) / 4.0;
    float y = start + step * 0.5;
    float value = 0.0;
    [unroll] for (int i = 0; i < 4; i++)
    {
        value += shadow_x(p.x, p.y - y, sigma, corner, half_size) * gaussian(y, sigma) * step;
        y += step;
    }
    return value;
}

// Blending in sRGB thins light text on dark backgrounds. Bend the coverage toward what linear blending would give,
// a little less for dark text, which reads better slightly bold.
float text_coverage(float a, float3 color)
{
    a = saturate(a);
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    float light = pow(a, 1.0 / 1.45);
    float dark = 1.0 - pow(1.0 - a, 1.0 / 1.15);
    return lerp(dark, light, luma);
}

// Interleaved gradient noise, +-half an 8-bit step: no banding in gradients.
float dither(float2 pixel)
{
    return (frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))) - 0.5) / 255.0;
}

float sd_segment(float2 p, float2 a, float2 b)
{
    float2 pa = p - a, ba = b - a;
    float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));
    return length(pa - ba * h);
}

// A glyph's coverage blurred by a Gaussian of about `radius` pixels: a 7x7 grid of weighted atlas samples (glyphs are
// rasterised at the size they are drawn, so a pixel is a texel). Samples outside the glyph's own texels count as empty,
// so its neighbours in the atlas never bleed in.
float blurred_glyph(float2 texel, float4 glyph_texels, float radius)
{
    const float sigma = radius * 0.5;
    const float step = radius / 3.0;
    float sum = 0.0, weights = 0.0;
    [unroll] for (int y = -3; y <= 3; y++)
    {
        [unroll] for (int x = -3; x <= 3; x++)
        {
            float2 o = float2(x, y) * step;
            float w = exp(-dot(o, o) / (2.0 * sigma * sigma));
            float2 t = texel + o;
            float inside = all(t >= glyph_texels.xy) && all(t <= glyph_texels.zw) ? 1.0 : 0.0;
            sum += w * inside * glyph_atlas.SampleLevel(atlas_sampler, t * atlas_inv_size, 0);
            weights += w;
        }
    }
    return sum / weights;
}

// Distance to an arc of a circle (radius r, from angle `start` clockwise by `sweep`), round caps.
float sd_arc(float2 p, float start, float sweep, float r)
{
    const float TAU = 6.28318530718;
    if (sweep >= TAU - 1e-3)
        return abs(length(p) - r);
    float a = atan2(p.y, p.x) - start;
    a = a - TAU * floor(a / TAU); // 0..2pi from the start
    if (a <= sweep)
        return abs(length(p) - r);
    float2 e0 = r * float2(cos(start), sin(start));
    float2 e1 = r * float2(cos(start + sweep), sin(start + sweep));
    return min(length(p - e0), length(p - e1));
}

float4 shade(VSOut i)
{
    if (i.kind == KIND_LINE)
    {
        float d = sd_segment(i.pixel, i.data.xy, i.data.zw) - i.params.z * 0.5;
        float a = saturate(0.5 - d) * i.color0.a;
        return float4(i.color0.rgb * a, a);
    }

    if (i.kind == KIND_QUAD)
    {
        // a convex quadrilateral (corners in uv.xy, uv.zw, radii.xy, radii.zw): the distance to it is the largest
        // distance outside any of its edges
        float2 q[4] = {i.data.xy, i.data.zw, i.radii.xy, i.radii.zw};
        float area = 0.0;
        [unroll] for (int k = 0; k < 4; k++)
            area += q[k].x * q[(k + 1) % 4].y - q[(k + 1) % 4].x * q[k].y;
        float side = area > 0.0 ? 1.0 : -1.0;
        float d = -1e9;
        [unroll] for (int k = 0; k < 4; k++)
        {
            float2 e = q[(k + 1) % 4] - q[k];
            float2 outward = side * float2(e.y, -e.x) / max(length(e), 1e-5);
            d = max(d, dot(i.pixel - q[k], outward));
        }
        float a = saturate(0.5 - d) * i.color0.a;
        return float4(i.color0.rgb * a, a);
    }

    if (i.kind == KIND_ARC)
    {
        float2 centre = (i.rect.xy + i.rect.zw) * 0.5;
        float d = sd_arc(i.pixel - centre, i.data.x, i.data.y, i.data.z) - i.data.w * 0.5;
        float a = saturate(0.5 - d) * i.color0.a;
        return float4(i.color0.rgb * a, a);
    }

    if (i.kind == KIND_GLYPH)
    {
        float a;
        if (i.params.y > 0.0)
            a = blurred_glyph(i.uv / atlas_inv_size, i.data, i.params.y);
        else
            a = glyph_atlas.Sample(atlas_sampler, i.uv);
        a = text_coverage(a, i.color0.rgb) * i.color0.a;
        return float4(i.color0.rgb * a, a);
    }

    float2 centre = (i.rect.xy + i.rect.zw) * 0.5;
    float2 half_size = (i.rect.zw - i.rect.xy) * 0.5;
    float2 p = i.pixel - centre;

    if (i.kind == KIND_SHADOW)
    {
        float sigma = max(i.params.y, 0.5);
        float a = rounded_box_shadow(p, half_size, min(i.radii.x, min(half_size.x, half_size.y)), sigma) * i.color0.a;
        a = saturate(a + dither(i.pixel));
        return float4(i.color0.rgb * a, a);
    }

    if (i.kind == KIND_IMAGE)
    {
        float d = sd_round_box(p, half_size, i.radii);
        float outer = saturate(0.5 - d);
        float border = i.params.x;
        float inner = saturate(0.5 - (d + border));
        float4 tex = image_atlas.SampleLevel(atlas_sampler, i.uv, 0);
        float4 fill_pm = float4(tex.rgb * i.color0.rgb, tex.a) * i.color0.a;
        float4 border_pm = float4(i.border_color.rgb * i.border_color.a, i.border_color.a);
        return lerp(border_pm, fill_pm, border > 0 ? inner : 1.0) * outer;
    }

    // rounded rectangle: a gradient fill inside a border; half a pixel either side of the edge is the AA band
    float d = sd_round_box(p, half_size, i.radii);
    float t = i.params.w > 0.5 ? saturate((i.pixel.x - i.rect.x) / max(i.rect.z - i.rect.x, 1.0))
                               : saturate((i.pixel.y - i.rect.y) / max(i.rect.w - i.rect.y, 1.0));
    float4 fill = lerp(i.color0, i.color1, t);
    if (any(i.color0 != i.color1))
        fill.rgb += dither(i.pixel);

    float border = i.params.x;
    float outer = saturate(0.5 - d);
    float inner = saturate(0.5 - (d + border));
    float4 fill_pm = float4(fill.rgb * fill.a, fill.a);
    float4 border_pm = float4(i.border_color.rgb * i.border_color.a, i.border_color.a);
    return lerp(border_pm, fill_pm, border > 0 ? inner : 1.0) * outer;
}

float4 ps_main(VSOut i) : SV_Target
{
    // how much of this pixel is inside the clip (1 everywhere but where a clip edge falls inside a pixel)
    float2 cover = saturate(min(i.pixel - i.clip.xy, i.clip.zw - i.pixel) + 0.5);
    return shade(i) * (cover.x * cover.y);
}
