// SmartCollapse entry point: single instance, notification area icon and settings.
//
//   SmartCollapse.exe              start (or tell the running instance to say hello)
//   SmartCollapse.exe --toggle [N] collapse / restore monitor N (\\.\DISPLAYN), default: the one under the cursor
//   SmartCollapse.exe --exit       stop the running instance
//   SmartCollapse.exe --replace    stop the running instance and take over (used to gain or drop admin rights)
//   SmartCollapse.exe --log        write diagnostics to %TEMP%\SmartCollapse.log
#include "common.h"
#include <objbase.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wctype.h>
#include "../res/resource.h"

namespace {

constexpr wchar_t kAppName[] = L"SmartCollapse";
constexpr wchar_t kWindowClass[] = L"SmartCollapse.Main";
constexpr wchar_t kMutexName[] = L"Local\\SmartCollapse.Instance";
constexpr wchar_t kSettingsKey[] = L"Software\\SmartCollapse";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

constexpr wchar_t kAboutText[] =
    L"Makes the taskbar's Show desktop button work on the monitor you click it on.\n\n"
    L"Author: <a href=\"https://github.com/zalexdev\">zalexdev</a>\n"
    L"Source: <a href=\"https://github.com/zalexdev/SmartCollapse\">github.com/zalexdev/SmartCollapse</a>\n\n"
    L"Found a bug or have an idea? <a href=\"https://github.com/zalexdev/SmartCollapse/issues\">Open an issue</a>";

constexpr UINT kTrayId = 1;
enum : UINT_PTR { kWatchdogTimer = 1, kTrayRetryTimer, kRefreshTimer, kRefreshSettleTimer };
enum : UINT { kMenuEnabled = 100, kMenuPeek, kMenuAutostart, kMenuAdmin, kMenuAbout, kMenuExit };
enum : ULONG_PTR { kCommandHello = 1, kCommandExit, kCommandToggle };

HINSTANCE g_instance;
HWND g_window;
HWND g_about;  // the About dialog while it is open
UINT g_taskbarCreated;
HICON g_iconOn, g_iconOff, g_iconLarge;
HBITMAP g_shield;
bool g_enabled = true;
bool g_elevated;
int g_trayRetries;

// ---- settings ----------------------------------------------------------------------------------------

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS
               ? value
               : fallback;
}

void WriteSetting(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof(value));
}

std::wstring AutostartCommand() {
    return L"\"" + admin::ExecutablePath() + L"\"";
}

std::wstring ReadRunValue() {
    wchar_t value[1024];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
    return value;
}

void WriteRunValue() {
    const std::wstring command = AutostartCommand();
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName, REG_SZ, command.c_str(),
                    DWORD((command.size() + 1) * sizeof(wchar_t)));
}

bool RunValueEnabled() {
    if (ReadRunValue().empty()) return false;
    // Disabling the entry in Task Manager keeps the Run value and flags it here (odd first byte = disabled).
    BYTE approved[16];
    DWORD size = sizeof(approved);
    return !(RegGetValueW(HKEY_CURRENT_USER, kStartupApprovedKey, kAppName, RRF_RT_REG_BINARY, nullptr, approved, &size) ==
                 ERROR_SUCCESS &&
             size > 0 && (approved[0] & 1));
}

// Autostart is a Run key entry normally. With admin rights it is a logon task instead: Windows skips Run
// entries that need elevation, while the task starts us elevated without a UAC prompt.
bool AutostartEnabled() {
    return g_elevated ? admin::TaskExists() : RunValueEnabled();
}

void SetAutostart(bool on) {
    if (g_elevated) {
        if (on) admin::CreateTask();
        else admin::DeleteTask();
    }
    if (on && !g_elevated) WriteRunValue();
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kStartupApprovedKey, kAppName);
}

// ---- notification area icon ----------------------------------------------------------------------------

NOTIFYICONDATAW TrayData(UINT flags) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayId;
    data.uFlags = flags;
    return data;
}

void UpdateTray(bool add) {
    NOTIFYICONDATAW data = TrayData(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    data.uCallbackMessage = WM_APP_TRAY;
    data.hIcon = g_enabled ? g_iconOn : g_iconOff;
    wcscpy_s(data.szTip, !g_enabled  ? L"SmartCollapse (paused)"
                         : g_elevated ? L"SmartCollapse (admin)\nShow desktop works per monitor"
                                      : L"SmartCollapse\nShow desktop works per monitor");
    if (!add) {
        Shell_NotifyIconW(NIM_MODIFY, &data);
        return;
    }
    Shell_NotifyIconW(NIM_DELETE, &data);
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        KillTimer(g_window, kTrayRetryTimer);
    } else if (g_trayRetries++ < 30) {
        SetTimer(g_window, kTrayRetryTimer, 2000, nullptr);  // the shell may not be up yet right after sign-in
    }
}

void RemoveTray() {
    NOTIFYICONDATAW data = TrayData(0);
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void ShowBalloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW data = TrayData(NIF_INFO);
    data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    data.hBalloonIcon = g_iconLarge;
    wcscpy_s(data.szInfoTitle, title);
    wcscpy_s(data.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void LoadIcons() {
    const int tray = GetSystemMetrics(SM_CXSMICON), balloon = GetSystemMetrics(SM_CXICON);
    g_iconOn = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, tray, tray, 0));
    g_iconOff = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_PAUSED), IMAGE_ICON, tray, tray, 0));
    g_iconLarge = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, balloon, balloon, 0));
}

// The UAC shield shown next to "Escalate to admin", as the 32-bit bitmap a menu item can carry.
HBITMAP ShieldBitmap() {
    if (g_shield) return g_shield;
    SHSTOCKICONINFO stock{sizeof(stock)};
    if (FAILED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &stock))) return nullptr;
    const int size = GetSystemMetrics(SM_CXSMICON);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits;
    const HDC dc = CreateCompatibleDC(nullptr);
    g_shield = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (g_shield) {
        const HGDIOBJ old = SelectObject(dc, g_shield);
        DrawIconEx(dc, 0, 0, stock.hIcon, size, size, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
    }
    DeleteDC(dc);
    DestroyIcon(stock.hIcon);
    return g_shield;
}

// Lets our popup menu follow the system dark mode, through uxtheme exports that are undocumented but
// unchanged since Windows 10 1903 (build 18362).
void AllowDarkMenu(HWND hwnd) {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW version{sizeof(version)};
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!rtlGetVersion || rtlGetVersion(&version) != 0 || version.dwBuildNumber < 18362) return;
    const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
    using FlushMenuThemesFn = void(WINAPI*)();
    if (auto f = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)))) f(1);  // AllowDark
    if (auto f = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)))) f(hwnd, TRUE);
    if (auto f = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)))) f();
}

// ---- behavior ------------------------------------------------------------------------------------------

// Windows' own "Peek at desktop" fades out the windows of every monitor, so it can't run alongside the
// per-monitor one. SmartCollapse borrows it: switched off while its own peek works, and switched back on
// once that stops (paused, peek turned off, exit), but only if SmartCollapse was the one that switched it
// off. notify = false while the session ends: Explorer reads the setting again at the next sign-in.
void BorrowWindowsPeek(bool borrow, bool notify = true) {
    if (borrow) {
        if (taskbar::SetNativePeek(false, notify)) {
            WriteSetting(L"WindowsPeekBorrowed", 1);
            Log(L"windows peek: borrowed");
        }
        return;
    }
    if (!ReadSetting(L"WindowsPeekBorrowed", 0)) return;
    WriteSetting(L"WindowsPeekBorrowed", 0);
    if (taskbar::NativePeekEnabled()) {
        // Ticked again by hand in the meantime: Windows' peek is the one wanted, so ours stays off.
        peek::SetEnabled(false);
        WriteSetting(L"HoverPeek", 0);
        Log(L"windows peek: turned back on by hand, hover peek off");
    } else {
        taskbar::SetNativePeek(true, notify);
        Log(L"windows peek: given back");
    }
}

void SetEnabled(bool enabled) {
    g_enabled = enabled;
    WriteSetting(L"Enabled", enabled);
    hook::SetActive(enabled);
    if (!enabled) peek::Hover(nullptr);
    BorrowWindowsPeek(enabled && peek::Enabled());
    UpdateTray(false);
    Log(L"%ls", enabled ? L"enabled" : L"paused");
}

void SetPeek(bool enabled) {
    peek::SetEnabled(enabled);
    WriteSetting(L"HoverPeek", enabled);
    BorrowWindowsPeek(g_enabled && enabled);
}

void Escalate() {
    // The elevated instance asks this one to exit as soon as it is up (--replace).
    if (!admin::RunElevated(L"--replace") && GetLastError() != ERROR_CANCELLED)
        ShowBalloon(L"Couldn't get administrator rights", L"Windows didn't start SmartCollapse elevated.");
}

void DropAdmin() {
    if (!admin::RunUnelevated(admin::ExecutablePath().c_str(), L"--replace")) {
        ShowBalloon(L"Couldn't drop administrator rights", L"Exit SmartCollapse and start it again normally.");
        return;
    }
    // The normal instance takes over; autostart goes back to the Run key.
    if (admin::TaskExists()) {
        admin::DeleteTask();
        WriteRunValue();
    }
}

// Links open in the browser as the normal user, even while SmartCollapse runs as admin.
void OpenLink(const wchar_t* url) {
    if (!g_elevated || !admin::RunUnelevated(url, L""))
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

HRESULT CALLBACK AboutCallback(HWND dialog, UINT notification, WPARAM, LPARAM lParam, LONG_PTR) {
    switch (notification) {
    case TDN_CREATED: g_about = dialog; break;
    case TDN_DESTROYED: g_about = nullptr; break;
    case TDN_HYPERLINK_CLICKED: OpenLink(reinterpret_cast<const wchar_t*>(lParam)); break;
    }
    return S_OK;
}

void ShowAbout() {
    if (g_about) {
        SetForegroundWindow(g_about);
        return;
    }
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hInstance = g_instance;
    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.pszWindowTitle = L"About SmartCollapse";
    config.hMainIcon = g_iconLarge;
    config.pszMainInstruction = L"SmartCollapse " SC_VERSION_TEXT;
    config.pszContent = kAboutText;
    config.pfCallback = AboutCallback;
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

void ShowMenu(POINT pt) {
    const HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_enabled ? MF_CHECKED : 0), kMenuEnabled, L"&Enabled");
    AppendMenuW(menu, MF_STRING | (peek::Enabled() ? MF_CHECKED : 0), kMenuPeek, L"&Peek at desktop on hover");
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kMenuAutostart, L"&Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (g_elevated) {
        AppendMenuW(menu, MF_STRING | MF_CHECKED, kMenuAdmin, L"Running as ad&min");
    } else {
        AppendMenuW(menu, MF_STRING, kMenuAdmin, L"Escalate to ad&min");
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_BITMAP;
        item.hbmpItem = ShieldBitmap();
        SetMenuItemInfoW(menu, kMenuAdmin, FALSE, &item);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Shift+click Show desktop: all monitors");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuAbout, L"&About SmartCollapse");
    AppendMenuW(menu, MF_STRING, kMenuExit, L"E&xit");
    SetForegroundWindow(g_window);  // lets the menu close when the user clicks elsewhere
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align,
                                          pt.x, pt.y, g_window, nullptr);
    PostMessageW(g_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (command) {
    case kMenuEnabled: SetEnabled(!g_enabled); break;
    case kMenuPeek: SetPeek(!peek::Enabled()); break;
    case kMenuAutostart: SetAutostart(!AutostartEnabled()); break;
    case kMenuAdmin:
        if (g_elevated) DropAdmin();
        else Escalate();
        break;
    case kMenuAbout: ShowAbout(); break;
    case kMenuExit: DestroyWindow(g_window); break;
    }
}

HMONITOR MonitorByNumber(LONG number) {
    if (number <= 0) {
        POINT pt{};
        GetCursorPos(&pt);
        return MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }
    struct Context {
        LONG number;
        HMONITOR found;
    } context{number, nullptr};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Context*>(param);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            const wchar_t* name = GetMonitorInfoW(monitor, &info) ? wcsstr(info.szDevice, L"DISPLAY") : nullptr;
            if (name && _wtoi(name + 7) == c->number) {
                c->found = monitor;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
    return context.found;
}

void HandleCommand(WPARAM command, LPARAM argument) {
    switch (command) {
    case kCommandExit: DestroyWindow(g_window); break;
    case kCommandToggle:
        if (const HMONITOR monitor = MonitorByNumber(LONG(argument))) desktop::Toggle(monitor);
        break;
    case kCommandHello:
        ShowBalloon(L"SmartCollapse is already running", L"Use its icon in the notification area to pause it or change settings.");
        break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_APP_TOGGLE: {
        const HWND button = reinterpret_cast<HWND>(wParam);
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        HMONITOR monitor = MonitorFromWindow(GetAncestor(button, GA_ROOT), MONITOR_DEFAULTTONULL);
        if (!monitor) monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        desktop::Toggle(monitor);
        // Peeks end after the toggle, so that windows being minimized don't flash back first.
        taskbar::EndPeek(button);
        peek::Clicked();
        return 0;
    }
    case WM_APP_HOVER:
        peek::Hover(reinterpret_cast<HWND>(wParam));
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_CONTEXTMENU: ShowMenu({GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)}); break;
        }
        return 0;
    case WM_APP_COMMAND:
        HandleCommand(wParam, lParam);
        return 0;
    case WM_COPYDATA: {
        // Runs inside the other instance's SendMessage: only queue the work.
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        LONG argument = 0;
        if (data->cbData == sizeof(argument)) memcpy(&argument, data->lpData, sizeof(argument));
        PostMessageW(hwnd, WM_APP_COMMAND, data->dwData, argument);
        return TRUE;
    }
    case WM_TIMER:
        switch (wParam) {
        case kWatchdogTimer: hook::Watchdog(); break;
        case kTrayRetryTimer: UpdateTray(true); break;
        case kRefreshSettleTimer: KillTimer(hwnd, kRefreshSettleTimer); [[fallthrough]];
        case kRefreshTimer: taskbar::Refresh(); break;
        }
        return 0;
    case WM_DISPLAYCHANGE:
        peek::Cancel();
        [[fallthrough]];
    case WM_SETTINGCHANGE:
    case WM_DPICHANGED:
        SetTimer(hwnd, kRefreshSettleTimer, 1500, nullptr);  // re-locate the buttons once the taskbars settle
        break;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC && g_enabled) hook::SetActive(true);
        break;
    case WM_ENDSESSION:
        if (wParam) {
            RemoveTray();
            BorrowWindowsPeek(false, false);  // the process ends without leaving the message loop
        }
        return 0;
    case WM_DESTROY:
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == g_taskbarCreated && msg) {  // Explorer (re)started
            UpdateTray(true);
            SetTimer(hwnd, kRefreshSettleTimer, 2000, nullptr);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Sends a command to the running instance; false if there is none.
bool SendToRunning(ULONG_PTR command, LONG argument) {
    const HWND other = FindWindowW(kWindowClass, nullptr);
    if (!other) return false;
    COPYDATASTRUCT data{command, sizeof(argument), &argument};
    SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000, nullptr);
    return true;
}

// The single-instance mutex, or null while another instance holds it. A normal process can't even open
// the mutex of an elevated instance (access denied); that means "another instance" just the same.
HANDLE ClaimInstance() {
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex && GetLastError() != ERROR_ALREADY_EXISTS) return mutex;
    if (mutex) CloseHandle(mutex);
    return nullptr;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;

    ULONG_PTR command = kCommandHello;
    LONG argument = 0;
    bool forwardOnly = false, replace = false, log = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--exit") == 0) {
                command = kCommandExit;
                forwardOnly = true;
            } else if (_wcsicmp(argv[i], L"--toggle") == 0) {
                command = kCommandToggle;
                forwardOnly = true;
                if (i + 1 < argc && iswdigit(argv[i + 1][0])) argument = _wtoi(argv[++i]);
            } else if (_wcsicmp(argv[i], L"--replace") == 0) {
                replace = true;
            } else if (_wcsicmp(argv[i], L"--log") == 0) {
                log = true;
            }
        }
        LocalFree(argv);
    }

    if (forwardOnly) {
        SendToRunning(command, argument);
        return 0;
    }
    if (replace) SendToRunning(kCommandExit, 0);
    HANDLE mutex = ClaimInstance();
    for (int wait = 0; !mutex && replace && wait < 100; ++wait) {  // the old instance is still shutting down
        Sleep(50);
        mutex = ClaimInstance();
    }
    if (!mutex) {
        SendToRunning(kCommandHello, 0);
        return 0;
    }

    if (log) {
        wchar_t path[MAX_PATH + 32];
        const DWORD len = GetTempPathW(MAX_PATH, path);
        if (len && len < MAX_PATH) {
            wcscat_s(path, L"SmartCollapse.log");
            LogOpen(path);
        }
    }
    g_elevated = admin::IsElevated();
    Log(L"SmartCollapse started, pid %lu%ls", GetCurrentProcessId(), g_elevated ? L", elevated" : L"");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // Task Scheduler and the shell, for admin rights

    // Keep the hook thread at full speed even when Windows throttles background processes.
    PROCESS_POWER_THROTTLING_STATE throttling{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                              PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    // A hidden top-level window rather than a message-only one: TaskbarCreated is broadcast to top-level windows.
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                               instance, nullptr);
    if (!g_window) return 1;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Elevated, we'd otherwise not hear from Explorer (tray icon) or from a normal instance (commands).
    ChangeWindowMessageFilterEx(g_window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_window, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_window, WM_APP_TRAY, MSGFLT_ALLOW, nullptr);
    AllowDarkMenu(g_window);
    LoadIcons();
    peek::Init(instance);

    g_enabled = ReadSetting(L"Enabled", 1) != 0;
    const bool firstRun = ReadSetting(L"Installed", 0) == 0;
    if (firstRun) {
        SetAutostart(true);
        WriteSetting(L"Installed", 1);
    } else if (g_elevated && RunValueEnabled()) {
        // Just escalated: a Run entry can't start us elevated, so autostart becomes the logon task.
        if (admin::CreateTask()) RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName);
    } else if (const std::wstring run = ReadRunValue(); !run.empty() && run != AutostartCommand()) {
        WriteRunValue();  // the exe was moved: keep autostart pointing at it
    }
    peek::SetEnabled(ReadSetting(L"HoverPeek", 1) != 0);
    BorrowWindowsPeek(g_enabled && peek::Enabled());

    desktop::Init();
    if (!hook::Start(g_window)) return 1;
    if (g_enabled) hook::SetActive(true);
    UpdateTray(true);
    SetTimer(g_window, kWatchdogTimer, 2000, nullptr);
    SetTimer(g_window, kRefreshTimer, 20000, nullptr);
    taskbar::Refresh();
    if (firstRun)
        ShowBalloon(L"SmartCollapse is running",
                    L"Show desktop now hides only the windows on the monitor you click, and hovering it peeks at that "
                    L"monitor only. Right-click the icon for options.");
    else if (replace)
        ShowBalloon(g_elevated ? L"SmartCollapse runs as admin" : L"SmartCollapse runs without admin rights",
                    g_elevated ? L"Windows of apps running as administrator are collapsed too."
                               : L"Windows of apps running as administrator stay where they are.");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    hook::Stop();
    // Before the mutex goes: an instance taking over (--replace) borrows it again only after this.
    BorrowWindowsPeek(false);
    taskbar::Shutdown();
    if (g_shield) DeleteObject(g_shield);
    CoUninitialize();
    Log(L"SmartCollapse stopped");
    CloseHandle(mutex);
    return 0;
}
