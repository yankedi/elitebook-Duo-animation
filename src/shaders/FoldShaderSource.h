// ---------------------------------------------------------------------------
//  FoldShaderSource.h
//
//  HLSL for the lid effect.
//
//  The model is iphone-duo's (see src/ATTRIBUTION.md): the screen is a *window*,
//  and the picture lives on a plane that is fixed in the body's frame -- the
//  room, not the panel.  Its fragment shader is the clearest statement of it:
//
//      vec3 ray = displayPosition - displayCamera;
//      vec3 intersection = displayCamera + ray * (-displayCamera.z / rayDepth);
//      vec2 planeUv = intersection.xy / planeSize + 0.5;
//      vec2 projectedUv = mix(screenUv, planeUv, parallax * projection);
//
//  A ray is cast from the eye through the *moving* pixel and intersected with a
//  plane that does not move, and that intersection is what gets sampled.  So
//  when the lid tilts, the content slides across the panel exactly as a picture
//  on a wall slides across a window you are turning.  lid-plane states the same
//  illusion from the other end: "the content holds its angle while the physical
//  display tilts around it".
//
//  The one thing that makes or breaks it is the eye distance.  Put the eye two
//  metres away and the parallax is a fraction of a percent -- the picture looks
//  glued to the panel and all that is left is a keystone and a blur.  At a
//  believable viewing distance (about 450 mm) the far edge of the panel swings
//  through the projection hard enough for the picture to visibly stay put.
//
//  On top of that model sits the glass: the blur that comes from the gap
//  between the pane and the content plane (the same quantity duo-open uses), and
//  five cues taken from real glass -- dispersion, a Fresnel-style reflection
//  wash, a highlight band that sweeps with the tilt, an edge rim, and scattered
//  light losing its colour.  Where the picture runs out beyond the content
//  plane, the sample is clamped and the area fades into that reflection, so the
//  edge reads as the pane catching light rather than as a smeared border.
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
    float  eyeDistancePx;       // eye to content plane, pixels (a real viewing
                                // distance, not a nominal one)

    float  blurStrength;        // blur radius per 1000 px of height at max delta
    float  darkening;           // light lost per pixel of blur radius
    float  hingeFromTop;        // 1 = hinge at the top edge, 0 = at the bottom
    float  attenuationFloor;    // darkest the scattered light is allowed to get

    float  sheenStrength;       // reflection wash, scaled by sin(delta)
    float  edgeGlow;            // brightness of the far-edge rim
    float  dispersionPx;        // per-channel radial offset at full tilt
    float  scatterDesaturation; // how colourless the scattered light becomes

    float  parallax;            // 0 = picture glued to the panel (old model),
                                // 1 = fully anchored in the body frame
    float  eyeUpPx;             // eye height above the hinge, along the plane
    float  edgeFade;            // width of the fade at the content plane's edge
    float  padding;
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
    const float2 maxCoord = resolution - 1.0;

    // 0 at the edge opposite the hinge, 1 right at the hinge.
    const float height = (hingeFromTop > 0.5) ? uv.y : (1.0 - uv.y);
    const float d = height * resolution.y;   // pixels from the hinge

    // ---- the window ---------------------------------------------------------
    // The pane rotates about the hinge by delta, so a point d from the hinge
    // sits d*sin(delta) closer to the eye and d*cos(delta) shorter along the
    // plane.  The ray from the eye through that point is continued until it
    // meets the content plane, which has not moved.
    const float hingeY = (hingeFromTop > 0.5) ? 0.0 : resolution.y;
    const float side = (hingeFromTop > 0.5) ? 1.0 : -1.0;
    const float2 pane = float2(fragCoord.x, hingeY + side * d * cos(delta));
    const float gap = d * sin(delta);

    const float2 eye = float2(resolution.x * 0.5, hingeY + side * eyeUpPx);
    const float depth = eyeDistancePx - gap;
    if (depth <= 1e-3)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float2 anchored = eye + (pane - eye) * (eyeDistancePx / depth);

    // parallax = 0 glues the picture to the panel; 1 leaves it where it is in
    // the room.  Everything downstream works on whichever was chosen.
    const float2 coord = lerp(fragCoord, anchored, saturate(parallax));

    // Where the window has moved past the picture, the sample is clamped and
    // the area fades into the reflection instead of smearing.
    const float2 outside = max(float2(0.0, 0.0), max(-coord, coord - maxCoord));
    const float outsidePx = max(outside.x, outside.y);
    const float coverage = 1.0 - saturate(outsidePx / max(edgeFade, 1.0));

    // ---- scatter ------------------------------------------------------------
    // The gap between the pane and the content plane is what scatters: it grows
    // with sin(delta) and with distance from the hinge, so the far edge is the
    // hazy end and the hinge stays crisp.  The display height normalises it, so
    // the look does not change with resolution.
    const float tilt = sin(delta);
    const float radius = blurStrength
                       * smoothstep(0.08, 1.0, height)
                       * tilt
                       * resolution.y / 1000.0;

    // Frosted glass absorbs light in proportion to how much it scatters -- but
    // it never goes black, or the result stops looking like a surface.
    const float atten = clamp(1.0 - darkening * radius, attenuationFloor, 1.0);

    const float2 lowBound = float2(-radius, -radius);
    const float2 highBound = maxCoord + radius;
    if (coord.x < lowBound.x || coord.y < lowBound.y ||
        coord.x > highBound.x || coord.y > highBound.y)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float2 clamped = clamp(coord, float2(0.0, 0.0), maxCoord);
    float3 scattered;

    if (radius < 0.5)
    {
        scattered = SampleRgb(clamped);
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
            const float2 tap = clamp(clamped + r * float2(cos(a), sin(a)),
                                     float2(0.0, 0.0), maxCoord);
            sum += SampleRgb(tap) * w;
            weightSum += w;
        }

        scattered = sum / max(weightSum, 1.0);
        scattered = lerp(scattered, Luminance(scattered).xxx, scatterDesaturation);
    }

    // Where the pane is nearly clear the picture stays sharp and the dispersion
    // is the thing that gives the glass away; where it is thick the dispersion
    // disappears into the scatter anyway.
    const float2 radial = normalize(clamped - eye + float2(1e-4, 0.0));
    const float3 sharp = SampleDispersed(clamped, radial, dispersionPx * tilt);
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

    // Beyond the picture: the same reflection, so the pane's edge reads as glass
    // rather than as a stretched border.
    colour = lerp(reflectionTint * (0.5 + 0.5 * tilt), colour, coverage);

    return float4(colour, 1.0);
}
)HLSL";

} // namespace dragonfly
