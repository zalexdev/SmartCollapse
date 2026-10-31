// SmartCollapse: makes the taskbar "Show desktop" button act on its own monitor only.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

// Messages handled by the main window.
enum : UINT {
    WM_APP_TOGGLE = WM_APP + 1,  // from the hook: wParam = the clicked button's window, lParam = click point
    WM_APP_TRAY,                 // notification icon callback
    WM_APP_COMMAND,              // from another instance: wParam = command, lParam = argument
    WM_APP_HOVER,                // from the hook: wParam = the Show desktop button now under the cursor, or null
};

// dwExtraInfo of input we synthesize ourselves; the hook lets it through untouched.
constexpr ULONG_PTR kSyntheticInputTag = 0x53434F4C;  // "SCOL"

// log.cpp: optional diagnostics, enabled with --log.
void LogOpen(const wchar_t* path);
void Log(_Printf_format_string_ const wchar_t* format, ...);

// hook.cpp: low-level mouse hook on a dedicated thread.
namespace hook {
bool Start(HWND notify);
void Stop();
void SetActive(bool active);
void Watchdog();  // reinstalls the hook if Windows silently dropped it
}

// taskbar.cpp: finding the Show desktop button.
namespace taskbar {
bool IsTaskbar(HWND hwnd);
HWND ShowDesktopHit(POINT pt);  // hook thread: the window under pt if it is a Show desktop button, or null
HWND HoverHit(POINT pt);        // hook thread, often while the mouse moves: same answer, from cached positions
void Refresh();                 // re-locate the buttons (main thread)
bool NativePeekEnabled();       // Windows' own "Peek at desktop"
void EndPeek(HWND button);      // end Windows' peek if hovering the button started it
bool SetNativePeek(bool on, bool notify);  // switch Windows' peek; true if it changed. notify: tell Explorer now
void Shutdown();
}

// peek.cpp: "Peek at desktop" for the hovered monitor only.
namespace peek {
void Init(HINSTANCE instance);
void SetEnabled(bool enabled);
bool Enabled();
void Hover(HWND button);  // the Show desktop button under the cursor changed (null: none)
void Clicked();           // the button was clicked: end the peek, none until the cursor leaves
void Cancel();            // hide at once (display change)
}

// desktop.cpp: collapsing and restoring the windows of one monitor.
namespace desktop {
void Init();  // picks up what an earlier run left collapsed
void Toggle(HMONITOR monitor);
}

// admin.cpp: running with administrator rights, to reach windows of elevated apps.
namespace admin {
std::wstring ExecutablePath();
bool IsElevated();
bool RunElevated(const wchar_t* arguments);                        // ourselves, through UAC
bool RunUnelevated(const wchar_t* file, const wchar_t* arguments);  // through Explorer, from an elevated process
bool TaskExists();                             // the logon task that starts SmartCollapse elevated
bool CreateTask();
bool DeleteTask();
}
