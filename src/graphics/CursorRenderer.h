// ---------------------------------------------------------------------------
//  CursorRenderer.h
//
//  Draws the mouse pointer into the captured desktop, so the effect can include
//  it.
//
//  The pointer is not part of the desktop image: it lives on its own plane and
//  the display controller composites it afterwards, which is why a captured
//  frame has no cursor in it at all.  An effect that samples only the frame
//  therefore leaves a sharp, unmoving cursor floating in front of the glass --
//  which is exactly what breaks the illusion.  The duplication hands over the
//  shape and the position separately, and this puts them back into the picture
//  before the pane is rendered.
//
//  Both shape kinds end up as one straight-alpha BGRA bitmap: colour shapes are
//  copied, and the two bit-planes of a monochrome shape are expanded on the CPU
//  (the "invert the destination" case becomes transparent, which is close enough
//  at cursor size and avoids a second blend mode).
// ---------------------------------------------------------------------------
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

#include "../capture/DesktopCapture.h"

namespace dragonfly {

class CursorRenderer {
public:
    bool Create(ID3D11Device* device);
    void Destroy();

    // Rebuilds the shape texture when the pointer's shape changed.  Returns
    // true when there is something drawable.
    bool UpdateShape(const DesktopPointer& pointer);

    // Draws the pointer into `target` (mip 0 of the content texture) at the
    // position the duplication reported.  Does nothing when it is not visible.
    void Draw(ID3D11DeviceContext* context, ID3D11Texture2D* target,
              const DesktopPointer& pointer);

    bool Ready() const { return m_texture != nullptr; }
    const char* LastError() const { return m_lastError; }

private:
    bool UploadShape(const DesktopPointer& pointer);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_view;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_targetView;
    ID3D11Texture2D* m_targetTexture = nullptr;

    uint64_t m_shapeVersion = 0;
    bool m_ready = false;
    const char* m_lastError = "";
};

} // namespace dragonfly
