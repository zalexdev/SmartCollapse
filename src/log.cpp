// Optional diagnostics log (UTF-8, appended), enabled with --log.
#include "common.h"
#include <stdarg.h>
#include <stdio.h>

namespace {
HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
}

void LogOpen(const wchar_t* path) {
    g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(const wchar_t* format, ...) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    wchar_t line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int len = swprintf_s(line, L"%02u:%02u:%02u.%03u [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                         GetCurrentThreadId());
    va_list args;
    va_start(args, format);
    const int body = _vsnwprintf_s(line + len, ARRAYSIZE(line) - len - 2, _TRUNCATE, format, args);
    va_end(args);
    len = body < 0 ? ARRAYSIZE(line) - 3 : len + body;
    line[len++] = L'\r';
    line[len++] = L'\n';
    char utf8[3 * ARRAYSIZE(line)];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, len, utf8, sizeof(utf8), nullptr, nullptr);
    AcquireSRWLockExclusive(&g_lock);
    DWORD written;
    WriteFile(g_file, utf8, bytes, &written, nullptr);
    ReleaseSRWLockExclusive(&g_lock);
}
