// ---------------------------------------------------------------------------
//  OverlayWindow.cpp
// ---------------------------------------------------------------------------
#include "OverlayWindow.h"

namespace dragonfly {

namespace {

const wchar_t* kWindowClass = L"DragonflyFoldOverlay";

} // namespace

bool OverlayWindow::Create(const std::wstring& title) {
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
    m_width = static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
    m_height = static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
    if (m_width == 0 || m_height == 0) {
        return false;
    }

    m_hwnd = CreateWindowExW(
        // Topmost, click-through, never steals focus or shows in the taskbar.
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kWindowClass, title.c_str(), WS_POPUP,
        0, 0, static_cast<int>(m_width), static_cast<int>(m_height),
        nullptr, nullptr, instance, this);

    if (!m_hwnd) {
        return false;
    }

    // Created hidden; the effect shows it only while it has something to draw.
    m_shown = false;
    return true;
}

void OverlayWindow::Destroy() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_shown = false;
}

void OverlayWindow::Show(bool visible) {
    if (!m_hwnd || m_shown == visible) {
        return;
    }

    if (visible) {
        // SW_SHOWNOACTIVATE keeps the foreground application's focus.
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, static_cast<int>(m_width),
                     static_cast<int>(m_height), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(m_hwnd, SW_HIDE);
    }
    m_shown = visible;
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
