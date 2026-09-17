// ---------------------------------------------------------------------------
//  OverlayWindow.cpp
// ---------------------------------------------------------------------------
#include "OverlayWindow.h"

namespace dragonfly {

namespace {

const wchar_t* kWindowClass = L"DragonflyFoldOverlay";

// The two power settings this overlay listens to.  The values are declared in
// winnt.h; they are repeated here so the build does not depend on which library
// happens to carry their definitions.
const GUID kConsoleDisplayState =
    {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
const GUID kLidSwitchState =
    {0xba3e0f4d, 0xb817, 0x4094, {0xa2, 0xd1, 0xd5, 0x63, 0x79, 0xe6, 0xa0, 0xf3}};

} // namespace

bool OverlayWindow::Create(const std::wstring& title, bool layered) {
    if (m_hwnd) {
        return true;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &OverlayWindow::WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&windowClass)) {
            return false;
        }
        classRegistered = true;
    }

    // Physical pixels: the process is per-monitor DPI aware (set in main), so
    // these match what Desktop Duplication reports.
    //
    // The pane covers the *work area*, not the whole screen: the taskbar is
    // topmost as well and sits above the pane, so covering its strip would make
    // DWM re-blur the taskbar's acrylic backdrop on every frame this window
    // updates -- which is exactly the flicker that was reported.  Leaving the
    // taskbar out keeps it crisp and keeps its backdrop cached.
    RECT workArea{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) == FALSE) {
        workArea.left = 0;
        workArea.top = 0;
        workArea.right = GetSystemMetrics(SM_CXSCREEN);
        workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    m_originX = workArea.left;
    m_originY = workArea.top;
    m_width = static_cast<uint32_t>(workArea.right - workArea.left);
    m_height = static_cast<uint32_t>(workArea.bottom - workArea.top);
    if (m_width == 0 || m_height == 0) {
        return false;
    }

    DWORD extendedStyle = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                          WS_EX_TOOLWINDOW;
    if (layered) {
        extendedStyle |= WS_EX_LAYERED;
    }

    m_hwnd = CreateWindowExW(
        // Topmost, click-through, never steals focus or shows in the taskbar.
        //
        // WS_EX_LAYERED is what makes the click-through real: WS_EX_TRANSPARENT
        // only affects painting order, it does not keep the window out of hit
        // testing, so without the layered flag this window swallows every click
        // on the desktop.  WM_NCHITTEST below answers HTTRANSPARENT as well, so
        // the two agree.
        extendedStyle,
        kWindowClass, title.c_str(), WS_POPUP,
        static_cast<int>(m_originX), static_cast<int>(m_originY),
        static_cast<int>(m_width), static_cast<int>(m_height),
        nullptr, nullptr, instance, this);

    if (!m_hwnd) {
        return false;
    }

    if (layered) {
        // Constant alpha 255: fully opaque, but a layered window.  Hit testing
        // skips it and clicks land on whatever is underneath.
        SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
    }

    // Created hidden; the effect shows it only while it has something to draw.
    m_shown = false;

    // Monitor power and lid switch come from Windows itself.  Registering is
    // allowed to fail (not every machine exposes a lid device), in which case
    // DisplayOn() stays true and the lid angle gate carries the load.
    m_displayNotify = RegisterPowerSettingNotification(
        m_hwnd, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    m_lidNotify = RegisterPowerSettingNotification(
        m_hwnd, &kLidSwitchState, DEVICE_NOTIFY_WINDOW_HANDLE);
    return true;
}

void OverlayWindow::Destroy() {
    if (m_displayNotify) {
        UnregisterPowerSettingNotification(m_displayNotify);
        m_displayNotify = nullptr;
    }
    if (m_lidNotify) {
        UnregisterPowerSettingNotification(m_lidNotify);
        m_lidNotify = nullptr;
    }
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_shown = false;
}

bool OverlayWindow::Resize(uint32_t width, uint32_t height) {
    if (!m_hwnd || width == 0 || height == 0) {
        return false;
    }
    if (width == m_width && height == m_height) {
        return true;
    }

    m_width = width;
    m_height = height;
    SetWindowPos(m_hwnd, HWND_TOPMOST, m_originX, m_originY, static_cast<int>(width),
                 static_cast<int>(height), SWP_NOACTIVATE);
    return true;
}

bool OverlayWindow::ConsumeEnvironmentChange() {
    const bool changed = m_environmentChanged;
    m_environmentChanged = false;
    return changed;
}

bool OverlayWindow::ExcludeFromCapture() {
    if (!m_hwnd) {
        return false;
    }
    // WDA_EXCLUDEFROMCAPTURE is Windows 10 2004 (build 19041) and newer.  On
    // anything older the call fails and the caller must not capture while the
    // overlay is up.
    m_captureExcluded =
        SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    return m_captureExcluded;
}

void OverlayWindow::Show(bool visible) {
    if (!m_hwnd || m_shown == visible) {
        return;
    }

    if (visible) {
        // SW_SHOWNOACTIVATE keeps the foreground application's focus.
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        BringToFront();
    } else {
        ShowWindow(m_hwnd, SW_HIDE);
    }
    m_shown = visible;
}

void OverlayWindow::BringToFront() {
    if (!m_hwnd) {
        return;
    }
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, static_cast<int>(m_width),
                 static_cast<int>(m_height),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE);
}

bool OverlayWindow::PumpMessages() {
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

int OverlayWindow::ConsumeKeyPress() {
    const int key = m_pendingKey;
    m_pendingKey = 0;
    return key;
}

LRESULT CALLBACK OverlayWindow::WindowProc(HWND window, UINT message, WPARAM wParam,
                                           LPARAM lParam) {
    OverlayWindow* self = nullptr;

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OverlayWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OverlayWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT OverlayWindow::HandleMessage(HWND window, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
    switch (message) {
    case WM_CLOSE:
        // Hiding rather than destroying keeps the swap chain alive.
        Show(false);
        return 0;

    case WM_DESTROY:
        m_hwnd = nullptr;
        m_shown = false;
        return 0;

    case WM_ERASEBKGND:
        return 1;  // D3D paints every frame

    case WM_NCHITTEST:
        // Click-through, the documented way: the system keeps looking for a
        // window that will take the hit, so the desktop underneath stays usable
        // while the pane is on screen.
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_POWERBROADCAST:
        switch (wParam) {
        case PBT_POWERSETTINGCHANGE: {
            const auto* setting =
                reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
            if (setting && setting->DataLength >= sizeof(DWORD)) {
                const DWORD value = *reinterpret_cast<const DWORD*>(setting->Data);
                if (setting->PowerSetting == kConsoleDisplayState) {
                    // 0 = off, 1 = on, 2 = dimmed.  Anything but "off" counts as
                    // usable: a dimmed panel is still a panel.
                    m_displayOn = (value != 0);
                    m_environmentChanged = true;
                } else if (setting->PowerSetting == kLidSwitchState) {
                    m_lidSwitch = static_cast<int>(value);
                    m_environmentChanged = true;
                }
            }
            break;
        }
        case PBT_APMSUSPEND:
        case PBT_APMRESUMESUSPEND:
        case PBT_APMRESUMEAUTOMATIC:
            // Whatever happens around a sleep invalidates the desktop we are
            // holding; the caller re-arms and captures again.
            m_environmentChanged = true;
            break;
        default:
            break;
        }
        return TRUE;

    case WM_DISPLAYCHANGE:
        // Resolution or output changed: the overlay, swap chain and captured
        // frame are all sized for the previous mode.
        m_environmentChanged = true;
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
