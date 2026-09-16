// ---------------------------------------------------------------------------
//  OverlayWindow.h
//
//  A borderless, topmost, click-through window covering the whole display.
//
//  It exists only while the fold effect is actually running: the caller shows it
//  when the lid starts moving and hides it the moment the effect resolves, so
//  the desktop is never left covered by a stale snapshot.  That lifecycle is the
//  central lesson from duo-open's overlay service (capture -> show -> follow ->
//  dismiss), expressed with Win32 windows instead of an Android accessibility
//  overlay.
//
//  The window is WS_EX_TRANSPARENT so the mouse keeps working through it; the
//  effect is stopped from the console (Q / Ctrl+C).
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace dragonfly {

class OverlayWindow {
public:
    bool Create(const std::wstring& title);
    void Destroy();

    void Show(bool visible);
    bool IsShown() const { return m_shown; }

    // Drains the message queue; returns false once the window is gone.
    bool PumpMessages();
    int ConsumeKeyPress();

    bool IsOpen() const { return m_hwnd != nullptr; }
    HWND Handle() const { return m_hwnd; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_shown = false;
    int m_pendingKey = 0;
};

} // namespace dragonfly
