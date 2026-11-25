// Pre-HUD screen-space water droplets (gothamor#212 / Xbox Rain Droplets).
// Refracts the 3D scene texture with organic bead distortion, rim darkening, and sky catchlights.

cbuffer DropConsts : register(b0)
{
    float4 uvBounds;  // xy: viewport min UV in scene texture, zw: viewport max UV
    float4 texelSize; // xy: 1.0 / scene texture size, z: display aspect (w/h), w: unused
};

struct VSIn
{
    float2 pos : POSITION;      // NDC (-1..1)
    float2 local : TEXCOORD0;   // quad-local (-1..1), +y = bottom (gravity bulb)
    float4 uvRect : TEXCOORD1;  // xy: drop center UV in scene texture, zw: UV refraction amplitude
    float4 props : TEXCOORD2;   // x: opacity (0..1), y: trail factor (0..1), z: organic seed (0..1), w: tint (0..1)
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 local : TEXCOORD0;
    float4 uvRect : TEXCOORD1;
    float4 props : TEXCOORD2;
};

VSOut vs_main(VSIn i)
{
    VSOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.local = i.local;
    o.uvRect = i.uvRect;
    o.props = i.props;
    return o;
}

Texture2D<float4> sceneTex : register(t0);
SamplerState sceneSampler : register(s0);

float4 ps_main(VSOut i) : SV_Target0
{
    float2 p = i.local;
    const float trail = saturate(i.props.y);
    const float seed = i.props.z;

    // Slight teardrop elongation toward the top tail (-y) when sliding downward.
    const float taper = 1.0 + trail * 0.42 * saturate(0.45 - p.y * 0.5);
    p.x *= taper;

    // Subtle organic contour perturbation so beads are not uniform mathematical circles.
    const float ang = atan2(p.y, p.x);
    const float phase = seed * 6.2831853;
    const float wobble = 1.0 + 0.045 * sin(ang * 2.0 + phase) + 0.025 * cos(ang * 3.0 - phase * 1.7);
    const float2 q = p / wobble;
    const float r2 = dot(q, q);
    if (r2 >= 1.0)
        discard;

    const float r = sqrt(r2);
    const float z = sqrt(max(0.0, 1.0 - r2));
    const float3 n = normalize(float3(q.x, q.y, z * 0.72));

    // Convex water refraction + subtle lens magnification towards the bead center.
    const float2 uvMargin = texelSize.xy * 0.5;
    const float2 uvLo = uvBounds.xy + uvMargin;
    const float2 uvHi = uvBounds.zw - uvMargin;
    const float lens = 0.85 + 0.35 * (1.0 - z);
    const float2 uvCenter = i.uvRect.xy;
    const float2 refractUv = uvCenter - n.xy * i.uvRect.zw * lens;

    float3 col = sceneTex.Sample(sceneSampler, clamp(refractUv, uvLo, uvHi)).rgb;
    // Slight chromatic fringe near droplet perimeter for sharper optical separation against bright backgrounds.
    const float fringe = smoothstep(0.45, 0.95, r) * 0.14;
    if (fringe > 0.001)
    {
        const float2 disp = n.xy * i.uvRect.zw * fringe;
        const float rChan = sceneTex.Sample(sceneSampler, clamp(refractUv - disp, uvLo, uvHi)).r;
        const float bChan = sceneTex.Sample(sceneSampler, clamp(refractUv + disp, uvLo, uvHi)).b;
        col.r = lerp(col.r, rChan, 0.45);
        col.b = lerp(col.b, bChan, 0.45);
    }

    // Internal Fresnel rim darkening, top dome sky highlight, and lower refractive caustic arc.
    const float rimDark = smoothstep(0.58, 0.96, r) * 0.15;
    const float fresnel = pow(1.0 - saturate(n.z), 3.0);
    const float spec = pow(saturate(dot(n, normalize(float3(-0.32, -0.58, 0.75)))), 18.0) * 0.30;
    const float caustic = pow(saturate(dot(n, normalize(float3(0.22, 0.74, 0.63)))), 7.0) * 0.12;
    col = saturate(col * (1.0 - rimDark) + (spec + caustic + fresnel * 0.05) * (0.75 + 0.25 * i.props.w));

    const float edgeAlpha = 1.0 - smoothstep(0.84, 1.0, r);
    const float alpha = saturate(edgeAlpha * i.props.x);
    return float4(col, alpha);
}
