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
    // Eye distance from the content plane, in pixels.  The reference uses
    // 450 mm; converted here with the display's own pixel density.
    float eyeDistancePx = 1700.0f;

    // Blur radius gained per pixel of glass-to-plane gap (reference: 0.12).
    float blurSpread = 0.12f;

    // Light lost per pixel of blur radius (reference: 0.015).
    float darkening = 0.015f;

    // Largest pane tilt handed to the shader.  The physical angle keeps growing
    // below 90 degrees, but the shader saturates, so duo-open maps the visible
    // range onto a fixed tilt instead of clamping (it uses 45 degrees).  The
    // same idea is used here with a slightly larger cap.
    float maxTiltDegrees = 62.0f;

    // A laptop lid hinges on the bottom edge of the panel.
    bool hingeFromTop = false;
};

class FoldRenderer {
public:
    bool Create(ID3D11Device* device, uint32_t width, uint32_t height);
    void Destroy();
    void Resize(uint32_t width, uint32_t height);

    // Renders `desktop` with the fold applied into `target`.
    // hingeAngleDeg: 180 = flat (effect off), smaller = more folded.
    void Render(ID3D11DeviceContext* context,
                ID3D11Texture2D* desktop,
                ID3D11RenderTargetView* target,
                float hingeAngleDeg,
                const FoldEffectParameters& parameters);

    // The tilt the shader would receive for a given hinge angle, in degrees.
    // Exposed so the caller can print it next to the sensor values.
    static float TiltDegreesForHinge(float hingeAngleDeg,
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
