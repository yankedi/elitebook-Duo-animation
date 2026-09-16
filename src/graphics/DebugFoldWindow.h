// ---------------------------------------------------------------------------
//  DebugFoldWindow.h
//
//  A deliberately small Win32 + D3D11 window used to eyeball the estimator:
//  two quads (base + screen) with the screen rotated around the hinge line by
//  the current fold progress.
//
//  This is NOT the final fold shader.  Its only job is to make it obvious
//  whether foldProgress moves smoothly, in the right direction, and without
//  drifting when the machine is moved.
// ---------------------------------------------------------------------------
#pragma once

#include "D3DDevice.h"

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace dragonfly {

class DebugFoldWindow {
public:
    DebugFoldWindow();
    ~DebugFoldWindow();

    DebugFoldWindow(const DebugFoldWindow&) = delete;
    DebugFoldWindow& operator=(const DebugFoldWindow&) = delete;

    bool Create(const std::wstring& title, int width, int height);
    void Destroy();

    // Drains the message queue. Returns false once the window has gone away.
    bool PumpMessages();

    void Render(float foldProgress, float confidence, bool manual);
    void UpdateTitle(float foldProgress, float confidence, bool manual);

    // Returns the last pressed virtual key (0 when none), and clears it.
    int ConsumeKeyPress();

    bool IsOpen() const { return m_hwnd != nullptr; }

private:
    struct Vertex {
        float position[3];
        float color[4];
        float isScreen;
    };

    struct SceneConstants {
        DirectX::XMFLOAT4X4 viewProjection;
        float hingeAngle;
        float screenBrightness;
        float padding[2];
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    static const Vertex* BuildVertexData(uint32_t& count);

    bool CreatePipeline();
    void ReleasePipeline();
    void UpdateSceneConstants(float progress, float confidence);

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
