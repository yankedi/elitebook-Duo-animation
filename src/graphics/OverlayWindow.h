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
    // Re-asserts the topmost position.  Another topmost window can take the
    // front slot while the overlay is hidden, and the overlay then sits behind
    // it -- which looks exactly like "the effect does nothing".
    void BringToFront();
    bool IsShown() const { return m_shown; }

    // Follows a desktop mode change.  The window and its swap chain have to
    // cover the new resolution or the effect would be drawn at the wrong size.
    bool Resize(uint32_t width, uint32_t height);

    // ---- environment -------------------------------------------------------
    // Monitor power state, asked of Windows rather than inferred from the lid
    // angle: 0 = off, 1 = on, 2 = dimmed.
    bool DisplayOn() const { return m_displayOn; }

    // Lid switch state from the ACPI lid device; -1 when the machine does not
    // expose one.
    int LidSwitchState() const { return m_lidSwitch; }

    // Whether Windows accepted the subscriptions.  When it did not, the safety
    // gate falls back to the lid angle and the sensor freshness alone.
    bool DisplayNotifyActive() const { return m_displayNotify != nullptr; }
    bool LidNotifyActive() const { return m_lidNotify != nullptr; }

    // True once after a display power transition, a lid switch change, a mode
    // change or a suspend/resume, so the caller can re-arm its safety gate
    // instead of trusting a frame captured before the transition.
    bool ConsumeEnvironmentChange();

    // Asks DWM to leave this window out of screen capture.
    //
    // That is what makes a live effect possible: the desktop duplication reads
    // the composed desktop, so an overlay that is *in* the capture would feed
    // its own output back into the shader every frame and the picture would
    // dissolve into itself.  With the exclusion the duplication keeps handing
    // back the real desktop, so it can be read every frame instead of once per
    // fold, and video keeps playing through the effect.
    //
    // Windows 10 2004 (build 19041) or newer; returns false on anything older,
    // and the caller has to fall back to a snapshot per fold.
    bool ExcludeFromCapture();
    bool CaptureExcluded() const { return m_captureExcluded; }

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

    // Power setting subscriptions.  A hidden window still receives these: they
    // are posted to the HWND, not delivered only while it is on screen.
    HPOWERNOTIFY m_displayNotify = nullptr;
    HPOWERNOTIFY m_lidNotify = nullptr;
    bool m_displayOn = true;
    int m_lidSwitch = -1;
    bool m_environmentChanged = false;
    bool m_captureExcluded = false;
};

} // namespace dragonfly
