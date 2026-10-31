// Low-level mouse hook that takes over left clicks on the taskbar's Show desktop button.
//
// The hook lives on its own high-priority thread that does nothing but pump messages, so it answers
// instantly no matter what the main thread is doing (Windows drops low-level hooks that are slow).
// It swallows the button press and release, which is all Explorer needs to never see the click,
// and posts the actual work to the main window.
#include "common.h"
#include <atomic>

namespace hook {
namespace {

enum : UINT { kInstall = WM_APP + 100, kRemove };

HWND g_notify;
HANDLE g_thread;
DWORD g_threadId;
std::atomic<bool> g_active;

// Hook thread only.
HHOOK g_hook;
HWND g_pressedButton;  // the button that received the press we swallowed
HWND g_hoveredButton;  // the button under the cursor, as last reported to the main window

// Written by the hook, read by the watchdog.
std::atomic<ULONGLONG> g_lastEventTick;
std::atomic<LONG> g_lastX, g_lastY;

LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        g_lastEventTick.store(GetTickCount64(), std::memory_order_relaxed);
        g_lastX.store(info->pt.x, std::memory_order_relaxed);
        g_lastY.store(info->pt.y, std::memory_order_relaxed);

        if (info->dwExtraInfo != kSyntheticInputTag) {
            if (wParam == WM_MOUSEMOVE) {
                // Only entering and leaving a button is reported, for the per-monitor peek.
                const HWND button = taskbar::HoverHit(info->pt);
                if (button != g_hoveredButton) {
                    g_hoveredButton = button;
                    PostMessageW(g_notify, WM_APP_HOVER, reinterpret_cast<WPARAM>(button), 0);
                }
            } else if (wParam == WM_LBUTTONDOWN) {
                g_pressedButton = nullptr;
                // Shift+click keeps the original behavior (all monitors).
                if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                    if (HWND button = taskbar::ShowDesktopHit(info->pt)) {
                        g_pressedButton = button;
                        return 1;
                    }
                }
            } else if (wParam == WM_LBUTTONUP && g_pressedButton) {
                // Like a real button: act only if released over the same button.
                const HWND button = g_pressedButton;
                g_pressedButton = nullptr;
                if (taskbar::ShowDesktopHit(info->pt) == button)
                    PostMessageW(g_notify, WM_APP_TOGGLE, reinterpret_cast<WPARAM>(button),
                                 MAKELPARAM(info->pt.x, info->pt.y));
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void Remove() {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    g_pressedButton = nullptr;
    if (g_hoveredButton) PostMessageW(g_notify, WM_APP_HOVER, 0, 0);  // nothing reports leaving it from now on
    g_hoveredButton = nullptr;
}

void Install() {
    Remove();  // reinstalling also moves us back to the front of the hook chain
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    if (!g_hook) Log(L"SetWindowsHookEx failed: %lu", GetLastError());
}

DWORD WINAPI ThreadMain(void* ready) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the message queue
    SetEvent(static_cast<HANDLE>(ready));
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kInstall) Install();
        else if (msg.message == kRemove) Remove();
    }
    Remove();
    return 0;
}

void NoteCursor() {
    POINT pt{};
    GetCursorPos(&pt);
    g_lastX = pt.x;
    g_lastY = pt.y;
    g_lastEventTick = GetTickCount64();
}

}  // namespace

bool Start(HWND notify) {
    g_notify = notify;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, ThreadMain, ready, 0, &g_threadId);
    if (g_thread) WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    return g_thread != nullptr;
}

void Stop() {
    if (!g_thread) return;
    PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}

void SetActive(bool active) {
    g_active = active;
    NoteCursor();
    PostThreadMessageW(g_threadId, active ? kInstall : kRemove, 0, 0);
}

void Watchdog() {
    // Windows silently unhooks a low-level hook that once took too long (e.g. the whole system stalled).
    // The cursor moving without the hook hearing about it for a while is the telltale sign.
    if (!g_active) return;
    POINT pt;
    if (!GetCursorPos(&pt)) return;  // secure desktop, nothing to compare
    if (pt.x == g_lastX && pt.y == g_lastY) return;
    if (GetTickCount64() - g_lastEventTick < 3000) return;
    Log(L"hook: cursor moved without events, reinstalling");
    NoteCursor();
    PostThreadMessageW(g_threadId, kInstall, 0, 0);
}

}  // namespace hook
