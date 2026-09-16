// ---------------------------------------------------------------------------
//  FoldRenderer.h
//
//  Draws the captured desktop through the fold shader.
//
//  Input  : a desktop texture (from DesktopCapture) and the hinge angle.
//  Output : the folded image, rendered into whatever target the caller binds.
//
//  The hinge angle arrives from the sensor pipeline as 0..180 where 180 means
//  "flat".  At 180 the shader is bypassed entirely and the desktop passes
//  through untouched -- that is what makes the effect disappear cleanly instead
//  of leaving a faint blur behind.
// ---------------------------------------------------------------------------
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace dragonfly {

// Tunables.  The first three come from Duo-animation's FoldParameters, which is
// where the reference project landed after tuning on real hardware.
struct FoldEffectParameters {
    // Largest angle delta handed to the shader, in degrees.
    //
    // 90 is where sin() peaks: over the range a laptop actually shows --
    // 110 degrees of hinge down to about 25 -- the blur keeps responding to the
    // lid, and any plateau lands below 20 degrees, where the panel faces the
    // keyboard.
    float maxDeltaDegrees = 90.0f;

    // Blur radius per 1000 px of display height, at the largest delta.
    // This is lid-plane's constant (65); the shader turns it into pixels using
    // the actual display height, so the look scales with panel size.
    float blurStrength = 65.0f;

    // Fraction of light lost per pixel of blur radius.
    float darkening = 0.015f;

    // Darkest the scattered light is allowed to get.  The reference lets it
    // reach zero, which turns the pane into a dark filter; glass is a lit
    // surface, so the scatter bottoms out and the reflection adds light back.
    float attenuationFloor = 0.72f;

    // ---- glass cues, see FoldShaderSource.h ------------------------------
    // Reflection wash at full tilt.  A Fresnel surface reflects more the
    // further it is from the plane, which is what makes this grow with delta.
    float sheenStrength = 0.22f;

    // Brightness of the rim where the pane ends (the far edge), at full tilt.
    float edgeGlow = 0.34f;

    // Per-channel radial offset at full tilt, in pixels: red one way, blue the
    // other.  This is the colour fringe that says "glass" rather than "blur".
    float dispersionPx = 2.4f;

    // How colourless the scattered light becomes.  Light that has been through
    // frosted glass loses its colour.
    float scatterDesaturation = 0.35f;

    // Eye distance from the content plane, in pixels.
    //
    // This is the parameter that decides whether the picture is anchored in the
    // room or glued to the panel.  At 6.4x the panel width (the number taken
    // from the reference, about two metres) the parallax is a fraction of a
    // percent and the effect collapses into "keystone plus blur".  At a real
    // viewing distance the far edge swings through the projection hard enough
    // for the picture to visibly stay put while the panel turns.
    float eyeDistancePx = 3000.0f;

    // Eye height above the hinge, along the content plane, in pixels.  Defaults
    // to half the panel height: a user sits with their eye roughly level with
    // the middle of the screen.
    float eyeUpPx = 540.0f;

    // 0 = picture glued to the panel (the model used before), 1 = anchored in
    // the body frame.  Everything in between cross-fades.
    float parallax = 1.0f;

    // Width of the fade where the sample runs past the content plane, in pixels.
    // Without it the clamped border smears; with it the edge reads as the pane
    // catching light.
    float edgeFadePx = 60.0f;

    // A laptop lid hinges on the bottom edge of the panel.
    bool hingeFromTop = false;
};

class FoldRenderer {
public:
    bool Create(ID3D11Device* device, uint32_t width, uint32_t height);
    void Destroy();
    void Resize(uint32_t width, uint32_t height);

    // Renders `desktop` with the effect applied into `target`.
    //
    // angleDeltaDeg is how far the lid has closed BELOW the activation angle:
    // zero or negative means the desktop is untouched and the shader passes the
    // frame straight through.
    void Render(ID3D11DeviceContext* context,
                ID3D11Texture2D* desktop,
                ID3D11RenderTargetView* target,
                float angleDeltaDeg,
                const FoldEffectParameters& parameters);

    // Clamps a raw delta to the range the shader accepts.
    static float ClampDelta(float angleDeltaDeg,
                            const FoldEffectParameters& parameters);

    bool Ready() const { return m_ready; }
    const char* LastError() const { return m_lastError; }

private:
    bool CreatePipeline(ID3D11Device* device);
    bool BindContent(ID3D11Texture2D* desktop);

    // Not owned; the caller's device outlives this renderer.
    ID3D11Device* m_device = nullptr;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_contentView;
    ID3D11Texture2D* m_contentTexture = nullptr;  // cached, not owned
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_ready = false;
    const char* m_lastError = "";
};

} // namespace dragonfly
