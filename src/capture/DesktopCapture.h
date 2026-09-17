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
#include <vector>

namespace dragonfly {

// The pointer the duplication reports alongside a frame.
//
// The desktop image itself never contains the cursor -- it lives on its own
// plane and is composited by the display controller -- so an effect that only
// samples the captured frame has no cursor in it at all.  The shape and the
// position come out of the duplication separately, and this is what carries
// them to the renderer that draws them in.
struct DesktopPointer {
    bool visible = false;
    int32_t x = 0;  // top-left of the shape in desktop pixels
    int32_t y = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
    std::vector<uint8_t> pixels;  // raw shape bytes, as the API handed them over
    uint64_t version = 0;         // bumped whenever the shape changes
};

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

    // True while the duplication is alive and can still be asked for frames.
    // A mode change, a display power transition, a switch to the secure desktop
    // or another capture taking over all kill it, and a dead duplication cannot
    // be revived -- only replaced.
    bool Healthy() const { return m_duplication != nullptr && !m_lost; }

    // The pointer that came with the last acquired frame.  Valid while the
    // duplication is healthy; the shape is only refreshed when it changes.
    const DesktopPointer& Pointer() const { return m_pointer; }

    // Replaces a duplication that returned DXGI_ERROR_ACCESS_LOST, or brings
    // one up for the first time.  Does nothing while the existing one is still
    // healthy.
    bool TryRestart(ID3D11Device* device);

private:
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_frame;

    bool m_acquired = false;
    bool m_lost = false;
    DesktopPointer m_pointer;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    std::string m_error;
};

} // namespace dragonfly
