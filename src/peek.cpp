// "Peek at desktop" for one monitor.
//
// Windows' own peek fades out the windows of every monitor while the cursor rests on the Show desktop
// button. This one covers only the monitor whose button is hovered: a click-through overlay over that
// monitor's work area shows a live DWM thumbnail of the desktop (wallpaper, animated ones included, and
// icons) and fades in and out. No other window is touched.
#include "common.h"
#include <dwmapi.h>

namespace peek {
namespace {

constexpr wchar_t kClass[] = L"SmartCollapse.Peek";
constexpr UINT_PTR kDelayTimer = 1, kFadeTimer = 2;
constexpr int kFadeMs = 180;

HWND g_overlay;
HTHUMBNAIL g_thumbnail;
HWND g_button;      // Show desktop button under the cursor
bool g_suppressed;  // clicked: no peek until the cursor leaves the button
bool g_enabled = true;
int g_alpha, g_fadeFrom, g_fadeTo;
ULONGLONG g_fadeStart;

UINT HoverDelay() {  // Explorer's own setting for its peek, if someone tuned it
    DWORD ms = 0, size = sizeof(ms);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                     L"DesktopLivePreviewHoverTime", RRF_RT_REG_DWORD, nullptr, &ms, &size) == ERROR_SUCCESS &&
        ms > 0 && ms < 5000)
        return ms;
    return 500;
}

void SetAlpha(int alpha) {
    g_alpha = alpha;
    SetLayeredWindowAttributes(g_overlay, 0, BYTE(alpha), LWA_ALPHA);
}

void FadeTo(int alpha) {
    g_fadeFrom = g_alpha;
    g_fadeTo = alpha;
    g_fadeStart = GetTickCount64();
    SetTimer(g_overlay, kFadeTimer, 15, nullptr);
}

void Release() {
    KillTimer(g_overlay, kFadeTimer);
    ShowWindow(g_overlay, SW_HIDE);
    SetAlpha(0);
    if (g_thumbnail) DwmUnregisterThumbnail(g_thumbnail);
    g_thumbnail = nullptr;
}

void Show(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    const HWND desktop = GetShellWindow();
    RECT source;
    if (!GetMonitorInfoW(monitor, &info) || !desktop || !GetWindowRect(desktop, &source)) return;
    const RECT& area = info.rcWork;  // leaves the taskbar uncovered
    const int width = area.right - area.left, height = area.bottom - area.top;

    if (g_thumbnail) DwmUnregisterThumbnail(g_thumbnail);
    g_thumbnail = nullptr;
    if (FAILED(DwmRegisterThumbnail(g_overlay, desktop, &g_thumbnail))) return;
    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
    props.rcDestination = {0, 0, width, height};
    props.rcSource = {area.left - source.left, area.top - source.top, area.right - source.left, area.bottom - source.top};
    props.fVisible = TRUE;
    props.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(g_thumbnail, &props);

    if (!IsWindowVisible(g_overlay)) SetAlpha(0);
    SetWindowPos(g_overlay, HWND_TOPMOST, area.left, area.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    FadeTo(255);
}

void Hide() {
    KillTimer(g_overlay, kDelayTimer);
    if (IsWindowVisible(g_overlay)) FadeTo(0);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == kDelayTimer) {
        KillTimer(hwnd, kDelayTimer);
        // With Windows' own peek on as well, the two would fight; leave it to Windows then.
        if (g_button && g_enabled && !g_suppressed && !taskbar::NativePeekEnabled())
            Show(MonitorFromWindow(GetAncestor(g_button, GA_ROOT), MONITOR_DEFAULTTONEAREST));
        return 0;
    }
    if (msg == WM_TIMER && wParam == kFadeTimer) {
        const int elapsed = int(GetTickCount64() - g_fadeStart);
        if (elapsed < kFadeMs) {
            SetAlpha(g_fadeFrom + (g_fadeTo - g_fadeFrom) * elapsed / kFadeMs);
        } else if (g_fadeTo) {
            KillTimer(hwnd, kFadeTimer);
            SetAlpha(g_fadeTo);
        } else {
            Release();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void Init(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClass;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&wc);
    // Layered + transparent: invisible to the mouse, so it never gets in the way of a click.
    g_overlay = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOPMOST,
                                kClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    SetAlpha(0);
}

void SetEnabled(bool enabled) {
    g_enabled = enabled;
    if (!enabled) Hide();
}

bool Enabled() {
    return g_enabled;
}

void Hover(HWND button) {
    if (button == g_button) return;
    g_button = button;
    if (!button) {
        g_suppressed = false;
        Hide();
        return;
    }
    KillTimer(g_overlay, kDelayTimer);
    if (g_enabled && !g_suppressed) SetTimer(g_overlay, kDelayTimer, HoverDelay(), nullptr);
}

void Clicked() {
    g_suppressed = true;
    Hide();
}

void Cancel() {
    KillTimer(g_overlay, kDelayTimer);
    Release();
}

}  // namespace peek
