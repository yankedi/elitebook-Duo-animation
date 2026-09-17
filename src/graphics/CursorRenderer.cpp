// ---------------------------------------------------------------------------
//  CursorRenderer.cpp
// ---------------------------------------------------------------------------
#include "CursorRenderer.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <vector>

namespace dragonfly {

namespace {

// Same full-screen-triangle trick as the pane shader, but the triangle is the
// pointer's rectangle: four vertices as a strip, so the corners come straight
// out of the vertex id.
const char kCursorShader[] = R"HLSL(
cbuffer CursorConstants : register(b0)
{
    float2 origin;      // top-left of the shape, in desktop pixels
    float2 size;        // shape size, in pixels
    float2 resolution;  // the render target's size
    float2 padding;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    // id 0..3 -> (0,0) (1,0) (0,1) (1,1)
    const float2 corner = float2(vertexID & 1, (vertexID >> 1) & 1);
    const float2 pixel = origin + corner * size;

    VSOutput output;
    output.position = float4(pixel / resolution * float2(2.0, -2.0) + float2(-1.0, 1.0),
                             0.0, 1.0);
    output.uv = corner;
    return output;
}

Texture2D    shapeTexture : register(t0);
SamplerState shapeSampler : register(s0);

float4 PSMain(VSOutput input) : SV_TARGET
{
    // The shape comes out of the duplication premultiplied, so it is passed
    // straight through to a ONE / INV_SRC_ALPHA blend.
    return shapeTexture.Sample(shapeSampler, input.uv);
}
)HLSL";

// Expands the two bit-planes of a monochrome pointer shape into straight-alpha
// BGRA.  The classic mask rules: both bits clear is black, only the XOR bit set
// is white, and anything with the AND bit set keeps the desktop (or inverts it,
// which is drawn as transparent here).
void ExpandMonochrome(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape,
                      const std::vector<uint8_t>& pixels,
                      std::vector<uint8_t>& out) {
    const uint32_t width = shape.Width;
    const uint32_t height = shape.Height;
    const uint32_t maskStride = ((width + 31) / 32) * 4;
    const size_t needed = static_cast<size_t>(maskStride) * height * 2;  // AND then XOR
    out.assign(static_cast<size_t>(width) * height * 4, 0);
    if (pixels.size() < needed) {
        return;
    }

    const uint8_t* andMask = pixels.data();
    const uint8_t* xorMask = pixels.data() + static_cast<size_t>(maskStride) * height;

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t byte = x / 8;
            const uint8_t bit = static_cast<uint8_t>(0x80u >> (x % 8));
            const uint8_t andBit = (andMask[y * maskStride + byte] & bit) != 0 ? 1 : 0;
            const uint8_t xorBit = (xorMask[y * maskStride + byte] & bit) != 0 ? 1 : 0;

            uint8_t* texel = out.data() + (static_cast<size_t>(y) * width + x) * 4;
            if (andBit == 0) {
                const uint8_t level = (xorBit != 0) ? 255 : 0;
                texel[0] = level;
                texel[1] = level;
                texel[2] = level;
                texel[3] = 255;
            }
            // andBit != 0: leave the desktop alone (alpha 0).
        }
    }
}

} // namespace

bool CursorRenderer::Create(ID3D11Device* device) {
    Destroy();
    if (!device) {
        m_lastError = "no D3D11 device";
        return false;
    }
    m_device = device;

    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
    HRESULT result = D3DCompile(kCursorShader, sizeof(kCursorShader), "CursorShader",
                                nullptr, nullptr, "VSMain", "vs_4_0", 0, 0,
                                &vertexBytecode, &errors);
    if (FAILED(result)) {
        m_lastError = errors ? static_cast<const char*>(errors->GetBufferPointer())
                             : "cursor vertex shader failed";
        return false;
    }
    result = D3DCompile(kCursorShader, sizeof(kCursorShader), "CursorShader", nullptr,
                        nullptr, "PSMain", "ps_4_0", 0, 0, &pixelBytecode, &errors);
    if (FAILED(result)) {
        m_lastError = errors ? static_cast<const char*>(errors->GetBufferPointer())
                             : "cursor pixel shader failed";
        return false;
    }
    if (FAILED(m_device->CreateVertexShader(vertexBytecode->GetBufferPointer(),
                                            vertexBytecode->GetBufferSize(), nullptr,
                                            &m_vertexShader)) ||
        FAILED(m_device->CreatePixelShader(pixelBytecode->GetBufferPointer(),
                                           pixelBytecode->GetBufferSize(), nullptr,
                                           &m_pixelShader))) {
        m_lastError = "could not create the cursor shader objects";
        return false;
    }

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 32;  // two 16-byte rows
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&buffer, nullptr, &m_constants))) {
        m_lastError = "could not create the cursor constant buffer";
        return false;
    }

    // Premultiplied alpha over whatever the pane already drew.
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_device->CreateBlendState(&blend, &m_blend))) {
        m_lastError = "could not create the cursor blend state";
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateSamplerState(&sampler, &m_sampler))) {
        m_lastError = "could not create the cursor sampler";
        return false;
    }

    m_ready = true;
    return true;
}

void CursorRenderer::Destroy() {
    m_targetView.Reset();
    m_targetTexture = nullptr;
    m_texture.Reset();
    m_view.Reset();
    m_vertexShader.Reset();
    m_pixelShader.Reset();
    m_constants.Reset();
    m_blend.Reset();
    m_sampler.Reset();
    m_device.Reset();
    m_shapeVersion = 0;
    m_ready = false;
}

bool CursorRenderer::UploadShape(const DesktopPointer& pointer) {
    const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape = pointer.shape;
    if (shape.Width == 0 || shape.Height == 0 || pointer.pixels.empty()) {
        return false;
    }

    std::vector<uint8_t> rgba;
    uint32_t width = shape.Width;
    uint32_t height = shape.Height;

    if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        ExpandMonochrome(shape, pointer.pixels, rgba);
    } else {
        // Colour (and masked-colour) shapes are already BGRA rows of `Pitch`
        // bytes; copy them into a tight bitmap.
        const size_t needed = static_cast<size_t>(shape.Pitch) * height;
        if (pointer.pixels.size() < needed) {
            return false;
        }
        rgba.assign(static_cast<size_t>(width) * height * 4, 0);
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* source = pointer.pixels.data() + static_cast<size_t>(y) * shape.Pitch;
            uint8_t* destination = rgba.data() + static_cast<size_t>(y) * width * 4;
            const size_t rowBytes = static_cast<size_t>(width) * 4;
            for (size_t i = 0; i < rowBytes; ++i) {
                destination[i] = source[i];
            }
        }
    }

    m_texture.Reset();
    m_view.Reset();

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = rgba.data();
    initial.SysMemPitch = width * 4;
    if (FAILED(m_device->CreateTexture2D(&description, &initial, &m_texture)) ||
        FAILED(m_device->CreateShaderResourceView(m_texture.Get(), nullptr, &m_view))) {
        m_texture.Reset();
        m_view.Reset();
        m_lastError = "could not upload the pointer shape";
        return false;
    }

    m_shapeVersion = pointer.version;
    return true;
}

bool CursorRenderer::UpdateShape(const DesktopPointer& pointer) {
    if (!m_ready) {
        return false;
    }
    if (m_texture && pointer.version == m_shapeVersion) {
        return true;
    }
    return UploadShape(pointer);
}

void CursorRenderer::Draw(ID3D11DeviceContext* context, ID3D11Texture2D* target,
                          const DesktopPointer& pointer) {
    if (!m_ready || !context || !target || !m_texture || !m_view) {
        return;
    }
    if (!pointer.visible || pointer.shape.Width == 0 || pointer.shape.Height == 0) {
        return;
    }

    // One render target view per content texture; the texture is replaced
    // whenever the desktop mode changes.
    if (target != m_targetTexture) {
        m_targetView.Reset();
        if (FAILED(m_device->CreateRenderTargetView(target, nullptr, &m_targetView))) {
            return;
        }
        m_targetTexture = target;
    }

    D3D11_TEXTURE2D_DESC targetDescription{};
    target->GetDesc(&targetDescription);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    float* values = static_cast<float*>(mapped.pData);
    values[0] = static_cast<float>(pointer.x);
    values[1] = static_cast<float>(pointer.y);
    values[2] = static_cast<float>(pointer.shape.Width);
    values[3] = static_cast<float>(pointer.shape.Height);
    values[4] = static_cast<float>(targetDescription.Width);
    values[5] = static_cast<float>(targetDescription.Height);
    values[6] = 0.0f;
    values[7] = 0.0f;
    context->Unmap(m_constants.Get(), 0);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(targetDescription.Width);
    viewport.Height = static_cast<float>(targetDescription.Height);
    viewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* view = m_targetView.Get();
    ID3D11Buffer* constants = m_constants.Get();
    ID3D11ShaderResourceView* shapeView = m_view.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    ID3D11VertexShader* vertexShader = m_vertexShader.Get();
    ID3D11PixelShader* pixelShader = m_pixelShader.Get();
    ID3D11BlendState* blend = m_blend.Get();

    context->OMSetRenderTargets(1, &view, nullptr);
    context->RSSetViewports(1, &viewport);
    context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(vertexShader, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &constants);
    context->PSSetShader(pixelShader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->PSSetShaderResources(0, 1, &shapeView);
    context->PSSetSamplers(0, 1, &sampler);
    context->Draw(4, 0);

    // Leave no render target bound: GenerateMips refuses to run on a resource
    // that is still attached.
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
}

} // namespace dragonfly
