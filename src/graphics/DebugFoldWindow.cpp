// ---------------------------------------------------------------------------
//  DebugFoldWindow.cpp
// ---------------------------------------------------------------------------
#include "DebugFoldWindow.h"

#include <windows.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dragonfly {

namespace {

const wchar_t* kWindowClass = L"DragonflyDebugFoldWindow";

// Base and screen are the same quad; the screen rotates around the hinge line
// (the X axis at z = 0).
constexpr float kHalfWidth = 0.62f;
constexpr float kPanelDepth = 0.80f;

// Visual hinge angle range. Never fully closed so the two quads stay
// distinguishable, never fully flat so the motion stays readable.
constexpr float kMinAngleDegrees = 5.0f;
constexpr float kMaxAngleDegrees = 95.0f;

const char* kShaderSource = R"(
cbuffer SceneConstants : register(b0)
{
    float4x4 viewProjection;
    float    hingeAngle;
    float    screenBrightness;
    float2   padding;
};

struct VSInput
{
    float3 position : POSITION;
    float4 color    : COLOR;
    float  isScreen : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

VSOutput VSMain(VSInput input)
{
    float s, c;
    sincos(hingeAngle, s, c);

    float3 rotated = float3(input.position.x,
                            input.position.z * s,
                            input.position.z * c);
    float3 world = lerp(input.position, rotated, saturate(input.isScreen));

    VSOutput output;
    output.position = mul(float4(world, 1.0), viewProjection);
    output.color = input.color;
    output.color.rgb *= lerp(1.0, screenBrightness, saturate(input.isScreen));
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}
)";

} // namespace

// ==========================================================================
//  Construction
// ==========================================================================
DebugFoldWindow::DebugFoldWindow() = default;

DebugFoldWindow::~DebugFoldWindow() {
    Destroy();
}

bool DebugFoldWindow::Create(const std::wstring& title, int width, int height) {
    if (m_hwnd) {
        return true;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &DebugFoldWindow::WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&windowClass)) {
            return false;
        }
        classRegistered = true;
    }

    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                             rect.bottom - rect.top, nullptr, nullptr, instance, this);
    if (!m_hwnd) {
        return false;
    }

    m_width = width;
    m_height = height;

    if (!m_device.Create(m_hwnd, static_cast<uint32_t>(width),
                         static_cast<uint32_t>(height))) {
        Destroy();
        return false;
    }
    if (!CreatePipeline()) {
        Destroy();
        return false;
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

void DebugFoldWindow::Destroy() {
    ReleasePipeline();
    m_device.Destroy();
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

// ==========================================================================
//  Pipeline
// ==========================================================================
const DebugFoldWindow::Vertex* DebugFoldWindow::BuildVertexData(uint32_t& count) {
    static Vertex storage[12];

    const float baseR = 0.17f, baseG = 0.19f, baseB = 0.23f;
    const float frontR = 0.23f, frontG = 0.26f, frontB = 0.31f;
    const float screenR = 0.30f, screenG = 0.55f, screenB = 0.88f;

    const float xs[2] = {-kHalfWidth, kHalfWidth};
    int index = 0;

    // Base: two triangles, hinge edge (z = 0) first.
    storage[index++] = {{xs[0], 0.0f, 0.0f}, {baseR, baseG, baseB, 1.0f}, 0.0f};
    storage[index++] = {{xs[1], 0.0f, 0.0f}, {baseR, baseG, baseB, 1.0f}, 0.0f};
    storage[index++] = {{xs[0], 0.0f, kPanelDepth}, {frontR, frontG, frontB, 1.0f}, 0.0f};

    storage[index++] = {{xs[1], 0.0f, 0.0f}, {baseR, baseG, baseB, 1.0f}, 0.0f};
    storage[index++] = {{xs[1], 0.0f, kPanelDepth}, {frontR, frontG, frontB, 1.0f}, 0.0f};
    storage[index++] = {{xs[0], 0.0f, kPanelDepth}, {frontR, frontG, frontB, 1.0f}, 0.0f};

    // Screen: same quad, flagged so the vertex shader rotates it around the
    // hinge line.  Drawn last so the painter's order is correct.
    storage[index++] = {{xs[0], 0.0f, 0.0f}, {screenR, screenG, screenB, 1.0f}, 1.0f};
    storage[index++] = {{xs[1], 0.0f, 0.0f}, {screenR, screenG, screenB, 1.0f}, 1.0f};
    storage[index++] = {{xs[0], 0.0f, kPanelDepth}, {screenR, screenG, screenB, 1.0f}, 1.0f};

    storage[index++] = {{xs[1], 0.0f, 0.0f}, {screenR, screenG, screenB, 1.0f}, 1.0f};
    storage[index++] = {{xs[1], 0.0f, kPanelDepth}, {screenR, screenG, screenB, 1.0f}, 1.0f};
    storage[index++] = {{xs[0], 0.0f, kPanelDepth}, {screenR, screenG, screenB, 1.0f}, 1.0f};

    count = 12;
    return storage;
}

bool DebugFoldWindow::CreatePipeline() {
    ID3D11Device* device = m_device.Device();
    if (!device) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;

    HRESULT result = D3DCompile(kShaderSource, std::strlen(kShaderSource),
                                "DebugFoldShader", nullptr, nullptr, "VSMain", "vs_4_0",
                                flags, 0, &vertexBlob, &errors);
    if (FAILED(result)) {
        return false;
    }

    result = D3DCompile(kShaderSource, std::strlen(kShaderSource), "DebugFoldShader",
                        nullptr, nullptr, "PSMain", "ps_4_0", flags, 0, &pixelBlob,
                        &errors);
    if (FAILED(result)) {
        return false;
    }

    if (FAILED(device->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                          vertexBlob->GetBufferSize(), nullptr,
                                          &m_vertexShader))) {
        return false;
    }
    if (FAILED(device->CreatePixelShader(pixelBlob->GetBufferPointer(),
                                         pixelBlob->GetBufferSize(), nullptr,
                                         &m_pixelShader))) {
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device->CreateInputLayout(layout, 3, vertexBlob->GetBufferPointer(),
                                         vertexBlob->GetBufferSize(), &m_inputLayout))) {
        return false;
    }

    uint32_t vertexCount = 0;
    const Vertex* vertices = BuildVertexData(vertexCount);

    D3D11_BUFFER_DESC vertexBufferDesc{};
    vertexBufferDesc.ByteWidth = static_cast<UINT>(sizeof(Vertex) * vertexCount);
    vertexBufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vertexData{};
    vertexData.pSysMem = vertices;

    if (FAILED(device->CreateBuffer(&vertexBufferDesc, &vertexData, &m_vertexBuffer))) {
        return false;
    }
    m_vertexCount = vertexCount;

    D3D11_BUFFER_DESC constantBufferDesc{};
    constantBufferDesc.ByteWidth = sizeof(SceneConstants);
    constantBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&constantBufferDesc, nullptr, &m_constantBuffer))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;  // the screen quad is seen from both sides
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizerDesc, &m_rasterizerState))) {
        return false;
    }

    return true;
}

void DebugFoldWindow::ReleasePipeline() {
    m_rasterizerState.Reset();
    m_constantBuffer.Reset();
    m_vertexBuffer.Reset();
    m_inputLayout.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_vertexCount = 0;
}

// ==========================================================================
//  Rendering
// ==========================================================================
void DebugFoldWindow::UpdateSceneConstants(float progress, float confidence) {
    const float clamped = std::clamp(progress, 0.0f, 1.0f);
    const float angleDegrees =
        kMinAngleDegrees + clamped * (kMaxAngleDegrees - kMinAngleDegrees);

    const float aspect = (m_height > 0)
                             ? static_cast<float>(m_width) / static_cast<float>(m_height)
                             : 1.5f;

    const DirectX::XMVECTOR eye = DirectX::XMVectorSet(0.0f, 0.85f, 2.45f, 1.0f);
    const DirectX::XMVECTOR target = DirectX::XMVectorSet(0.0f, 0.26f, 0.34f, 1.0f);
    const DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    const DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, target, up);
    const DirectX::XMMATRIX projection =
        DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(45.0f), aspect,
                                          0.05f, 100.0f);

    SceneConstants constants{};
    DirectX::XMStoreFloat4x4(&constants.viewProjection,
                             DirectX::XMMatrixTranspose(view * projection));
    constants.hingeAngle = DirectX::XMConvertToRadians(angleDegrees);
    constants.screenBrightness = 0.55f + 0.45f * std::clamp(confidence, 0.0f, 1.0f);
    constants.padding[0] = 0.0f;
    constants.padding[1] = 0.0f;

    ID3D11DeviceContext* context = m_device.Context();
    if (!context) {
        return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                               &mapped))) {
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(m_constantBuffer.Get(), 0);
    }
}

void DebugFoldWindow::Render(float foldProgress, float confidence, bool manual) {
    (void)manual;  // represented in the title only
    if (!m_hwnd || !m_device.Valid() || !m_vertexBuffer) {
        return;
    }

    UpdateSceneConstants(foldProgress, confidence);

    m_device.BeginFrame(0.06f, 0.07f, 0.09f, 1.0f);

    ID3D11DeviceContext* context = m_device.Context();
    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;

    context->IASetInputLayout(m_inputLayout.Get());
    context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    context->RSSetState(m_rasterizerState.Get());
    context->Draw(m_vertexCount, 0);

    m_device.Present(true);
}

void DebugFoldWindow::UpdateTitle(float foldProgress, float confidence, bool manual) {
    if (!m_hwnd) {
        return;
    }
    wchar_t buffer[256] = {};
    swprintf_s(buffer, L"Dragonfly Fold Debug  |  progress %.3f   confidence %.2f   %s",
               foldProgress, confidence, manual ? L"MANUAL" : L"AUTO");
    SetWindowTextW(m_hwnd, buffer);
}

// ==========================================================================
//  Messages
// ==========================================================================
bool DebugFoldWindow::PumpMessages() {
    if (!m_hwnd) {
        return false;
    }

    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return m_hwnd != nullptr;
}

int DebugFoldWindow::ConsumeKeyPress() {
    const int key = m_pendingKey;
    m_pendingKey = 0;
    return key;
}

LRESULT CALLBACK DebugFoldWindow::WindowProc(HWND window, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
    DebugFoldWindow* self = nullptr;

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<DebugFoldWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DebugFoldWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT DebugFoldWindow::HandleMessage(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
    switch (message) {
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        m_hwnd = nullptr;
        return 0;

    case WM_ERASEBKGND:
        return 1;  // D3D paints every frame; skip the GDI erase flicker

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                m_width = width;
                m_height = height;
                m_device.Resize(static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height));
            }
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        m_pendingKey = static_cast<int>(wParam);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace dragonfly
