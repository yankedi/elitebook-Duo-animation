// ---------------------------------------------------------------------------
//  FoldShaderSource.h
//
//  HLSL for the lid effect.
//
//  Strategy is taken from lid-plane (jh3y/lid-plane, GPL-3.0-or-later): hold
//  the desktop at an apparent fixed angle and blur it progressively as the lid
//  closes below an activation angle.  Its README states the illusion: the
//  content holds its angle while the physical display tilts around it.
//
//  The implementation here is independent -- different capture API, different
//  renderer, HLSL instead of Metal -- but the model is deliberately the same:
//
//    * `height` is 0 at the hinge and 1 at the far edge, matching lid-plane's
//      `height = 1.0 - uv.y`;
//    * the pane rotates about the hinge by the angle delta, its far edge
//      lifting towards the viewer;
//    * per pixel a ray is traced from the eye through the pane and continued to
//      the content plane, so the picture appears to keep the activation angle
//      instead of following the panel;
//    * the blur radius grows with sin(delta) and with height, so the area near
//      the hinge stays crisp;
//    * the radius is normalised by display height, which is what makes the look
//      scale with panel size rather than with resolution.
//
//  On top of that model sit the cues that make the result read as *glass*
//  rather than as frosted plastic.  All five are the same trick: real glass is
//  a slab with two surfaces, and none of them can be had from blur alone.
//
//    1. dispersion -- glass bends each wavelength by a slightly different
//       amount, which is where the colour fringe on the edge of a window pane
//       comes from.  Applied only where the image is still sharp; inside a
//       heavy blur it is invisible anyway.
//    2. reflection sheen -- a reflection that grows with the tilt (a Fresnel
//       surface reflects more the further it is from the plane) and is
//       brightest at the far edge.
//    3. a highlight band that sweeps down the pane as the lid closes, which is
//       what a room light does when a surface tilts under it.
//    4. scattering desaturated -- light that has been through frosted glass
//       loses its colour, so the scattered part is mixed towards luminance.
//    5. attenuation with a floor: the scattered light dims but never to black,
//       and the reflection above adds brightness back, so the net effect is a
//       lit surface rather than a dark filter.
//
//  Deliberate differences from the reference:
//    * a laptop lid hinges on a HORIZONTAL line at the bottom edge;
//    * the blur is a Vogel disk gathered in one pass rather than four
//      pre-blurred mip levels blended together;
//    * the reflection is a tinted sheen rather than a captured environment,
//      which the desktop duplication API does not provide.
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
    float2 resolution;          // output size in pixels
    float  angleDelta;          // radians the lid has closed past the activation
                                // angle; 0 or less means the effect is off
    float  eyeDistancePx;       // eye distance from the content plane, pixels

    float  blurStrength;        // blur radius per 1000 px of height at max delta
    float  darkening;           // light lost per pixel of blur radius
    float  hingeFromTop;        // 1 = hinge at the top edge, 0 = at the bottom
    float  attenuationFloor;    // darkest the scattered light is allowed to get

    float  sheenStrength;       // reflection wash, scaled by sin(delta)
    float  edgeGlow;            // brightness of the far-edge rim
    float  dispersionPx;        // per-channel radial offset at full tilt
    float  scatterDesaturation; // how colourless the scattered light becomes
};

Texture2D    contentTexture : register(t0);
SamplerState contentSampler : register(s0);

static const float GOLDEN_ANGLE = 2.39996322972865332;
static const float TWO_PI = 6.28318530717958648;

float3 SampleRgb(float2 pixel)
{
    return contentTexture.Sample(contentSampler, pixel / resolution).rgb;
}

float Luminance(float3 colour)
{
    return dot(colour, float3(0.2126, 0.7152, 0.0722));
}

// Glass bends each wavelength a little differently: red lands one way, blue the
// other, green stays put.  That mismatch is the colour fringe on the edge of a
// pane, and it is the single strongest "this is glass" cue there is.  It is
// only worth paying for where the image is still sharp, so the caller mixes it
// in by blur radius.
float3 SampleDispersed(float2 pixel, float2 direction, float amount)
{
    if (amount < 0.05)
    {
        return SampleRgb(pixel);
    }

    float3 colour;
    colour.r = SampleRgb(pixel + direction * amount).r;
    colour.g = SampleRgb(pixel).g;
    colour.b = SampleRgb(pixel - direction * amount).b;
    return colour;
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
    const float2 pane = float2(fragCoord.x, hingeY + side * d * cos(delta));
    const float gap = d * sin(delta);

    // Ray eye -> pane point, continued until it meets the content plane.
    const float2 eye = resolution * 0.5;
    const float depth = eyeDistancePx - gap;
    if (depth <= 1e-3)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 hit = eye + (pane - eye) * (eyeDistancePx / depth);

    // lid-plane's calibration: the radius grows with sin(delta), the display
    // height keeps it independent of resolution, and the smoothstep leaves the
    // last few percent near the hinge sharp.
    const float tilt = sin(delta);
    const float radius = blurStrength
                       * smoothstep(0.08, 1.0, height)
                       * tilt
                       * resolution.y / 1000.0;

    // Frosted glass absorbs light in proportion to how much it scatters -- but
    // it never goes black, or the result stops looking like a surface.
    const float atten = clamp(1.0 - darkening * radius, attenuationFloor, 1.0);

    const float2 maxCoord = resolution - 1.0;
    const float2 lowBound = float2(-radius, -radius);
    const float2 highBound = maxCoord + radius;

    if (hit.x < lowBound.x || hit.y < lowBound.y ||
        hit.x > highBound.x || hit.y > highBound.y)
    {
        // Far enough outside that even the smeared edge is meaningless.
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float2 clampedHit = clamp(hit, float2(0.0, 0.0), maxCoord);
    float3 scattered;

    if (radius < 0.5)
    {
        scattered = SampleRgb(clampedHit);
    }
    else
    {
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
            const float2 tap = clamp(clampedHit + r * float2(cos(a), sin(a)),
                                     float2(0.0, 0.0), maxCoord);
            sum += SampleRgb(tap) * w;
            weightSum += w;
        }

        scattered = sum / max(weightSum, 1.0);
        scattered = lerp(scattered, Luminance(scattered).xxx, scatterDesaturation);
    }

    // Where the pane is nearly clear the picture stays sharp and the dispersion
    // is the thing that gives the glass away; where it is thick the dispersion
    // disappears into the scatter anyway.  The crossover is deliberately far
    // out: at a blur radius of 15 px the pane is still clear enough for a fringe
    // to be the point, and mixing it away by then would waste it.
    const float2 radial = normalize(clampedHit - eye + float2(1e-4, 0.0));
    const float3 sharp = SampleDispersed(clampedHit, radial, dispersionPx * tilt);
    float3 colour = lerp(sharp, scattered, saturate(radius / 32.0)) * atten;

    // ---- the second surface -------------------------------------------------
    // A reflection needs something to reflect.  Without a captured environment
    // the honest approximation is a Fresnel-style sheen -- stronger the further
    // the pane is from the plane, brightest at the far edge -- plus the bright
    // rim where the pane ends.  The tint runs from a dark room low down to a lit
    // ceiling high up, which is what a pane held over a desk actually reflects.
    const float sheen = sheenStrength * tilt * pow(saturate(height), 1.6);

    const float bandCentre = 0.92 - 0.5 * tilt;
    const float bandOffset = (height - bandCentre) * 3.2;
    const float glow = edgeGlow * tilt * exp(-bandOffset * bandOffset);

    const float3 reflectionTint =
        lerp(float3(0.16, 0.19, 0.26), float3(0.58, 0.62, 0.70), saturate(height));
    colour += (sheen + glow) * reflectionTint;

    return float4(colour, 1.0);
}
)HLSL";

} // namespace dragonfly
