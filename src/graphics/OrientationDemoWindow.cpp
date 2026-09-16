// ---------------------------------------------------------------------------
//  OrientationDemoWindow.cpp
// ---------------------------------------------------------------------------
#include "OrientationDemoWindow.h"

#include <windows.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dragonfly {

namespace {

const wchar_t* kWindowClass = L"DragonflyOrientationDemoWindow";

constexpr float kHalfWidth = 0.72f;    // slab half width  (device X)
constexpr float kHalfHeight = 0.46f;   // slab half height (device Y)
constexpr float kHalfDepth = 0.07f;    // slab half depth  (device Z, screen normal)

const char* kShaderSource = R"(
cbuffer SceneConstants : register(b0)
{
    float4x4 viewProjection;
    float4   tint;
};

struct VSInput
{
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.color = input.color * tint;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}
)";

// Note: a general "shortest arc from (0,0,1) to the screen normal" rotation was
// tried here and rejected -- it also rotates about the sideways component of the
// tilt, which makes a laptop lid appear to swing diagonally.  The Stable mode
// uses a hinge-axis-only rotation instead.

} // namespace

// ==========================================================================
//  Geometry: a fixed base plus a screen hinged on the base's rear edge.
//
//  The hinge line sits at the world origin and runs along X, so the screen
//  vertices can be defined with y in [0, height] and simply rotated about it.
//  Vertex range [0, 36) is the base, [36, 72) is the screen.
// ==========================================================================
const OrientationDemoWindow::Vertex* OrientationDemoWindow::BuildVertexData(
    uint32_t& count) {
    static Vertex storage[72];

    const float halfWidth = 0.72f;
    const float baseDepth = 0.85f;
    const float baseThickness = 0.05f;
    const float screenHeight = 0.90f;
    const float screenThickness = 0.03f;

    const float baseTopR = 0.30f, baseTopG = 0.32f, baseTopB = 0.36f;
    const float baseSideR = 0.20f, baseSideG = 0.21f, baseSideB = 0.24f;
    const float screenR = 0.24f, screenG = 0.56f, screenB = 0.95f;
    const float screenBackR = 0.17f, screenBackG = 0.18f, screenBackB = 0.21f;
    const float screenEdgeR = 0.36f, screenEdgeG = 0.38f, screenEdgeB = 0.42f;

    int index = 0;
    auto quad = [&](const float* a, const float* b, const float* c, const float* d,
                    float r, float g, float bl) {
        const float* corners[6] = {a, b, c, a, c, d};
        for (const float* corner : corners) {
            storage[index].position[0] = corner[0];
            storage[index].position[1] = corner[1];
            storage[index].position[2] = corner[2];
            storage[index].color[0] = r;
            storage[index].color[1] = g;
            storage[index].color[2] = bl;
            storage[index].color[3] = 1.0f;
            ++index;
        }
    };

    // ---- base (fixed) -----------------------------------------------------
    {
        const float x0 = -halfWidth, x1 = halfWidth;
        const float y0 = -baseThickness, y1 = 0.0f;
        const float z0 = 0.0f, z1 = baseDepth;

        // top deck
        const float t0[3] = {x0, y1, z0};
        const float t1[3] = {x1, y1, z0};
        const float t2[3] = {x1, y1, z1};
        const float t3[3] = {x0, y1, z1};
        quad(t0, t1, t2, t3, baseTopR, baseTopG, baseTopB);

        // front edge
        const float f0[3] = {x0, y0, z1};
        const float f1[3] = {x1, y0, z1};
        const float f2[3] = {x1, y1, z1};
        const float f3[3] = {x0, y1, z1};
        quad(f0, f1, f2, f3, baseSideR, baseSideG, baseSideB);

        // bottom
        const float b0[3] = {x0, y0, z0};
        const float b1[3] = {x1, y0, z0};
        const float b2[3] = {x1, y0, z1};
        const float b3[3] = {x0, y0, z1};
        quad(b0, b1, b2, b3, baseSideR, baseSideG, baseSideB);

        // left / right
        const float l0[3] = {x0, y0, z0};
        const float l1[3] = {x0, y0, z1};
        const float l2[3] = {x0, y1, z1};
        const float l3[3] = {x0, y1, z0};
        quad(l0, l1, l2, l3, baseSideR, baseSideG, baseSideB);

        const float r0[3] = {x1, y0, z0};
        const float r1[3] = {x1, y0, z1};
        const float r2[3] = {x1, y1, z1};
        const float r3[3] = {x1, y1, z0};
        quad(r0, r1, r2, r3, baseSideR, baseSideG, baseSideB);

        // rear edge (behind the hinge)
        const float k0[3] = {x0, y0, z0};
        const float k1[3] = {x1, y0, z0};
        const float k2[3] = {x1, y1, z0};
        const float k3[3] = {x0, y1, z0};
        quad(k0, k1, k2, k3, baseSideR, baseSideG, baseSideB);
    }

    // ---- screen (hinged at the origin, extends along +Y) ------------------
    {
        const float x0 = -halfWidth, x1 = halfWidth;
        const float y0 = 0.0f, y1 = screenHeight;
        const float z0 = -screenThickness, z1 = screenThickness;

        // front face (towards the viewer)
        const float f0[3] = {x0, y0, z1};
        const float f1[3] = {x1, y0, z1};
        const float f2[3] = {x1, y1, z1};
        const float f3[3] = {x0, y1, z1};
        quad(f0, f1, f2, f3, screenR, screenG, screenB);

        // back face
        const float b0[3] = {x1, y0, z0};
        const float b1[3] = {x0, y0, z0};
        const float b2[3] = {x0, y1, z0};
        const float b3[3] = {x1, y1, z0};
        quad(b0, b1, b2, b3, screenBackR, screenBackG, screenBackB);

        // top edge
        const float t0[3] = {x0, y1, z1};
        const float t1[3] = {x1, y1, z1};
        const float t2[3] = {x1, y1, z0};
        const float t3[3] = {x0, y1, z0};
        quad(t0, t1, t2, t3, screenEdgeR, screenEdgeG, screenEdgeB);

        // left / right edges
        const float l0[3] = {x0, y0, z0};
        const float l1[3] = {x0, y0, z1};
        const float l2[3] = {x0, y1, z1};
        const float l3[3] = {x0, y1, z0};
        quad(l0, l1, l2, l3, screenEdgeR, screenEdgeG, screenEdgeB);

        const float r0[3] = {x1, y0, z0};
        const float r1[3] = {x1, y0, z1};
        const float r2[3] = {x1, y1, z1};
        const float r3[3] = {x1, y1, z0};
        quad(r0, r1, r2, r3, screenEdgeR, screenEdgeG, screenEdgeB);

        // bottom edge (at the hinge)
        const float k0[3] = {x0, y0, z0};
        const float k1[3] = {x1, y0, z0};
        const float k2[3] = {x1, y0, z1};
        const float k3[3] = {x0, y0, z1};
        quad(k0, k1, k2, k3, screenEdgeR, screenEdgeG, screenEdgeB);
    }

    count = 72;
    return storage;
}

// ==========================================================================
//  Construction
// ==========================================================================
OrientationDemoWindow::OrientationDemoWindow() = default;

OrientationDemoWindow::~OrientationDemoWindow() {
    Destroy();
}

bool OrientationDemoWindow::Create(const std::wstring& title, int width, int height) {
    if (m_hwnd) {
        return true;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &OrientationDemoWindow::WindowProc;
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

void OrientationDemoWindow::Destroy() {
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
bool OrientationDemoWindow::CreatePipeline() {
    ID3D11Device* device = m_device.Device();
    if (!device) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;

    if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource),
                          "OrientationDemoShader", nullptr, nullptr, "VSMain", "vs_4_0",
                          flags, 0, &vertexBlob, &errors))) {
        return false;
    }
    if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource),
                          "OrientationDemoShader", nullptr, nullptr, "PSMain", "ps_4_0",
                          flags, 0, &pixelBlob, &errors))) {
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
    };
    if (FAILED(device->CreateInputLayout(layout, 2, vertexBlob->GetBufferPointer(),
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
    rasterizerDesc.CullMode = D3D11_CULL_NONE;  // the slab is seen from both sides
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizerDesc, &m_rasterizerState))) {
        return false;
    }

    return true;
}

void OrientationDemoWindow::ReleasePipeline() {
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
void OrientationDemoWindow::UpdateConstants(const DirectX::XMMATRIX& model) {
    using namespace DirectX;

    const float aspect = (m_height > 0)
                             ? static_cast<float>(m_width) / static_cast<float>(m_height)
                             : 1.3333f;

    // Right-handed pipeline: the device frame is right-handed (X right, Y up,
    // Z out of the screen), so pairing it with RH matrices keeps the tilt
    // direction correct.  A left-handed camera mirrors the scene, which is what
    // made the motion read as reversed.
    // Nearly head-on so the screen reads as "square to the viewer" the way the
    // reference browser page does, while still showing the base.
    const XMVECTOR eye = XMVectorSet(0.0f, 0.52f, 2.75f, 1.0f);
    const XMVECTOR target = XMVectorSet(0.0f, 0.42f, 0.22f, 1.0f);
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMMATRIX view = XMMatrixLookAtRH(eye, target, up);
    const XMMATRIX projection =
        XMMatrixPerspectiveFovRH(XMConvertToRadians(40.0f), aspect, 0.05f, 100.0f);

    SceneConstants constants{};
    XMStoreFloat4x4(&constants.viewProjection,
                    XMMatrixTranspose(model * view * projection));
    constants.tint[0] = constants.tint[1] = constants.tint[2] = 1.0f;
    constants.tint[3] = 1.0f;

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

void OrientationDemoWindow::RenderScreenRotation(const OrientationVisual& visual,
                                                 DirectX::XMMATRIX& model) const {
    using namespace DirectX;

    switch (visual.mode) {
    case OrientationVisualMode::Website: {
        // The site's `rotateX(-beta) rotateY(gamma)` lives in CSS space, which
        // is left-handed (Y points down).  Mapping it onto this right-handed
        // scene flips every rotation, giving rotateX(+beta) rotateY(-gamma).
        const float beta = static_cast<float>(visual.betaDeg);
        const float gamma = static_cast<float>(visual.gammaDeg);
        model = XMMatrixRotationX(XMConvertToRadians(beta)) *
                XMMatrixRotationY(XMConvertToRadians(-gamma));
        break;
    }

    case OrientationVisualMode::Stable: {
        // A laptop lid only ever turns about its hinge, so the screen is
        // rotated about the hinge axis (X) by the pitch of the device's Z axis
        // relative to the reference attitude.
        //
        // Deliberately NOT a general shortest-arc rotation: that would also
        // pick up sideways tilt and make the lid swing diagonally, which reads
        // as a book opening from a corner rather than a lid turning on a hinge.
        const double pitch = std::atan2(visual.relativeNormalY, visual.relativeNormalZ);
        model = XMMatrixRotationX(static_cast<float>(-pitch));
        break;
    }

    case OrientationVisualMode::Full: {
        // Complete attitude relative to the reference, column-vector convention
        // (v_ref = R_rel * v_device), so DirectXMath needs the transpose.
        const Mat3& r = visual.relativeRotation;
        XMFLOAT4X4 packed;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                packed.m[row][col] = static_cast<float>(r.m[col * 3 + row]);
            }
            packed.m[row][3] = 0.0f;
        }
        packed.m[3][0] = packed.m[3][1] = packed.m[3][2] = 0.0f;
        packed.m[3][3] = 1.0f;
        model = XMLoadFloat4x4(&packed);
        break;
    }
    }
}

void OrientationDemoWindow::Render(const OrientationVisual& visual) {
    if (!m_hwnd || !m_device.Valid() || !m_vertexBuffer) {
        return;
    }

    m_device.BeginFrame(0.07f, 0.08f, 0.10f, 1.0f);

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

    // The base is fixed in the world; only the screen moves.
    UpdateConstants(DirectX::XMMatrixIdentity());
    context->Draw(36, 0);

    DirectX::XMMATRIX screenModel = DirectX::XMMatrixIdentity();
    RenderScreenRotation(visual, screenModel);
    UpdateConstants(screenModel);
    context->Draw(36, 36);

    m_device.Present(true);
}

void OrientationDemoWindow::UpdateTitle(const OrientationVisual& visual) {
    if (!m_hwnd) {
        return;
    }

    const wchar_t* modeName = L"?";
    switch (visual.mode) {
    case OrientationVisualMode::Website: modeName = L"WEBSITE (rotateX/rotateY)"; break;
    case OrientationVisualMode::Stable: modeName = L"STABLE (screen normal)"; break;
    case OrientationVisualMode::Full: modeName = L"FULL attitude"; break;
    }

    wchar_t buffer[360] = {};
    swprintf_s(buffer, L"Orientation Demo  |  %s  |  a %7.1f   b %7.1f   g %7.1f %s%s  |  %s",
               modeName, visual.alphaDeg, visual.betaDeg, visual.gammaDeg,
               visual.gimbalLock ? L" [GIMBAL]" : L"",
               visual.foldedBeta ? L" [FOLDED]" : L"",
               visual.valid ? (visual.transposed ? L"transposed" : L"as-reported")
                            : L"(no sensor data)");
    SetWindowTextW(m_hwnd, buffer);
}

// ==========================================================================
//  Messages
// ==========================================================================
bool OrientationDemoWindow::PumpMessages() {
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

int OrientationDemoWindow::ConsumeKeyPress() {
    const int key = m_pendingKey;
    m_pendingKey = 0;
    return key;
}

LRESULT CALLBACK OrientationDemoWindow::WindowProc(HWND window, UINT message,
                                                   WPARAM wParam, LPARAM lParam) {
    OrientationDemoWindow* self = nullptr;

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OrientationDemoWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OrientationDemoWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT OrientationDemoWindow::HandleMessage(HWND window, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
    switch (message) {
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        m_hwnd = nullptr;
        return 0;

    case WM_ERASEBKGND:
        return 1;

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
