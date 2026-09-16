// ---------------------------------------------------------------------------
//  FoldShaderSource.h
//
//  HLSL for the lid effect.
//
//  The strategy is taken from lid-plane (jh3y/lid-plane, GPL-3.0-or-later):
//  hold the desktop at an apparent fixed angle and blur it progressively as the
//  lid closes below an activation angle.  Its README states the illusion: the
//  content holds its angle while the physical display tilts around it.
//
//  The implementation here is independent -- different capture API, different
//  renderer, HLSL instead of Metal -- but the model is deliberately the same:
//
//    * `height` is 0 at the edge opposite the hinge and 1 at the hinge, matching
//      lid-plane's `height = 1.0 - uv.y`;
//    * the glass pane rotates about the hinge by the angle delta, its far edge
//      lifting towards the viewer;
//    * per pixel a ray is traced from the eye through the glass point and
//      continued to the content plane, so the picture appears to keep the
//      activation angle instead of following the panel;
//    * the blur radius grows with sin(delta) and with height, so the area near
//      the hinge stays crisp;
//    * the radius is normalised by display height, which is what makes the look
//      scale with panel size rather than with resolution.
//
//  Deliberate differences:
//    * a laptop lid hinges on a HORIZONTAL line at the bottom edge;
//    * the blur is a Vogel disk gathered in one pass rather than four
//      pre-blurred mip levels blended together -- cheaper to build here and the
//      result is visually equivalent at this radius;
//    * no keystone/perspective taper mode; the projection stays the plain
//      pinhole form, which is the "Hold Content Angle" default.
// ---------------------------------------------------------------------------
#pragma once

namespace dragonfly {

// Full-screen triangle.  Three vertices cover the viewport with no vertex
// buffer at all, and the UV is derived from the vertex index.
inline constexpr char kFoldVertexShader[] = R"HLSL(
struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    // (0,0) (2,0) (0,2) -> a triangle covering the whole clip square.
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);

    VSOutput output;
    output.uv = uv;
    // uv.y is 0 at the top of the screen and 1 at the bottom, matching the
    // desktop texture and the hinge convention.
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)HLSL";

inline constexpr char kFoldPixelShader[] = R"HLSL(
cbuffer EffectConstants : register(b0)
{
    float2 resolution;      // output size in pixels
    float  angleDelta;      // radians the lid has closed past the activation
                            // angle; 0 or less means the effect is off
    float  eyeDistancePx;   // eye distance from the content plane, pixels

    float  blurStrength;    // blur radius per 1000 px of height at max delta
    float  darkening;       // light lost per pixel of blur radius
    float  hingeFromTop;    // 1 = hinge at the top edge, 0 = at the bottom
    float  padding;
};

Texture2D    contentTexture : register(t0);
SamplerState contentSampler : register(s0);

static const float GOLDEN_ANGLE = 2.39996322972865332;
static const float TWO_PI = 6.28318530717958648;

float4 SampleContent(float2 pixel)
{
    return contentTexture.Sample(contentSampler, pixel / resolution);
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    float4 direct = contentTexture.Sample(contentSampler, uv);

    // Above the activation angle the desktop is simply being used and must be
    // left alone.
    const float delta = clamp(angleDelta, 0.0, 1.5707963);
    if (resolution.x <= 1.0 || resolution.y <= 1.0 || delta < 1e-4)
    {
        return direct;
    }

    const float2 fragCoord = uv * resolution;

    // 0 at the edge opposite the hinge, 1 right at the hinge.
    const float height = (hingeFromTop > 0.5) ? uv.y : (1.0 - uv.y);
    const float d = height * resolution.y;   // pixels from the hinge

    // The pane rotated about the hinge, its far edge lifting towards the eye.
    const float hingeY = (hingeFromTop > 0.5) ? 0.0 : resolution.y;
    const float side = (hingeFromTop > 0.5) ? 1.0 : -1.0;
    const float2 glass = float2(fragCoord.x, hingeY + side * d * cos(delta));
    const float gap = d * sin(delta);

    // Ray eye -> glass point, continued until it meets the content plane.
    const float2 eye = resolution * 0.5;
    const float depth = eyeDistancePx - gap;
    if (depth <= 1e-3)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 hit = eye + (glass - eye) * (eyeDistancePx / depth);

    // lid-plane's calibration: the radius grows with sin(delta), the display
    // height keeps it independent of resolution, and the smoothstep leaves the
    // last few percent near the hinge sharp.
    const float radius = blurStrength
                       * smoothstep(0.08, 1.0, height)
                       * sin(delta)
                       * resolution.y / 1000.0;

    // Frosted glass absorbs light in proportion to how much it scatters.
    const float atten = max(1.0 - darkening * radius, 0.35);

    const float2 maxCoord = resolution - 1.0;
    const float2 lowBound = float2(-radius, -radius);
    const float2 highBound = maxCoord + radius;

    if (hit.x < lowBound.x || hit.y < lowBound.y ||
        hit.x > highBound.x || hit.y > highBound.y)
    {
        // Far enough outside that even the smeared edge is meaningless.
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    if (radius < 0.5)
    {
        return float4(SampleContent(clamp(hit, float2(0.0, 0.0), maxCoord)).rgb * atten,
                      1.0);
    }

    // Vogel disk with a per-pixel rotation, so banding reads as glass grain.
    const float tapsF = clamp(radius * 2.0, 6.0, 32.0);
    const float rotation =
        frac(sin(dot(fragCoord, float2(12.9898, 78.233))) * 43758.5453) * TWO_PI;

    float3 sum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;

    [loop]
    for (int i = 0; i < 32; ++i)
    {
        const float fi = float(i);
        const float w = 1.0 - step(tapsF, fi);
        const float r = radius * sqrt((fi + 0.5) / tapsF);
        const float a = fi * GOLDEN_ANGLE + rotation;
        const float2 tap = clamp(hit + r * float2(cos(a), sin(a)),
                                 float2(0.0, 0.0), maxCoord);
        sum += SampleContent(tap).rgb * w;
        weightSum += w;
    }

    return float4(sum / max(weightSum, 1.0) * atten, 1.0);
}
)HLSL";

} // namespace dragonfly
