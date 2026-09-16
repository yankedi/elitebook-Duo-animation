// ---------------------------------------------------------------------------
//  DesktopCapture.cpp
// ---------------------------------------------------------------------------
#include "DesktopCapture.h"

namespace dragonfly {

bool DesktopCapture::Start(ID3D11Device* device) {
    Stop();

    if (!device) {
        m_error = "no D3D11 device";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
        m_error = "QueryInterface(IDXGIDevice) failed";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
        m_error = "IDXGIDevice::GetAdapter failed";
        return false;
    }

    // Output 0 is the one the desktop is composed onto.
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, &output))) {
        m_error = "IDXGIAdapter::EnumOutputs failed";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output1)))) {
        m_error = "QueryInterface(IDXGIOutput1) failed (Windows 8 or newer required)";
        return false;
    }

    const HRESULT result = output1->DuplicateOutput(device, &m_duplication);
    if (FAILED(result)) {
        // The two common causes worth naming in the message.
        if (result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            m_error = "DuplicateOutput: the desktop duplication limit is reached "
                      "(another capture is already running, or this is a remote session)";
        } else if (result == E_ACCESSDENIED) {
            m_error = "DuplicateOutput: access denied (a protected or secure "
                      "desktop is active)";
        } else {
            char text[128];
            std::snprintf(text, sizeof(text),
                          "DuplicateOutput failed (hr=0x%08lX)",
                          static_cast<unsigned long>(result));
            m_error = text;
        }
        return false;
    }

    DXGI_OUTDUPL_DESC description{};
    m_duplication->GetDesc(&description);
    m_width = description.ModeDesc.Width;
    m_height = description.ModeDesc.Height;
    m_format = description.ModeDesc.Format;
    return true;
}

void DesktopCapture::Stop() {
    ReleaseFrame();
    m_duplication.Reset();
    m_frame.Reset();
    m_lost = false;
    m_width = 0;
    m_height = 0;
}

bool DesktopCapture::TryRestart(ID3D11Device* device) {
    if (Healthy()) {
        return true;
    }
    return Start(device);
}

bool DesktopCapture::AcquireFrame(uint32_t timeoutMs) {
    if (!m_duplication) {
        return false;
    }

    // Never hold two frames at once.
    ReleaseFrame();

    DXGI_OUTDUPL_FRAME_INFO info{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    const HRESULT result =
        m_duplication->AcquireNextFrame(timeoutMs, &info, &resource);

    if (result == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;  // nothing changed on screen; not an error
    }
    if (FAILED(result)) {
        // Everything else means the duplication is finished: the display mode
        // changed, the panel was powered off, the secure desktop came up, or
        // another capture took over.  Holding on to it and retrying forever is
        // what leaves a stale frame on screen, so it is marked dead here and
        // the caller has to call TryRestart().
        m_lost = true;
        m_duplication.Reset();

        char text[160];
        std::snprintf(text, sizeof(text),
                      "AcquireNextFrame failed (hr=0x%08lX); the duplication is gone",
                      static_cast<unsigned long>(result));
        m_error = text;
        return false;
    }

    if (FAILED(resource.As(&m_frame))) {
        m_duplication->ReleaseFrame();
        return false;
    }

    m_acquired = true;
    return true;
}

void DesktopCapture::ReleaseFrame() {
    if (m_acquired && m_duplication) {
        m_duplication->ReleaseFrame();
    }
    m_acquired = false;
    m_frame.Reset();
}

void DesktopCapture::CopyFrameTo(ID3D11DeviceContext* context,
                                 ID3D11Texture2D* destination) const {
    if (!m_acquired || !m_frame || !destination || !context) {
        return;
    }

    context->CopyResource(destination, m_frame.Get());

    // Submit immediately.  D3D11 records commands asynchronously, and the
    // duplication API is free to overwrite its texture the moment the frame is
    // released -- without this flush the copy can lose the race and the
    // destination ends up black or half-updated.
    context->Flush();
}

} // namespace dragonfly
