// ---------------------------------------------------------------------------
//  FoldRenderer.cpp
// ---------------------------------------------------------------------------
#include "FoldRenderer.h"

#include "../shaders/FoldShaderSource.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace dragonfly {

namespace {

// Must stay in sync with the cbuffer in FoldShaderSource.h: three 16-byte rows.
struct FoldConstants {
    float resolution[2];
    float angleDelta;
    float eyeDistancePx;

    float blurStrength;
    float darkening;
    float hingeFromTop;
    float attenuationFloor;

    float sheenStrength;
    float edgeGlow;
    float dispersionPx;
    float scatterDesaturation;
};

constexpr float kPi = 3.14159265358979323846f;

bool Compile(const char* source, const char* entryPoint, const char* target,
             ID3DBlob** blob, const char** errorText, std::string& errorStorage) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), "FoldShader",
                                      nullptr, nullptr, entryPoint, target,
                                      D3DCOMPILE_ENABLE_STRICTNESS, 0, blob, &errors);
    if (FAILED(result)) {
        if (errors) {
            errorStorage.assign(static_cast<const char*>(errors->GetBufferPointer()),
                                errors->GetBufferSize());
            *errorText = errorStorage.c_str();
        } else {
            *errorText = "shader compilation failed";
        }
        return false;
    }
    return true;
}

} // namespace

// ==========================================================================
//  Setup
// ==========================================================================
bool FoldRenderer::Create(ID3D11Device* device, uint32_t width, uint32_t height) {
    Destroy();
    m_width = width;
    m_height = height;
    m_device = device;

    if (!device) {
        m_lastError = "no D3D11 device";
        return false;
    }
    if (!CreatePipeline(device)) {
        return false;
    }

    m_ready = true;
    return true;
}

bool FoldRenderer::CreatePipeline(ID3D11Device* device) {
    std::string vertexErrors;
    std::string pixelErrors;
    Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBlob;

    if (!Compile(kFoldVertexShader, "VSMain", "vs_5_0", &vertexBlob, &m_lastError,
                 vertexErrors)) {
        return false;
    }
    if (!Compile(kFoldPixelShader, "main", "ps_5_0", &pixelBlob, &m_lastError,
                 pixelErrors)) {
        return false;
    }

    if (FAILED(device->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                          vertexBlob->GetBufferSize(), nullptr,
                                          &m_vertexShader))) {
        m_lastError = "CreateVertexShader failed";
        return false;
    }
    if (FAILED(device->CreatePixelShader(pixelBlob->GetBufferPointer(),
                                         pixelBlob->GetBufferSize(), nullptr,
                                         &m_pixelShader))) {
        m_lastError = "CreatePixelShader failed";
        return false;
    }

    D3D11_BUFFER_DESC bufferDescription{};
    bufferDescription.ByteWidth = sizeof(FoldConstants);
    bufferDescription.Usage = D3D11_USAGE_DYNAMIC;
    bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bufferDescription, nullptr, &m_constantBuffer))) {
        m_lastError = "CreateBuffer(constants) failed";
        return false;
    }

    // The fold samples the desktop far outside its original bounds, so clamping
    // at the edge and filtering linearly is what makes the blur look soft
    // rather than repeating the desktop like a tiled texture.
    D3D11_SAMPLER_DESC samplerDescription{};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samplerDescription, &m_sampler))) {
        m_lastError = "CreateSamplerState failed";
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDescription{};
    rasterizerDescription.FillMode = D3D11_FILL_SOLID;
    rasterizerDescription.CullMode = D3D11_CULL_NONE;
    rasterizerDescription.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizerDescription,
                                             &m_rasterizerState))) {
        m_lastError = "CreateRasterizerState failed";
        return false;
    }

    return true;
}

void FoldRenderer::Destroy() {
    m_rasterizerState.Reset();
    m_contentView.Reset();
    m_contentTexture = nullptr;
    m_sampler.Reset();
    m_constantBuffer.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_device = nullptr;
    m_ready = false;
}

void FoldRenderer::Resize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
}

// ==========================================================================
//  Angle mapping
// ==========================================================================
float FoldRenderer::ClampDelta(float angleDeltaDeg,
                               const FoldEffectParameters& parameters) {
    // A negative delta means the lid is above the activation angle: the desktop
    // is simply being used and the shader must pass it through untouched.
    return std::clamp(angleDeltaDeg, 0.0f, parameters.maxDeltaDegrees);
}

// ==========================================================================
//  Rendering
// ==========================================================================
bool FoldRenderer::BindContent(ID3D11Texture2D* desktop) {
    if (!desktop || !m_device) {
        return false;
    }
    if (desktop == m_contentTexture && m_contentView) {
        return true;  // same underlying resource, view is still valid
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(m_device->CreateShaderResourceView(desktop, nullptr, &view))) {
        m_lastError = "CreateShaderResourceView(desktop) failed";
        return false;
    }

    m_contentView = view;
    m_contentTexture = desktop;
    return true;
}

void FoldRenderer::Render(ID3D11DeviceContext* context,
                          ID3D11Texture2D* desktop,
                          ID3D11RenderTargetView* target,
                          float angleDeltaDeg,
                          const FoldEffectParameters& parameters) {
    if (!m_ready || !context || !target || !desktop) {
        return;
    }
    if (!BindContent(desktop)) {
        return;
    }

    // ---- constants --------------------------------------------------------
    FoldConstants constants{};
    constants.resolution[0] = static_cast<float>(m_width);
    constants.resolution[1] = static_cast<float>(m_height);
    constants.angleDelta = ClampDelta(angleDeltaDeg, parameters) * kPi / 180.0f;
    constants.eyeDistancePx = parameters.eyeDistancePx;
    constants.blurStrength = parameters.blurStrength;
    constants.darkening = parameters.darkening;
    constants.hingeFromTop = parameters.hingeFromTop ? 1.0f : 0.0f;
    constants.attenuationFloor = parameters.attenuationFloor;
    constants.sheenStrength = parameters.sheenStrength;
    constants.edgeGlow = parameters.edgeGlow;
    constants.dispersionPx = parameters.dispersionPx;
    constants.scatterDesaturation = parameters.scatterDesaturation;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                               &mapped))) {
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(m_constantBuffer.Get(), 0);
    }

    // ---- viewport ---------------------------------------------------------
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    // ---- pipeline ---------------------------------------------------------
    ID3D11RenderTargetView* targets[1] = {target};
    context->OMSetRenderTargets(1, targets, nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    context->PSSetShaderResources(0, 1, m_contentView.GetAddressOf());
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    context->RSSetState(m_rasterizerState.Get());

    context->Draw(3, 0);

    // Unbind so the desktop texture is not left attached to the pipeline.
    ID3D11ShaderResourceView* none[1] = {nullptr};
    context->PSSetShaderResources(0, 1, none);
}

} // namespace dragonfly
