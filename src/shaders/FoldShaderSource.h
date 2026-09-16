// ---------------------------------------------------------------------------
//  FoldShaderSource.h
//
//  HLSL for the fold effect.
//
//  The model is taken from the two reference implementations that document it:
//
//    Duo-animation  app/src/main/res/raw/duo_fold.agsl
//    duo-open       app/src/main/res/raw/duo_unfold.agsl
//
//  Their shared model, restated:
//    * the content lives on a fixed plane (here: the captured desktop);
//    * the eye is stationary on that plane's normal, `eyeDistance` back from it;
//    * the glass pane is rotated about the hinge line, its far edge rising
//      towards the viewer;
//    * per pixel a ray is traced from the eye through the glass point and
//      continued to the content plane; the intersection is what gets sampled;
//    * blur radius grows with the glass-to-plane gap, and the result is dimmed
//      in proportion to how much it scatters (a fully-missed kernel is black).
//
//  Deliberate differences for this machine:
//    * a laptop lid hinges on a HORIZONTAL line at the bottom edge, so the work
//      happens in transposed coordinates -- this is duo-open's `axisSwap` idea
//      applied unconditionally;
//    * there is no pane side or hinge position to resolve: the hinge is always
//      the bottom edge of the panel;
//    * the blur loop is fixed-length with a weight mask rather than an early
//      break, matching what both reference shaders do for compiler safety;
//    * the content is a texture sampled with a linear/clamp sampler instead of a
//      shader object evaluated at a coordinate.
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
    // uv.y is 0 at the top, 1 at the bottom, matching how the desktop texture
    // and the hinge convention are defined.
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)HLSL";

inline constexpr char kFoldPixelShader[] = R"HLSL(
cbuffer FoldConstants : register(b0)
{
    float2 resolution;      // output size in pixels
    float  foldAngle;       // pane tilt away from the content plane, radians
                            //   0    -> pane coincides with the plane (off)
                            //   pi/2 -> pane perpendicular to it
    float  eyeDistancePx;   // eye distance from the plane, in pixels

    float  blurSpread;      // blur radius gained per pixel of glass/plane gap
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

    const float tilt = clamp(foldAngle, 0.0, 1.5707963);
    if (resolution.x <= 1.0 || resolution.y <= 1.0 || tilt < 1e-4)
    {
        return direct;
    }

    const float2 fragCoord = uv * resolution;

    // The hinge is a horizontal line on one edge of the panel.
    const float hingeY = (hingeFromTop > 0.5) ? 0.0 : resolution.y;
    const float side = (hingeFromTop > 0.5) ? 1.0 : -1.0;
    const float d = abs(fragCoord.y - hingeY);

    // The pane rotated about the hinge; its far edge lifts towards the eye.
    const float2 glass = float2(fragCoord.x, hingeY + side * d * cos(tilt));
    const float gap = d * sin(tilt);

    const float2 eye = resolution * 0.5;

    // Ray eye -> glass point, continued until it meets the content plane.
    const float depth = eyeDistancePx - gap;
    if (depth <= 1e-3)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 hit = eye + (glass - eye) * (eyeDistancePx / depth);

    const float radius = blurSpread * gap;

    // Frosted glass absorbs light in proportion to how much it scatters.
    //
    // The reference constant (0.015 per pixel of radius) was tuned on a 70 mm
    // panel where the largest kernel is only ~36 px.  A 1080p laptop panel
    // produces a ~92 px kernel at the same angle, and 0.015 * 92 would drive
    // the whole frame to black.  The floor keeps the absorption readable while
    // staying faithful to the "dimming grows with scattering" behaviour.
    const float atten = max(1.0 - darkening * radius, 0.35);

    // Clamp the sampled point to the content plane.  The projection magnifies
    // the frame by t = eye / (eye - gap), so near the hinge the ray can land
    // outside the captured desktop; clamping smears the edge pixels instead of
    // dropping to black, which is both what glass does and what keeps the frame
    // readable.  The reference shaders return black here -- fine for a 70 mm
    // phone at 45 degrees, but on a 1080p panel it costs a third of the frame.
    const float2 maxCoord = resolution - 1.0;
    const float clampRadius = max(radius, 1.0);
    const float2 lowBound = -clampRadius;
    const float2 highBound = maxCoord + clampRadius;

    if (hit.x < lowBound.x || hit.y < lowBound.y ||
        hit.x > highBound.x || hit.y > highBound.y)
    {
        // So far outside that even the smeared edge is meaningless.
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
