// Locates the taskbar's Show desktop button.
//
// Classic taskbars (Windows 10, StartAllBack, ExplorerPatcher) implement it as a child window of class
// TrayShowDesktopButtonWClass on every taskbar that shows one, so hit-testing is exact and cheap.
// The Windows 11 taskbar draws it with XAML instead. For those taskbars a background thread asks
// UI Automation where the button is, and the hook compares the click against that cached rectangle.
// With classic taskbars the UI Automation code is never touched.
//
// Clicks are checked with WindowFromPoint. Hovering is checked on every mouse move, so it first compares
// the point against the cached button rectangles and only confirms a match with WindowFromPoint.
#include "common.h"
#include <ole2.h>  // WIN32_LEAN_AND_MEAN leaves out the COM declarations UIAutomation.h relies on
#include <UIAutomation.h>
#include <atomic>
#include <string.h>

namespace taskbar {
namespace {

constexpr wchar_t kButtonClass[] = L"TrayShowDesktopButtonWClass";
constexpr wchar_t kXamlHostClass[] = L"Windows.UI.Composition.DesktopWindowContentBridge";
constexpr int kMaxTaskbars = 16;

struct XamlButton {
    HWND taskbar;
    SIZE taskbarSize;  // taskbar size when scanned; any other size means the entry is stale
    RECT rect;         // relative to the taskbar's top-left corner, so auto-hide sliding doesn't matter
};

SRWLOCK g_lock = SRWLOCK_INIT;  // guards the caches below
HWND g_classic[kMaxTaskbars];    // classic Show desktop buttons
int g_classicCount;
XamlButton g_buttons[kMaxTaskbars];
int g_buttonCount;
std::atomic<bool> g_haveXaml;

HANDLE g_scanEvent;
HANDLE g_scanThread;
std::atomic<bool> g_stopping;

bool ClassIs(HWND hwnd, const wchar_t* name) {
    wchar_t cls[64];
    return GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && wcscmp(cls, name) == 0;
}

// The XAML taskbar whose cached Show desktop rectangle contains pt (rectangles only, no hit-testing).
HWND XamlTaskbarAt(POINT pt) {
    HWND found = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_buttonCount && !found; ++i) {
        const XamlButton& b = g_buttons[i];
        RECT tb;
        if (!GetWindowRect(b.taskbar, &tb) || b.taskbarSize.cx != tb.right - tb.left ||
            b.taskbarSize.cy != tb.bottom - tb.top)
            continue;
        RECT r = b.rect;
        OffsetRect(&r, tb.left, tb.top);
        if (PtInRect(&r, pt)) found = b.taskbar;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

constexpr const wchar_t* kTaskbarClasses[] = {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"};

int FindXamlTaskbars(HWND* out, int max) {
    int count = 0;
    for (const wchar_t* cls : kTaskbarClasses) {
        HWND taskbar = nullptr;
        while (count < max && (taskbar = FindWindowExW(nullptr, taskbar, cls, nullptr))) {
            HWND host = FindWindowExW(taskbar, nullptr, kXamlHostClass, nullptr);
            if (host && IsWindowVisible(host)) out[count++] = taskbar;
        }
    }
    return count;
}

void RefreshClassicButtons() {
    HWND found[kMaxTaskbars];
    int count = 0;
    for (const wchar_t* cls : kTaskbarClasses) {
        HWND taskbar = nullptr;
        while (count < kMaxTaskbars && (taskbar = FindWindowExW(nullptr, taskbar, cls, nullptr))) {
            HWND button = nullptr;  // a grandchild (under TrayNotifyWnd) on the taskbars seen so far
            EnumChildWindows(
                taskbar,
                [](HWND child, LPARAM param) -> BOOL {
                    if (!ClassIs(child, kButtonClass)) return TRUE;
                    *reinterpret_cast<HWND*>(param) = child;
                    return FALSE;
                },
                reinterpret_cast<LPARAM>(&button));
            if (button) found[count++] = button;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    memcpy(g_classic, found, sizeof(HWND) * count);
    g_classicCount = count;
    ReleaseSRWLockExclusive(&g_lock);
}

bool CornerButtonDisabled() {
    // Settings > Personalization > Taskbar > "Select the far corner of the taskbar to show the desktop".
    DWORD value = 1, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                        L"TaskbarSd", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value == 0;
}

bool Mentions(BSTR text, const wchar_t* what) {
    return text && FindStringOrdinal(FIND_FROMSTART, text, -1, what, -1, TRUE) >= 0;
}

// Asks UI Automation for the Show desktop element of one XAML taskbar: an element at the far end of the
// taskbar that spans most of its thickness and is either named after Show desktop or a thin button.
bool LocateXamlButton(IUIAutomation* uia, HWND taskbar, RECT* found) {
    RECT tb;
    if (!GetWindowRect(taskbar, &tb)) return false;
    const bool horizontal = tb.right - tb.left >= tb.bottom - tb.top;
    const LONG thickness = horizontal ? tb.bottom - tb.top : tb.right - tb.left;
    const UINT dpi = GetDpiForWindow(taskbar);
    const LONG thinMax = MulDiv(24, dpi, 96), namedMax = MulDiv(64, dpi, 96), slack = MulDiv(2, dpi, 96);

    IUIAutomationElement* root = nullptr;
    IUIAutomationCacheRequest* cache = nullptr;
    IUIAutomationCondition* all = nullptr;
    IUIAutomationElementArray* items = nullptr;
    int best = 0;
    if (SUCCEEDED(uia->ElementFromHandle(taskbar, &root)) && root && SUCCEEDED(uia->CreateCacheRequest(&cache)) &&
        SUCCEEDED(cache->AddProperty(UIA_BoundingRectanglePropertyId)) &&
        SUCCEEDED(cache->AddProperty(UIA_ControlTypePropertyId)) &&
        SUCCEEDED(cache->AddProperty(UIA_AutomationIdPropertyId)) &&
        SUCCEEDED(cache->AddProperty(UIA_ClassNamePropertyId)) && SUCCEEDED(uia->CreateTrueCondition(&all)) &&
        SUCCEEDED(root->FindAllBuildCache(TreeScope_Descendants, all, cache, &items)) && items) {
        int count = 0;
        items->get_Length(&count);
        for (int i = 0; i < count; ++i) {
            IUIAutomationElement* element = nullptr;
            if (FAILED(items->GetElement(i, &element)) || !element) continue;
            RECT r{};
            CONTROLTYPEID type = 0;
            BSTR id = nullptr, cls = nullptr;
            element->get_CachedBoundingRectangle(&r);
            element->get_CachedControlType(&type);
            element->get_CachedAutomationId(&id);
            element->get_CachedClassName(&cls);
            const LONG length = horizontal ? r.right - r.left : r.bottom - r.top;
            const LONG depth = horizontal ? r.bottom - r.top : r.right - r.left;
            const bool atEnd = horizontal ? labs(r.right - tb.right) <= slack : labs(r.bottom - tb.bottom) <= slack;
            int score = 0;
            if (atEnd && length > 0 && depth * 2 >= thickness) {
                if ((Mentions(id, L"ShowDesktop") || Mentions(cls, L"ShowDesktop")) && length <= namedMax) score = 2;
                else if (type == UIA_ButtonControlTypeId && length <= thinMax) score = 1;
            }
            if (score > best) {
                best = score;
                *found = r;
            }
            SysFreeString(id);
            SysFreeString(cls);
            element->Release();
        }
    }
    if (items) items->Release();
    if (all) all->Release();
    if (cache) cache->Release();
    if (root) root->Release();
    return best > 0;
}

DWORD WINAPI ScanThread(void*) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 0;
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    while (uia && WaitForSingleObject(g_scanEvent, INFINITE) == WAIT_OBJECT_0 && !g_stopping) {
        HWND taskbars[kMaxTaskbars];
        const int count = CornerButtonDisabled() ? 0 : FindXamlTaskbars(taskbars, kMaxTaskbars);
        XamlButton found[kMaxTaskbars];
        int foundCount = 0;
        for (int i = 0; i < count; ++i) {
            RECT tb, r;
            if (!GetWindowRect(taskbars[i], &tb) || !LocateXamlButton(uia, taskbars[i], &r)) continue;
            Log(L"xaml: taskbar %p button (%ld,%ld)-(%ld,%ld)", taskbars[i], r.left, r.top, r.right, r.bottom);
            OffsetRect(&r, -tb.left, -tb.top);
            found[foundCount++] = {taskbars[i], {tb.right - tb.left, tb.bottom - tb.top}, r};
        }
        AcquireSRWLockExclusive(&g_lock);
        memcpy(g_buttons, found, sizeof(XamlButton) * foundCount);
        g_buttonCount = foundCount;
        ReleaseSRWLockExclusive(&g_lock);
        g_haveXaml.store(foundCount > 0, std::memory_order_release);
    }
    if (uia) uia->Release();
    CoUninitialize();
    return 0;
}

}  // namespace

bool IsTaskbar(HWND hwnd) {
    return hwnd && (ClassIs(hwnd, L"Shell_TrayWnd") || ClassIs(hwnd, L"Shell_SecondaryTrayWnd"));
}

HWND ShowDesktopHit(POINT pt) {
    // Called for every left button press, so only cheap, non-blocking calls here.
    const HWND hit = WindowFromPoint(pt);
    if (!hit) return nullptr;
    const HWND root = GetAncestor(hit, GA_ROOT);
    if (!IsTaskbar(root)) return nullptr;
    if (ClassIs(hit, kButtonClass)) return hit;
    return g_haveXaml.load(std::memory_order_acquire) && XamlTaskbarAt(pt) == root ? hit : nullptr;
}

HWND HoverHit(POINT pt) {
    HWND candidate = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_classicCount && !candidate; ++i) {
        RECT r;
        if (GetWindowRect(g_classic[i], &r) && PtInRect(&r, pt)) candidate = g_classic[i];
    }
    ReleaseSRWLockShared(&g_lock);
    if (!candidate && g_haveXaml.load(std::memory_order_acquire)) candidate = XamlTaskbarAt(pt);
    // Only now hit-test, to make sure nothing (a full-screen app, a menu) covers the button.
    return candidate ? ShowDesktopHit(pt) : nullptr;
}

void Refresh() {
    RefreshClassicButtons();
    if (!g_scanThread) {
        HWND any[1];
        if (!FindXamlTaskbars(any, 1)) return;  // classic taskbars only: UI Automation is never loaded
        g_scanEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_scanThread = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
        if (!g_scanThread) return;
    }
    SetEvent(g_scanEvent);
}

namespace {
constexpr wchar_t kAdvancedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
}

bool NativePeekEnabled() {
    DWORD disabled = 1, size = sizeof(disabled);
    return RegGetValueW(HKEY_CURRENT_USER, kAdvancedKey, L"DisablePreviewDesktop", RRF_RT_REG_DWORD, nullptr, &disabled,
                        &size) == ERROR_SUCCESS &&
           disabled == 0;
}

void EndPeek(HWND button) {
    // Hovering the button for a moment makes every window on every monitor transparent. Explorer ends that
    // when the button is clicked, but the click never reaches it now; telling the button the mouse left
    // has the same effect.
    if (button && ClassIs(button, kButtonClass) && NativePeekEnabled()) PostMessageW(button, WM_MOUSELEAVE, 0, 0);
}

bool TurnOffPeek() {
    // The same setting as "Peek at desktop" in the button's context menu.
    if (!NativePeekEnabled()) return false;
    const DWORD disabled = 1;
    if (RegSetKeyValueW(HKEY_CURRENT_USER, kAdvancedKey, L"DisablePreviewDesktop", REG_DWORD, &disabled,
                        sizeof(disabled)) != ERROR_SUCCESS)
        return false;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"), SMTO_ABORTIFHUNG,
                        1000, nullptr);
    return true;
}

void Shutdown() {
    if (!g_scanThread) return;
    g_stopping = true;
    SetEvent(g_scanEvent);
    WaitForSingleObject(g_scanThread, 3000);
    CloseHandle(g_scanThread);
    CloseHandle(g_scanEvent);
    g_scanThread = nullptr;
}

}  // namespace taskbar
