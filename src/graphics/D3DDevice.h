// ---------------------------------------------------------------------------
//  D3DDevice.h
//
//  Minimal Direct3D 11 device + swap chain wrapper for the debug window.
//  This is intentionally NOT the final renderer: it only exists so the fold
//  progress can be inspected visually.
// ---------------------------------------------------------------------------
#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdint>

namespace dragonfly {

class D3DDevice {
public:
    bool Create(HWND window, uint32_t width, uint32_t height);
    void Resize(uint32_t width, uint32_t height);
    void Destroy();

    void BeginFrame(float r, float g, float b, float a);
    void Present(bool vsync);

    bool Valid() const { return m_device != nullptr && m_swapChain != nullptr; }

    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }
    ID3D11RenderTargetView* BackBuffer() const { return m_backBuffer.Get(); }
    // The back buffer texture itself, for cases that need to copy out of it.
    ID3D11Texture2D* BackBufferTexture() const { return m_backBufferTexture.Get(); }

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    bool CreateBackBuffer();

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backBuffer;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_backBufferTexture;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace dragonfly
