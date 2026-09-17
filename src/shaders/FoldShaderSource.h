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

    float  parallax;            // 0 = picture glued to the pane (old model),
                                // 1 = fully anchored in the room
    float  eyeUpPx;             // eye height above the hinge, along the plane
    float  edgeFade;            // minimum feather at the picture's edge, pixels
    float  screenDepthPx;       // how far behind the pane's anchored plane the
                                // picture hangs; 0 puts it back on that plane,
                                // which is what made it swell as the lid closed
};

Texture2D    contentTexture : register(t0);
SamplerState contentSampler : register(s0);

static const float GOLDEN_ANGLE = 2.39996322972865332;
static const float TWO_PI = 6.28318530717958648;

// What is behind the picture.  The reference puts a near-black void there, and
// that is what makes the desktop read as an object sitting in space instead of
// a wallpaper stretched across the pane -- the single most important difference
// between "a screen in a virtual room" and "a smeared screenshot".
static const float3 VOID_COLOUR = float3(0.02, 0.035, 0.05);

float3 SampleRgb(float2 pixel)
{
    return contentTexture.Sample(contentSampler, pixel / resolution).rgb;
}

// Same as SampleRgb but off a prefiltered level of the mip chain, which is what
// makes a large radius smooth instead of speckled.
float3 SampleLevelRgb(float2 pixel, float lod)
{
    return contentTexture.SampleLevel(contentSampler, pixel / resolution, lod).rgb;
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

    // Past the activation angle the desktop is simply being used and must be
    // left alone.  The delta is signed: closing swings the pane one way and
    // opening swings it the other, and the reference renders both.
    const float delta = clamp(angleDelta, -0.65, 1.25);
    if (resolution.x <= 1.0 || resolution.y <= 1.0 || abs(delta) < 1e-4)
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
    // plane.  That is all the geometry the picture needs: where the pane is.
    const float hingeY = (hingeFromTop > 0.5) ? 0.0 : resolution.y;
    const float side = (hingeFromTop > 0.5) ? 1.0 : -1.0;
    const float gap = d * sin(delta);

    // ---- the picture --------------------------------------------------------
    // The picture is held in the *eye's* frame: a fixed angular rectangle, sized
    // to exactly what the pane subtends when it is at the anchor.  So turning
    // the pane changes nothing about the picture itself -- it neither swells nor
    // slides -- and what moves is the pane, which reveals more of the space
    // around the picture as it swings towards the eye.
    //
    // The earlier construction intersected the ray with the anchored *plane*.
    // That is a window, and a window coming closer magnifies what is behind it:
    // the picture grew as the lid closed, which reads as being stretched upward
    // and tipped away.  Holding it at a fixed angle is what "the picture does
    // not move, only the glass does" actually means.
    //
    // Both sub-planes are done in (distance-along-the-plane, distance-from-the-
    // eye) coordinates: the angle off the eye's axis is atan2 of the in-plane
    // offset over the forward distance, and the picture's own span is the same
    // angle measured at the anchor.
    const float forward = eyeDistancePx - gap;   // distance from the eye to the pane
    if (forward <= 1.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    // Vertical: 0 at the hinge, up to resolution.y at the far edge.
    const float angleUp = atan2(d * cos(delta) - eyeUpPx, forward);
    const float anchorLow = atan2(-eyeUpPx, eyeDistancePx);
    const float anchorHigh = atan2(resolution.y - eyeUpPx, eyeDistancePx);
    const float pictureV = (angleUp - anchorLow) / (anchorHigh - anchorLow);

    // Horizontal, the same way about the pane's centre line.
    const float halfWidth = resolution.x * 0.5;
    const float angleAcross = atan2(fragCoord.x - halfWidth, forward);
    const float anchorLeft = atan2(-halfWidth, eyeDistancePx);
    const float anchorRight = atan2(halfWidth, eyeDistancePx);
    const float pictureU = (angleAcross - anchorLeft) / (anchorRight - anchorLeft);

    // 0 at the picture's top, 1 at its bottom, matching the desktop texture.
    const float2 pictureUv = float2(pictureU, 1.0 - pictureV);

    // parallax = 0 glues the picture to the pane; 1 leaves it hanging in place.
    const float2 screenUv = lerp(uv, pictureUv, saturate(parallax));
    const float2 coord = screenUv * resolution;

    // Where the window has moved past the picture, the sample is clamped and
    // the area falls back to the void; the feather is computed once the blur
    // radius is known, further down.

    // ---- scatter ------------------------------------------------------------
    // The gap between the pane and the content plane is what scatters: it grows
    // with sin(delta) and with distance from the hinge, so the far edge is the
    // hazy end and the hinge stays crisp.  The display height normalises it, so
    // the look does not change with resolution.
    const float tilt = abs(sin(delta));
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
    const float2 contentUv = coord / resolution;

    // The dispersion runs away from the pane's centre, which is where the eye is.
    const float2 eyeCentre = resolution * 0.5;

    // The picture is a finite rectangle in space, and the reference blurs its
    // boundary by the same amount it blurs the picture -- three sigma on either
    // side, which is what a Gaussian would do to an edge.  edgeFade is a floor,
    // so the boundary is never razor sharp even where the pane is clear.
    const float sigmaUnits = radius * resolution.y / 1000.0;
    const float2 feather =
        max(float2(3.0, 3.0) * sigmaUnits / resolution,
            float2(edgeFade, edgeFade) / resolution);
    const float2 coverage =
        smoothstep(-feather, feather, contentUv) *
        (1.0 - smoothstep(1.0 - feather, 1.0 + feather, contentUv));
    const float mask = coverage.x * coverage.y;

    float3 scattered;
    const float lod = max(0.0, log2(max(1.0, radius / 3.0)));

    if (radius < 0.5)
    {
        scattered = SampleRgb(clamped);
    }
    else
    {
        // A Gaussian pyramid (the mip chain) for the bulk of the haze, plus a
        // handful of rotated taps for structure.  Gathering the whole kernel by
        // hand leaves visible speckle at these radii; the pyramid is smooth and
        // costs one sample.
        const float tapsF = 8.0;
        const float rotation =
            frac(sin(dot(fragCoord, float2(12.9898, 78.233))) * 43758.5453) * TWO_PI;

        float3 sum = SampleLevelRgb(clamped, lod) * 2.0;
        float weightSum = 2.0;

        [loop]
        for (int i = 0; i < 8; ++i)
        {
            const float fi = float(i);
            const float r = radius * sqrt((fi + 0.5) / tapsF);
            const float a = fi * GOLDEN_ANGLE + rotation;
            const float2 tap = clamp(clamped + r * float2(cos(a), sin(a)),
                                     float2(0.0, 0.0), maxCoord);
            const float w = 1.0 - 0.5 * (fi / tapsF);
            sum += SampleLevelRgb(tap, lod) * w;
            weightSum += w;
        }

        scattered = sum / weightSum;
        scattered = lerp(scattered, Luminance(scattered).xxx, scatterDesaturation);
    }

    // Where the pane is nearly clear the picture stays sharp and the dispersion
    // is the thing that gives the glass away; where it is thick the dispersion
    // disappears into the scatter anyway.
    const float2 radial = normalize(clamped - eyeCentre + float2(1e-4, 0.0));
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

    // ---- behind the picture -------------------------------------------------
    // Past the picture's own rectangle there is nothing to show, and the
    // reference is explicit about what belongs there: a near-black void.  That
    // is the whole trick -- it turns the desktop into a lit rectangle hanging in
    // space and the panel into the glass in front of it, instead of a wallpaper
    // stretched to fill whatever the projection happens to cover.
    return float4(lerp(VOID_COLOUR, colour, mask), 1.0);
}
)HLSL";

} // namespace dragonfly
