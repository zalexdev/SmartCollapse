// Per-monitor Show desktop.
//
// Collapse: the app windows on the monitor are minimized in parallel, without activation. If one of them
// had focus, focus moves to the desktop rather than to a window on another monitor.
// Restore: the saved windows are restored without activation (maximized windows come back maximized),
// re-stacked in their original Z-order, and focus returns to where it was. State is kept per monitor;
// windows collapsed on another virtual desktop wait until the user is back there.
//
// Calls on other processes' windows are asynchronous (ShowWindowAsync, SWP_ASYNCWINDOWPOS), so a hung
// application can only delay its own window, never SmartCollapse.
#include "common.h"
#include <dwmapi.h>
#include <algorithm>
#include <string>
#include <vector>

namespace desktop {
namespace {

struct Window {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    DWORD tid = 0;  // with pid, catches a recycled window handle
};

struct MonitorState {
    std::wstring device;          // \\.\DISPLAYn
    std::vector<Window> windows;  // windows we minimized, Z-order top to bottom
    Window focus;                 // re-activated on restore; empty when focus was on another monitor
    ULONGLONG collapsedAt = 0;
};

constexpr DWORD kSettleMs = 700;     // longest wait for minimize / restore animations
constexpr DWORD kInFlightMs = 2000;  // a window we just minimized that is still visible is not "restored"

std::vector<MonitorState> g_states;
std::vector<Window> g_untouchable;  // windows Windows refused to let us minimize (apps running as administrator)
bool g_busy;

Window Track(HWND hwnd) {
    Window w;
    w.hwnd = hwnd;
    w.tid = GetWindowThreadProcessId(hwnd, &w.pid);
    return w;
}

bool Alive(const Window& w) {
    DWORD pid = 0;
    return w.hwnd && GetWindowThreadProcessId(w.hwnd, &pid) == w.tid && pid == w.pid;
}

bool Contains(const std::vector<Window>& list, HWND hwnd) {
    return std::any_of(list.begin(), list.end(), [hwnd](const Window& w) { return w.hwnd == hwnd; });
}

bool IsUntouchable(HWND hwnd) {
    return std::any_of(g_untouchable.begin(), g_untouchable.end(),
                       [hwnd](const Window& w) { return w.hwnd == hwnd && Alive(w); });
}

bool IsShown(const Window& w) {  // on screen, not minimized
    return Alive(w) && IsWindowVisible(w.hwnd) && !IsIconic(w.hwnd);
}

bool IsMinimized(const Window& w) {
    return Alive(w) && IsIconic(w.hwnd);
}

template <class Pred>
bool AnyIndex(size_t count, Pred pred) {
    for (size_t i = 0; i < count; ++i)
        if (pred(i)) return true;
    return false;
}

// After a wait timed out: which windows were still not done.
template <class Pending>
void LogUnsettled(const wchar_t* what, const std::vector<Window>& windows, Pending pending) {
    for (size_t i = 0; i < windows.size(); ++i) {
        if (!pending(i)) continue;
        wchar_t cls[64] = L"?";
        GetClassNameW(windows[i].hwnd, cls, ARRAYSIZE(cls));
        Log(L"  %ls still pending after %lu ms: %p [%ls] pid %lu", what, kSettleMs, windows[i].hwnd, cls,
            windows[i].pid);
    }
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

bool IsTopmost(HWND hwnd) {
    return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

// Shell surfaces that are never collapsed, whatever their window styles say.
bool IsShellSurface(HWND hwnd) {
    static const wchar_t* const kClasses[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow",
        L"XamlExplorerHostIslandWindow", L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow",
        L"Shell_InputSwitchTopLevelWindow", L"MultitaskingViewFrame", L"TaskSwitcherWnd", L"ForegroundStaging",
        L"ApplicationManager_DesktopShellWindow",
    };
    if (hwnd == GetShellWindow()) return true;
    wchar_t cls[64];
    if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) return true;
    for (const wchar_t* name : kClasses)
        if (wcscmp(cls, name) == 0) return true;
    return false;
}

// What Show desktop hides: visible app windows with their own taskbar button, mostly on `monitor`.
// Owned windows (dialogs, palettes) disappear and come back together with their owner.
bool IsCollapsible(HWND hwnd, HMONITOR monitor) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_APPWINDOW) && ((ex & WS_EX_TOOLWINDOW) || GetWindow(hwnd, GW_OWNER))) return false;
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != monitor) return false;
    RECT r;
    if (!GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // Cloaked: on another virtual desktop, or a suspended UWP app. Hung: it would minimize whenever it recovers.
    // Untouchable: an elevated app we can't minimize; counting it as "still visible" would keep the monitor
    // from ever toggling back.
    return pid != GetCurrentProcessId() && !IsCloaked(hwnd) && !IsShellSurface(hwnd) && !IsHungAppWindow(hwnd) &&
           !IsUntouchable(hwnd);
}

std::vector<HWND> CollectWindows(HMONITOR monitor) {  // top to bottom
    struct Context {
        HMONITOR monitor;
        std::vector<HWND> found;
    } context{monitor, {}};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Context*>(param);
            if (IsCollapsible(hwnd, c->monitor)) c->found.push_back(hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
    return std::move(context.found);
}

// Polls `done` for up to `timeoutMs`. Sent messages are still serviced meanwhile, so broadcasts to our
// window (WM_SETTINGCHANGE and friends) never stall the applications sending them.
template <class Done>
bool WaitFor(DWORD timeoutMs, Done done) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (!done()) {
        if (GetTickCount64() >= deadline) return false;
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_SENDMESSAGE, 0);
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    return true;
}

// Windows lets only the process behind the latest input move the foreground. The click we act on was
// swallowed by the hook, so take that role with a no-op mouse event first.
bool Activate(HWND hwnd) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwExtraInfo = kSyntheticInputTag;
    SendInput(1, &input, sizeof(input));
    if (SetForegroundWindow(hwnd)) return true;
    Log(L"  SetForegroundWindow(%p) failed", hwnd);
    return false;
}

// The window hosting the desktop icons: Progman, or a WorkerW if a wallpaper tool moved them there.
// It stays at the bottom of the Z-order when activated.
HWND DesktopWindow() {
    const HWND progman = GetShellWindow();
    if (progman && FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)) return progman;
    for (HWND worker = nullptr; (worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr;)
        if (FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr)) return worker;
    return progman;
}

bool IsShellFocus(HWND hwnd) {
    return !hwnd || IsShellSurface(hwnd) || taskbar::IsTaskbar(GetAncestor(hwnd, GA_ROOT));
}

bool IsBelow(HWND lower, HWND upper) {
    int guard = 0;
    for (HWND h = GetWindow(upper, GW_HWNDNEXT); h && guard++ < 10000; h = GetWindow(h, GW_HWNDNEXT))
        if (h == lower) return true;
    return false;
}

// Re-stacks `windows` (top to bottom) right below `anchor`. Each move is posted to the window's own
// thread and we wait for it to land before placing the next one. When a window already is below its
// predecessor we don't wait: its move can then only lift it closer to the predecessor, which keeps
// the order intact even if it lands late.
void Restack(const std::vector<Window>& windows, HWND anchor) {
    HWND after = anchor;
    for (const Window& w : windows) {
        if (!Alive(w) || IsTopmost(w.hwnd)) continue;  // topmost ones stay above everything we stack anyway
        if (!SetWindowPos(w.hwnd, after, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS))
            continue;
        if (after != HWND_TOP) WaitFor(50, [&] { return IsBelow(w.hwnd, after); });
        after = w.hwnd;
    }
}

// True until the user brings back any of the windows we minimized on this monitor (of this virtual desktop).
bool StillCollapsed(const MonitorState& state) {
    if (GetTickCount64() - state.collapsedAt < kInFlightMs) return true;  // still animating
    return std::none_of(state.windows.begin(), state.windows.end(),
                        [](const Window& w) { return IsShown(w) && !IsCloaked(w.hwnd); });
}

// Returns how many windows it minimized; 0 leaves `state` untouched.
size_t Collapse(MonitorState& state, const std::vector<HWND>& windows) {
    const HWND fg = GetForegroundWindow();
    const HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;

    std::vector<Window> minimized;
    for (HWND hwnd : windows) {
        if (ShowWindowAsync(hwnd, SW_SHOWMINNOACTIVE)) {
            minimized.push_back(Track(hwnd));
        } else {  // an elevated window, out of reach for a non-elevated process: leave it out from now on
            Log(L"  cannot minimize %p (error %lu), ignoring it", hwnd, GetLastError());
            g_untouchable.push_back(Track(hwnd));
        }
    }
    const size_t count = minimized.size();
    if (!count) return 0;

    // SW_SHOWMINNOACTIVE leaves focus on the (now minimized) window. Hand it to the desktop, like the
    // original button does, instead of letting it fall to a window on another monitor. Doing this after
    // queueing the minimizes matters: the other way round, Explorer windows react to losing focus
    // first and take noticeably longer to minimize.
    const bool focusHere = Contains(minimized, fg) || Contains(minimized, fgRoot);
    if (focusHere) Activate(DesktopWindow());

    // Windows from an earlier collapse stay restorable, below the newer ones: always if they belong to
    // another virtual desktop, and here as long as the user hasn't brought any of them back by hand
    // (say they only opened a file from the desktop meanwhile).
    const bool intact = StillCollapsed(state);
    for (const Window& w : state.windows)
        if (IsMinimized(w) && !Contains(minimized, w.hwnd) && (intact || IsCloaked(w.hwnd))) minimized.push_back(w);

    state.windows = std::move(minimized);
    state.focus = focusHere ? Track(fg) : Window{};
    state.collapsedAt = GetTickCount64();

    // Let the animations finish so that a quick second click sees the monitor as collapsed.
    const auto shown = [&](size_t i) { return IsShown(state.windows[i]); };
    if (!WaitFor(kSettleMs, [&] { return !AnyIndex(state.windows.size(), shown); }))
        LogUnsettled(L"minimize", state.windows, shown);
    return count;
}

// Restores what was collapsed on `monitor` of the current virtual desktop. Windows collapsed on another
// virtual desktop stay in `state` until the user is back there.
bool Restore(MonitorState& state, HMONITOR monitor) {
    std::vector<Window> windows, elsewhere;
    for (const Window& w : state.windows) {
        // Not minimized: the user brought it back already. Hidden: the app put itself into the tray.
        if (!IsMinimized(w) || !IsWindowVisible(w.hwnd)) continue;
        if (IsCloaked(w.hwnd)) elsewhere.push_back(w);  // lives on another virtual desktop
        // (For a minimized window this is the monitor it restores to; skips one the user moved meanwhile.)
        else if (MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTONEAREST) == monitor) windows.push_back(w);
    }
    state.windows = std::move(elsewhere);
    if (windows.empty()) return false;

    // Focus goes back to the window that had it when we collapsed, if that was one of these. Otherwise to the
    // top one, but only while focus idles on the desktop or taskbar: someone busy on another monitor keeps it.
    const HWND fg = GetForegroundWindow();
    HWND focus = nullptr;
    if (Alive(state.focus) &&
        (Contains(windows, state.focus.hwnd) || Contains(windows, GetAncestor(state.focus.hwnd, GA_ROOTOWNER))))
        focus = state.focus.hwnd;
    if (!focus && IsShellFocus(fg)) {
        for (const Window& w : windows) {
            if (!IsTopmost(w.hwnd)) {
                focus = w.hwnd;
                break;
            }
        }
    }

    // Restore them all at once. Every app's own thread carries out its restore, and Windows stacks those
    // in whatever order the threads happen to run, so the stacking is rebuilt afterwards. "Done" means the
    // window left its parking spot off-screen: IsIconic() turns false a few milliseconds before that.
    std::vector<RECT> parked(windows.size());
    for (size_t i = 0; i < windows.size(); ++i) {
        GetWindowRect(windows[i].hwnd, &parked[i]);
        if (IsIconic(windows[i].hwnd))  // re-checked: on a window that isn't minimized it would un-maximize it
            ShowWindowAsync(windows[i].hwnd, SW_SHOWNOACTIVATE);  // maximized windows come back maximized
    }
    const auto pending = [&](size_t i) {
        RECT r;
        return Alive(windows[i]) &&
               (IsIconic(windows[i].hwnd) || (GetWindowRect(windows[i].hwnd, &r) && EqualRect(&r, &parked[i])));
    };
    if (!WaitFor(kSettleMs, [&] { return !AnyIndex(windows.size(), pending); }))
        LogUnsettled(L"restore", windows, pending);

    // Original stacking: on top if one of these windows gets focus, otherwise right below the window the
    // user is working in.
    Restack(windows, focus || IsShellFocus(fg) || IsTopmost(fg) ? HWND_TOP : fg);
    if (focus) Activate(focus);
    return true;
}

MonitorState& StateFor(const wchar_t* device) {
    for (MonitorState& state : g_states)
        if (state.device == device) return state;
    g_states.push_back({device});
    return g_states.back();
}

// What is collapsed survives a restart of SmartCollapse: one REG_MULTI_SZ line per monitor,
// "device|hwnd:pid:tid of the focus window|hwnd:pid:tid hwnd:pid:tid ...". Window handles stay valid for the
// window's lifetime and pid/tid catch a recycled one, so stale entries simply drop out when loaded.
constexpr wchar_t kSettingsKey[] = L"Software\\SmartCollapse";
constexpr wchar_t kStateValue[] = L"Collapsed";

void AppendWindow(std::wstring& out, const Window& w, wchar_t separator) {
    wchar_t item[48];
    swprintf_s(item, L"%llx:%lu:%lu%c", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(w.hwnd)), w.pid, w.tid,
               separator);
    out += item;
}

const wchar_t* ParseWindow(const wchar_t* text, Window* w) {
    unsigned long long hwnd = 0;
    int used = 0;
    if (swscanf_s(text, L"%llx:%lu:%lu%n", &hwnd, &w->pid, &w->tid, &used) != 3) return nullptr;
    w->hwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(hwnd));
    return text + used;
}

void SaveState() {
    std::wstring data;
    for (const MonitorState& state : g_states) {
        if (state.windows.empty()) continue;
        data += state.device + L'|';
        AppendWindow(data, state.focus, L'|');
        for (const Window& w : state.windows) AppendWindow(data, w, L' ');
        data += L'\0';
    }
    if (data.empty()) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kSettingsKey, kStateValue);
        return;
    }
    data += L'\0';
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, kStateValue, REG_MULTI_SZ, data.c_str(),
                    DWORD(data.size() * sizeof(wchar_t)));
}

void LoadState() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kStateValue, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size == 0)
        return;
    std::wstring data(size / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kStateValue, RRF_RT_REG_MULTI_SZ, nullptr, data.data(), &size) !=
        ERROR_SUCCESS)
        return;
    for (const wchar_t* line = data.c_str(); *line; line += wcslen(line) + 1) {
        const wchar_t* bar = wcschr(line, L'|');
        if (!bar) continue;
        MonitorState state;
        state.device.assign(line, bar);
        const wchar_t* p = ParseWindow(bar + 1, &state.focus);
        if (!p || *p != L'|') continue;
        Window w;
        for (++p; (p = ParseWindow(p, &w)) != nullptr && *p == L' '; ++p)
            if (Alive(w)) state.windows.push_back(w);
        if (!state.windows.empty()) g_states.push_back(std::move(state));
    }
}

}  // namespace

void Init() {
    LoadState();
    SaveState();  // drops what no longer exists
    for (const MonitorState& state : g_states)
        Log(L"remembered %ls: %zu collapsed window(s)", state.device.c_str(), state.windows.size());
}

void Toggle(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (g_busy || !GetMonitorInfoW(monitor, &info)) {
        Log(L"toggle ignored (%ls)", g_busy ? L"busy" : L"unknown monitor");
        return;
    }
    g_busy = true;
    g_untouchable.erase(std::remove_if(g_untouchable.begin(), g_untouchable.end(), [](const Window& w) { return !Alive(w); }),
                        g_untouchable.end());
    MonitorState& state = StateFor(info.szDevice);

    std::vector<HWND> windows = CollectWindows(monitor);
    // A window still animating from our last collapse isn't visible "again".
    if (GetTickCount64() - state.collapsedAt < kInFlightMs)
        windows.erase(std::remove_if(windows.begin(), windows.end(),
                                     [&](HWND hwnd) { return Contains(state.windows, hwnd); }),
                      windows.end());

    const ULONGLONG start = GetTickCount64();
    // Visible windows that turn out to be out of reach (elevated apps) leave nothing to collapse: restore then.
    if (const size_t minimized = windows.empty() ? 0 : Collapse(state, windows)) {
        Log(L"collapse %ls: %zu window(s) minimized, %zu restorable, %llu ms", info.szDevice, minimized,
            state.windows.size(), GetTickCount64() - start);
    } else {
        const bool restored = Restore(state, monitor);
        Log(L"restore %ls: %ls, %llu ms, %zu kept for other virtual desktops", info.szDevice,
            restored ? L"done" : L"nothing to restore", GetTickCount64() - start, state.windows.size());
        state.collapsedAt = 0;
    }
    SaveState();
    g_busy = false;
}

}  // namespace desktop
