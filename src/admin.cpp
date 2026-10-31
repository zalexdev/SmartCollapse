// Running with administrator rights, so that windows of elevated apps can be collapsed too.
//
// Windows doesn't let a normal process minimize the windows of an elevated one. "Escalate to admin"
// relaunches SmartCollapse through UAC and the new instance takes over. Its autostart then moves from the
// Run key, which can't start anything elevated, to a Task Scheduler logon task with highest privileges,
// which starts it elevated without a UAC prompt at every sign-in.
#include "common.h"
#include <ole2.h>  // WIN32_LEAN_AND_MEAN leaves out the COM declarations the shell headers rely on
#include <shellapi.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>
#include <taskschd.h>
#include <sddl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace admin {
namespace {

constexpr wchar_t kTaskName[] = L"SmartCollapse";

class Bstr {
public:
    explicit Bstr(const wchar_t* text) : value_(SysAllocString(text)) {}
    ~Bstr() { SysFreeString(value_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return value_; }

private:
    BSTR value_;
};

std::wstring UserSid() {
    std::wstring sid;
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return sid;
    BYTE buffer[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)];
    DWORD size;
    wchar_t* text;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &text)) {
        sid = text;
        LocalFree(text);
    }
    CloseHandle(token);
    return sid;
}

ComPtr<ITaskService> Scheduler() {
    ComPtr<ITaskService> service;
    const VARIANT none{};
    if (FAILED(CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service))) ||
        FAILED(service->Connect(none, none, none, none)))
        return nullptr;
    return service;
}

ComPtr<ITaskFolder> RootFolder(ITaskService* service) {
    ComPtr<ITaskFolder> folder;
    if (service) service->GetFolder(Bstr(L"\\"), &folder);
    return folder;
}

}  // namespace

std::wstring ExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length;
    while ((length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()))) >= path.size())
        path.resize(path.size() * 2);
    path.resize(length);
    return path;
}

bool IsElevated() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                          elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

bool RunElevated(const wchar_t* arguments) {
    const std::wstring exe = ExecutablePath();
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = arguments;
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

bool RunUnelevated(const wchar_t* file, const wchar_t* arguments) {
    // An elevated process can't simply start a normal one. Explorer can: ask the desktop's own shell
    // view to launch it (the documented way around, see "The Old New Thing", 2013-11-18).
    ComPtr<IShellWindows> shellWindows;
    if (FAILED(CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shellWindows))))
        return false;
    VARIANT location{};
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VARIANT none{};
    long hwnd = 0;
    ComPtr<IDispatch> dispatch;
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background;
    ComPtr<IShellFolderViewDual> folderView;
    ComPtr<IDispatch> application;
    ComPtr<IShellDispatch2> shell;
    if (shellWindows->FindWindowSW(&location, &none, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &dispatch) != S_OK ||
        !dispatch || FAILED(dispatch.As(&provider)) ||
        FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) ||
        FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) || FAILED(background.As(&folderView)) ||
        FAILED(folderView->get_Application(&application)) || FAILED(application.As(&shell)))
        return false;

    const std::wstring exe = ExecutablePath();
    const std::wstring folder = exe.substr(0, exe.find_last_of(L'\\'));
    VARIANT args{}, directory{}, operation{}, show{};
    args.vt = VT_BSTR;
    args.bstrVal = SysAllocString(arguments);
    directory.vt = VT_BSTR;
    directory.bstrVal = SysAllocString(folder.c_str());
    operation.vt = VT_BSTR;
    operation.bstrVal = SysAllocString(L"");
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    const HRESULT hr = shell->ShellExecute(Bstr(file), args, directory, operation, show);
    VariantClear(&args);
    VariantClear(&directory);
    VariantClear(&operation);
    return SUCCEEDED(hr);
}

bool TaskExists() {
    const ComPtr<ITaskFolder> folder = RootFolder(Scheduler().Get());
    ComPtr<IRegisteredTask> task;
    return folder && SUCCEEDED(folder->GetTask(Bstr(kTaskName), &task));
}

bool CreateTask() {
    const ComPtr<ITaskService> service = Scheduler();
    const ComPtr<ITaskFolder> folder = RootFolder(service.Get());
    const std::wstring sid = UserSid(), exe = ExecutablePath();
    ComPtr<ITaskDefinition> task;
    ComPtr<IRegistrationInfo> registration;
    ComPtr<IPrincipal> principal;
    ComPtr<ITaskSettings> settings;
    ComPtr<ITriggerCollection> triggers;
    ComPtr<ITrigger> trigger;
    ComPtr<ILogonTrigger> logon;
    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> exec;
    if (!folder || sid.empty() || FAILED(service->NewTask(0, &task)) ||
        FAILED(task->get_RegistrationInfo(&registration)) || FAILED(task->get_Principal(&principal)) ||
        FAILED(task->get_Settings(&settings)) || FAILED(task->get_Triggers(&triggers)) ||
        FAILED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) || FAILED(trigger.As(&logon)) ||
        FAILED(task->get_Actions(&actions)) || FAILED(actions->Create(TASK_ACTION_EXEC, &action)) ||
        FAILED(action.As(&exec)))
        return false;

    registration->put_Description(Bstr(L"Starts SmartCollapse with administrator rights when you sign in."));
    principal->put_UserId(Bstr(sid.c_str()));
    principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    logon->put_UserId(Bstr(sid.c_str()));
    // Task Scheduler defaults suit batch jobs: stop after 3 days, not on battery, below-normal priority.
    settings->put_ExecutionTimeLimit(Bstr(L"PT0S"));
    settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    settings->put_Priority(4);
    settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
    exec->put_Path(Bstr(exe.c_str()));

    const VARIANT none{};
    ComPtr<IRegisteredTask> registered;
    const HRESULT hr = folder->RegisterTaskDefinition(Bstr(kTaskName), task.Get(), TASK_CREATE_OR_UPDATE, none, none,
                                                      TASK_LOGON_INTERACTIVE_TOKEN, none, &registered);
    Log(L"logon task: register -> %08lx", hr);
    return SUCCEEDED(hr);
}

bool DeleteTask() {
    const ComPtr<ITaskFolder> folder = RootFolder(Scheduler().Get());
    return folder && SUCCEEDED(folder->DeleteTask(Bstr(kTaskName), 0));
}

}  // namespace admin
