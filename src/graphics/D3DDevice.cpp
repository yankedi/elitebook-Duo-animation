// ---------------------------------------------------------------------------
//  D3DDevice.cpp
// ---------------------------------------------------------------------------
#include "D3DDevice.h"

#include <iterator>

namespace dragonfly {

bool D3DDevice::CreateBackBuffer() {
    if (!m_swapChain || !m_device) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBufferTexture;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBufferTexture)))) {
        return false;
    }
    m_backBufferTexture = backBufferTexture;
    if (FAILED(m_device->CreateRenderTargetView(backBufferTexture.Get(), nullptr,
                                                &m_backBuffer))) {
        return false;
    }
    return true;
}

bool D3DDevice::Create(HWND window, uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Width = width;
    swapChainDesc.BufferDesc.Height = height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = window;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL obtained{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &swapChainDesc,
        &m_swapChain, &m_device, &obtained, &m_context);

    if (FAILED(result)) {
        // 11_1 requires Windows 8+; retry without it for safety.
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1,
            static_cast<UINT>(std::size(levels)) - 1, D3D11_SDK_VERSION,
            &swapChainDesc, &m_swapChain, &m_device, &obtained, &m_context);
    }
    if (FAILED(result)) {
        return false;
    }

    return CreateBackBuffer();
}

void D3DDevice::Resize(uint32_t width, uint32_t height) {
    if (!m_swapChain || width == 0 || height == 0) {
        return;
    }
    if (width == m_width && height == m_height) {
        return;
    }

    if (m_context) {
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    m_backBuffer.Reset();

    if (FAILED(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
        return;
    }

    m_width = width;
    m_height = height;
    CreateBackBuffer();
}

void D3DDevice::Destroy() {
    m_backBuffer.Reset();
    m_backBufferTexture.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
    m_width = 0;
    m_height = 0;
}

void D3DDevice::BeginFrame(float r, float g, float b, float a) {
    if (!m_context || !m_backBuffer) {
        return;
    }

    const float clearColor[4] = {r, g, b, a};
    m_context->ClearRenderTargetView(m_backBuffer.Get(), clearColor);
    m_context->OMSetRenderTargets(1, m_backBuffer.GetAddressOf(), nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);
}

void D3DDevice::Present(bool vsync) {
    if (m_swapChain) {
        m_swapChain->Present(vsync ? 1u : 0u, 0);
    }
}

} // namespace dragonfly
