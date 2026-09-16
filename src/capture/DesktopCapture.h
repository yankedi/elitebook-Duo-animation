// ---------------------------------------------------------------------------
//  DesktopCapture.h
//
//  Grabs the desktop through the DXGI Desktop Duplication API.
//
//  One frame at a time, on demand: the caller acquires, draws and releases, so
//  the desktop is never held for longer than a single rendered frame.
//
//  Ported concept from duo-open's overlay pipeline (detect motion -> capture ->
//  show -> follow -> dismiss); the API itself is the Windows equivalent.
// ---------------------------------------------------------------------------
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace dragonfly {

class DesktopCapture {
public:
    // The duplication object must be created from the same adapter as the
    // device that will sample the frames.
    bool Start(ID3D11Device* device);
    void Stop();

    // Waits up to timeoutMs for a frame in which the desktop actually changed.
    // Returns false on timeout or failure -- a timeout is normal and simply
    // means "nothing on screen moved".
    bool AcquireFrame(uint32_t timeoutMs);
    void ReleaseFrame();

    // Valid between a successful AcquireFrame() and ReleaseFrame().
    ID3D11Texture2D* FrameTexture() const { return m_frame.Get(); }

    // Format of the captured frames, for diagnostics.
    DXGI_FORMAT Format() const { return m_format; }

    // Copies the acquired frame into a caller-owned texture, so its contents
    // survive ReleaseFrame().  The fold shader needs a stable texture to sample
    // while the lid keeps moving, and the duplication API requires the frame to
    // be released every cycle.
    void CopyFrameTo(ID3D11DeviceContext* context,
                     ID3D11Texture2D* destination) const;

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

    bool Valid() const { return m_duplication != nullptr; }
    const std::string& Error() const { return m_error; }

private:
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_frame;

    bool m_acquired = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    std::string m_error;
};

} // namespace dragonfly
