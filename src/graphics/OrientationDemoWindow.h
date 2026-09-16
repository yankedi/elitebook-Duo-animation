// ---------------------------------------------------------------------------
//  OrientationDemoWindow.h
//
//  Native counterpart to the analysed browser test: a D3D11 window that shows a
//  slab whose attitude follows the fused Windows OrientationSensor.
//
//  Two modes:
//    Website  - reproduces the site exactly: rotateX(-beta) followed by
//               rotateY(gamma), i.e. only two of the three angles reach the
//               model.  This is the default so the two can be compared 1:1.
//    Full     - applies the complete smoothed device attitude matrix, so yaw
//               also moves the model.
//
//  Keys: F toggles the mode, T flips the assumed matrix convention, R resets.
// ---------------------------------------------------------------------------
#pragma once

#include "D3DDevice.h"

#include "../sensors/SensorTypes.h"

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace dragonfly {

// How the slab attitude is derived from the sensor data.
enum class OrientationVisualMode {
    // Exactly what the analysed site does: rotateX(-beta) rotateY(gamma).
    // Faithful, but beta/gamma come from a ZXY decomposition and therefore
    // suffer gimbal lock when the screen is upright.
    Website,
    // Rotates the slab so its screen normal follows the device's Z axis.
    // Same "tilt only" feel, but built from the rotation matrix, so it has no
    // gimbal lock and cannot flip.
    Stable,
    // Complete smoothed attitude matrix: yaw moves the model too.
    Full,
};

struct OrientationVisual {
    double alphaDeg = 0.0;
    double betaDeg = 0.0;
    double gammaDeg = 0.0;
    Mat3 rotation;                 // device -> world (smoothed)
    Mat3 relativeRotation;         // device -> reference frame (smoothed)
    double relativeNormalX = 0.0;  // screen normal in the reference frame
    double relativeNormalY = 0.0;
    double relativeNormalZ = 1.0;
    OrientationVisualMode mode = OrientationVisualMode::Stable;
    bool transposed = false;
    bool valid = false;
    bool gimbalLock = false;
    bool foldedBeta = false;
};

class OrientationDemoWindow {
public:
    OrientationDemoWindow();
    ~OrientationDemoWindow();

    OrientationDemoWindow(const OrientationDemoWindow&) = delete;
    OrientationDemoWindow& operator=(const OrientationDemoWindow&) = delete;

    bool Create(const std::wstring& title, int width, int height);
    void Destroy();

    bool PumpMessages();
    void Render(const OrientationVisual& visual);
    void UpdateTitle(const OrientationVisual& visual);
    int ConsumeKeyPress();

    bool IsOpen() const { return m_hwnd != nullptr; }

private:
    struct Vertex {
        float position[3];
        float color[4];
    };

    struct SceneConstants {
        DirectX::XMFLOAT4X4 viewProjection;
        float tint[4];
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    static const Vertex* BuildVertexData(uint32_t& count);
    bool CreatePipeline();
    void ReleasePipeline();
    void UpdateConstants(const DirectX::XMMATRIX& model);
    void RenderScreenRotation(const OrientationVisual& visual,
                              DirectX::XMMATRIX& model) const;

    HWND m_hwnd = nullptr;
    D3DDevice m_device;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;

    uint32_t m_vertexCount = 0;
    int m_pendingKey = 0;
    int m_width = 0;
    int m_height = 0;
};

} // namespace dragonfly
