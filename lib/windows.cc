#include <cmath>
#include <cstdint>
#include <iostream>
#include <napi.h>
#include <shtypes.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <thread>
#include <atomic>
#include <chrono>

typedef int (__stdcall* lp_GetScaleFactorForMonitor) (HMONITOR, DEVICE_SCALE_FACTOR*);

// Global variables for window monitoring
static std::vector<HWINEVENTHOOK> g_hooks;
static Napi::ThreadSafeFunction g_tsfn;
static std::atomic<bool> g_monitoring(false);
static std::thread* g_monitorThread = nullptr;
static DWORD g_monitorThreadId = 0;

// Throttling globals
static std::chrono::steady_clock::time_point g_lastUpdateTime;
static UINT_PTR g_timerId = 0;
static const int THROTTLE_MS = 50;

struct Process {
    int pid;
    std::string path;
};

struct WindowData {
    int64_t id;
    std::string title;
    std::string path;
    int processId;
    struct {
        long x;
        long y;
        long width;
        long height;
    } bounds;
    int zOrder;
    bool isVisible;
};

template <typename T>
T getValueFromCallbackData (const Napi::CallbackInfo& info, unsigned handleIndex) {
    return reinterpret_cast<T> (info[handleIndex].As<Napi::Number> ().Int64Value ());
}

std::wstring get_wstring (const std::string str) {
    return std::wstring (str.begin (), str.end ());
}

std::string toUtf8 (const std::wstring& str) {
    std::string ret;
    int len = WideCharToMultiByte (CP_UTF8, 0, str.c_str (), str.length (), NULL, 0, NULL, NULL);
    if (len > 0) {
        ret.resize (len);
        WideCharToMultiByte (CP_UTF8, 0, str.c_str (), str.length (), &ret[0], len, NULL, NULL);
    }
    return ret;
}

Process getWindowProcess (HWND handle) {
    DWORD pid{ 0 };
    GetWindowThreadProcessId (handle, &pid);

    HANDLE pHandle{ OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, false, pid) };

    DWORD dwSize{ MAX_PATH };
    wchar_t exeName[MAX_PATH]{};

    QueryFullProcessImageNameW (pHandle, 0, exeName, &dwSize);

    CloseHandle (pHandle);

    auto wspath (exeName);
    auto path = toUtf8 (wspath);

    return { static_cast<int> (pid), path };
}

HWND find_top_window (DWORD pid) {
    std::pair<HWND, DWORD> params = { 0, pid };

    BOOL bResult = EnumWindows (
    [] (HWND hwnd, LPARAM lParam) -> BOOL {
        auto pParams = (std::pair<HWND, DWORD>*)(lParam);

        DWORD processId;
        if (GetWindowThreadProcessId (hwnd, &processId) && processId == pParams->second) {
            SetLastError (-1);
            pParams->first = hwnd;
            return FALSE;
        }

        return TRUE;
    },
    (LPARAM)&params);

    if (!bResult && GetLastError () == -1 && params.first) {
        return params.first;
    }

    return 0;
}

Napi::Number getProcessMainWindow (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    unsigned long process_id = info[0].ToNumber ().Uint32Value ();

    auto handle = find_top_window (process_id);

    return Napi::Number::New (env, reinterpret_cast<int64_t> (handle));
}

Napi::Number createProcess (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto path = info[0].ToString ().Utf8Value ();

    std::string cmd = "";

    if (info[1].IsString ()) {
        cmd = info[1].ToString ().Utf8Value ();
    }

    STARTUPINFOA sInfo = { sizeof (sInfo) };
    PROCESS_INFORMATION processInfo;
    CreateProcessA (path.c_str (), &cmd[0], NULL, NULL, FALSE,
                    CREATE_NEW_PROCESS_GROUP | CREATE_NEW_CONSOLE, NULL, NULL, &sInfo, &processInfo);

    return Napi::Number::New (env, processInfo.dwProcessId);
}

Napi::Number getActiveWindow (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle = GetForegroundWindow ();

    return Napi::Number::New (env, reinterpret_cast<int64_t> (handle));
}

std::vector<int64_t> _windows;

BOOL CALLBACK EnumWindowsProc (HWND hwnd, LPARAM lparam) {
    _windows.push_back (reinterpret_cast<int64_t> (hwnd));
    return TRUE;
}

Napi::Array getWindows (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    _windows.clear ();
    EnumWindows (&EnumWindowsProc, NULL);

    auto arr = Napi::Array::New (env);
    auto i = 0;
    for (auto _win : _windows) {
        arr.Set (i++, Napi::Number::New (env, _win));
    }

    return arr;
}

std::vector<int64_t> _monitors;

BOOL CALLBACK EnumMonitorsProc (HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    _monitors.push_back (reinterpret_cast<int64_t> (hMonitor));
    return TRUE;
}

Napi::Array getMonitors (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    _monitors.clear ();
    if (EnumDisplayMonitors (NULL, NULL, &EnumMonitorsProc, NULL)) {
        auto arr = Napi::Array::New (env);
        auto i = 0;

        for (auto _mon : _monitors) {

            arr.Set (i++, Napi::Number::New (env, _mon));
        }

        return arr;
    }

    return Napi::Array::New (env);
}

Napi::Number getMonitorFromWindow (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle = getValueFromCallbackData<HWND> (info, 0);

    return Napi::Number::New (env, reinterpret_cast<int64_t> (MonitorFromWindow (handle, 0)));
}

Napi::Object initWindow (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };
    auto process = getWindowProcess (handle);

    Napi::Object obj{ Napi::Object::New (env) };

    obj.Set ("processId", process.pid);
    obj.Set ("path", process.path);

    return obj;
}

Napi::Object getWindowBounds (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };

    RECT rect{};
    GetWindowRect (handle, &rect);

    Napi::Object bounds{ Napi::Object::New (env) };

    bounds.Set ("x", rect.left);
    bounds.Set ("y", rect.top);
    bounds.Set ("width", rect.right - rect.left);
    bounds.Set ("height", rect.bottom - rect.top);

    return bounds;
}

Napi::String getWindowTitle (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };

    int bufsize = GetWindowTextLengthW (handle) + 1;
    LPWSTR t = new WCHAR[bufsize];
    GetWindowTextW (handle, t, bufsize);

    std::wstring ws (t);
    std::string title = toUtf8 (ws);

    return Napi::String::New (env, title);
}

Napi::String getWindowName (const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env ();

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };

    wchar_t name[256];

    GetWindowTextW (handle, name, sizeof (name) / sizeof (name[0]));

    std::wstring ws (name);
    std::string str (ws.begin (), ws.end ());

    return Napi::String::New (env, str);
}

Napi::Number getWindowOpacity (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };

    BYTE opacity{};
    GetLayeredWindowAttributes (handle, NULL, &opacity, NULL);

    return Napi::Number::New (env, static_cast<double> (opacity) / 255.);
}

Napi::Number getWindowOwner (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };

    return Napi::Number::New (env, GetWindowLongPtrA (handle, GWLP_HWNDPARENT));
}

Napi::Number getMonitorScaleFactor (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    HMODULE hShcore{ LoadLibraryA ("SHcore.dll") };
    lp_GetScaleFactorForMonitor f{ (
    lp_GetScaleFactorForMonitor)GetProcAddress (hShcore, "GetScaleFactorForMonitor") };

    DEVICE_SCALE_FACTOR sf{};
    f (getValueFromCallbackData<HMONITOR> (info, 0), &sf);

    return Napi::Number::New (env, static_cast<double> (sf) / 100.);
}

Napi::Boolean toggleWindowTransparency (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };
    bool toggle{ info[1].As<Napi::Boolean> () };
    LONG_PTR style{ GetWindowLongPtrA (handle, GWL_EXSTYLE) };

    SetWindowLongPtrA (handle, GWL_EXSTYLE, ((toggle) ? (style | WS_EX_LAYERED) : (style & (~WS_EX_LAYERED))));

    return Napi::Boolean::New (env, true);
}

Napi::Boolean setWindowOpacity (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };
    double opacity{ info[1].As<Napi::Number> ().DoubleValue () };

    SetLayeredWindowAttributes (handle, NULL, opacity * 255., LWA_ALPHA);

    return Napi::Boolean::New (env, true);
}

Napi::Boolean setWindowBounds (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    Napi::Object bounds{ info[1].As<Napi::Object> () };
    auto handle{ getValueFromCallbackData<HWND> (info, 0) };

    BOOL b{ MoveWindow (handle, bounds.Get ("x").ToNumber (), bounds.Get ("y").ToNumber (),
                        bounds.Get ("width").ToNumber (), bounds.Get ("height").ToNumber (), true) };

    return Napi::Boolean::New (env, b);
}

Napi::Boolean setWindowParent (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };
    auto newOwner{ getValueFromCallbackData<HWND> (info, 1) };

    RECT rect{};
    GetClientRect (newOwner, &rect);

    // SetWindowLongPtrA (handle, GWLP_HWNDPARENT, newOwner);
    // SetWindowLongPtrA (handle, GWL_STYLE, WS_CHILD | WS_VISIBLE);
    SetParent (handle, newOwner);
    SetWindowPos (handle, 0, rect.left, rect.top, rect.right, rect.bottom, 0);
    SetActiveWindow (handle);

    return Napi::Boolean::New (env, true);
}

Napi::Boolean showWindow (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };
    std::string type{ info[1].As<Napi::String> () };

    DWORD flag{ 0 };

    if (type == "show")
        flag = SW_SHOW;
    else if (type == "hide")
        flag = SW_HIDE;
    else if (type == "minimize")
        flag = SW_MINIMIZE;
    else if (type == "restore")
        flag = SW_RESTORE;
    else if (type == "maximize")
        flag = SW_MAXIMIZE;

    return Napi::Boolean::New (env, ShowWindow (handle, flag));
}

Napi::Boolean bringWindowToTop (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };
    auto handle{ getValueFromCallbackData<HWND> (info, 0) };
    BOOL b{ SetForegroundWindow (handle) };

    HWND hCurWnd = ::GetForegroundWindow ();
    DWORD dwMyID = ::GetCurrentThreadId ();
    DWORD dwCurID = ::GetWindowThreadProcessId (hCurWnd, NULL);
    ::AttachThreadInput (dwCurID, dwMyID, TRUE);
    ::SetWindowPos (handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    ::SetWindowPos (handle, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    ::SetForegroundWindow (handle);
    ::AttachThreadInput (dwCurID, dwMyID, FALSE);
    ::SetFocus (handle);
    ::SetActiveWindow (handle);

    return Napi::Boolean::New (env, b);
}

Napi::Boolean redrawWindow (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };
    BOOL b{ SetWindowPos (handle, 0, 0, 0, 0, 0,
                          SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                          SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_DRAWFRAME | SWP_NOCOPYBITS) };

    return Napi::Boolean::New (env, b);
}

Napi::Boolean isWindow (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };

    return Napi::Boolean::New (env, IsWindow (handle));
}

Napi::Boolean isWindowVisible (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };

    return Napi::Boolean::New (env, IsWindowVisible (handle));
}

Napi::Number getWindowZOrder (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HWND> (info, 0) };

    // Count how many windows are above this one in the Z-order
    // Topmost window should return 0
    int zIndex = 0;
    HWND walker = handle;
    while (walker) {
        walker = GetWindow (walker, GW_HWNDPREV);
        if (walker)
            ++zIndex;
    }

    return Napi::Number::New (env, zIndex);
}

// ---------------------------------------------------------
// Thread-safe Window Data Collection & Conversion
// ---------------------------------------------------------

std::vector<WindowData> FetchWindowData() {
    std::vector<WindowData> result;

    // 1. Snapshot Z-Order
    std::unordered_map<HWND, int> zOrderMap;
    int currentZ = 0;
    HWND walker = GetTopWindow(NULL);
    while (walker) {
        zOrderMap[walker] = currentZ++;
        walker = GetWindow(walker, GW_HWNDNEXT);
    }

    // Load dwmapi.dll once thread-locally or statically
    static HMODULE hDwmapi = LoadLibraryA("dwmapi.dll");
    typedef HRESULT (WINAPI *DwmGetWindowAttributeProc)(HWND, DWORD, PVOID, DWORD);
    static DwmGetWindowAttributeProc pDwmGetWindowAttribute = nullptr;
    if (hDwmapi && !pDwmGetWindowAttribute) {
        pDwmGetWindowAttribute = (DwmGetWindowAttributeProc)GetProcAddress(hDwmapi, "DwmGetWindowAttribute");
    }

    // Enum context
    struct EnumContext {
        std::vector<WindowData>* result;
        std::unordered_map<HWND, int>* zOrderMap;
        DwmGetWindowAttributeProc pDwmGetWindowAttribute;
        std::vector<WCHAR> titleBuffer;
    } ctx;
    
    ctx.result = &result;
    ctx.zOrderMap = &zOrderMap;
    ctx.pDwmGetWindowAttribute = pDwmGetWindowAttribute;
    ctx.titleBuffer.resize(256);

    DWORD currentPid = GetCurrentProcessId();

    EnumWindows([](HWND handle, LPARAM lParam) -> BOOL {
        auto* context = reinterpret_cast<EnumContext*>(lParam);

        // Filter: only visible windows
        if (!IsWindowVisible(handle)) return TRUE;

        // Get process info first to check if it's our own process
        DWORD pid = 0;
        GetWindowThreadProcessId(handle, &pid);
        if (pid == 0) return TRUE;

        // Get title
        // For our own process, GetWindowText on a background thread requires sending a message to the main thread.
        // If the main thread is busy, this can hang. Use SendMessageTimeout to avoid hanging.
        // For other processes, GetWindowText reads from kernel structures and is fast/safe.
        int actualLen = 0;
        
        DWORD myPid = GetCurrentProcessId(); // Need to capture this or pass in context? 
        // We can't access local vars of enclosing function in lambda unless captured.
        // But GetCurrentProcessId() is a syscall, fast enough.
        
        if (pid == GetCurrentProcessId()) {
            // Own process: use SendMessageTimeout
            DWORD_PTR result = 0;
            // Provide a reasonable buffer size. context->titleBuffer is available.
            // We need to resize strictly before writing.
            if (context->titleBuffer.size() < 256) context->titleBuffer.resize(256);
            
            LRESULT res = SendMessageTimeoutW(
                handle, 
                WM_GETTEXT, 
                context->titleBuffer.size(), 
                reinterpret_cast<LPARAM>(context->titleBuffer.data()),
                SMTO_ABORTIFHUNG | SMTO_NORMAL,
                100, // 100ms timeout
                &result
            );
            
            if (res == 0) {
                // Timeout or failure.
                // If we fail to get the title of our own window, it's risky to skip it (might close overlay).
                // But if we return empty title, downstream logic might fail.
                // However, skipping it is definitely bad if it's just a momentary hang.
                // Let's try to get the length at least?
                // For now, if we timeout, we assume actualLen = 0.
                actualLen = 0; 
            } else {
                actualLen = static_cast<int>(result);
            }
        } else {
            // Other process: GetWindowTextW is safe/fast
            int titleLen = GetWindowTextLengthW(handle);
            if (titleLen == 0) return TRUE;

            if (titleLen >= static_cast<int>(context->titleBuffer.size())) {
                context->titleBuffer.resize(titleLen + 1);
            }

            actualLen = GetWindowTextW(handle, context->titleBuffer.data(), context->titleBuffer.size());
        }

        if (actualLen == 0) return TRUE;

        std::string title = toUtf8(std::wstring(context->titleBuffer.data(), actualLen));
        if (title.empty()) return TRUE;

        HANDLE pHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (!pHandle) return TRUE;

        wchar_t exePath[MAX_PATH]{};
        DWORD pathSize = MAX_PATH;
        QueryFullProcessImageNameW(pHandle, 0, exePath, &pathSize);
        CloseHandle(pHandle);

        std::string path = toUtf8(std::wstring(exePath));
        if (path.empty()) return TRUE;

        // Get bounds
        RECT rect{};
        if (!GetWindowRect(handle, &rect)) return TRUE;

        // Check window visibility
        bool isVisible = true;
        
        // Check cloaking
        if (context->pDwmGetWindowAttribute) {
            DWORD cloaked = 0;
            // DWMWA_CLOAKED = 14
            HRESULT hr = context->pDwmGetWindowAttribute(handle, 14, &cloaked, sizeof(cloaked));
            if (SUCCEEDED(hr) && cloaked != 0) {
                isVisible = false;
            }
        }
        
        // Filter out zero or very small windows
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width < 1 || height < 1) {
            isVisible = false;
        }

        // Get Z-order
        int zOrder = -1;
        auto zIt = context->zOrderMap->find(handle);
        if (zIt != context->zOrderMap->end()) {
            zOrder = zIt->second;
        }

        WindowData wd;
        wd.id = reinterpret_cast<int64_t>(handle);
        wd.title = title;
        wd.path = path;
        wd.processId = static_cast<int>(pid);
        wd.bounds.x = rect.left;
        wd.bounds.y = rect.top;
        wd.bounds.width = width;
        wd.bounds.height = height;
        wd.zOrder = zOrder;
        wd.isVisible = isVisible;

        context->result->push_back(wd);

        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return result;
}

Napi::Array ConvertToJs(Napi::Env env, const std::vector<WindowData>& data) {
    auto arr = Napi::Array::New(env, data.size());
    for (size_t i = 0; i < data.size(); i++) {
        const auto& d = data[i];
        
        // Create summary object
        auto obj = Napi::Object::New(env);
        obj.Set("id", Napi::Number::New(env, d.id));
        obj.Set("title", Napi::String::New(env, d.title));
        obj.Set("path", Napi::String::New(env, d.path));
        obj.Set("processId", Napi::Number::New(env, d.processId));
        
        auto bounds = Napi::Object::New(env);
        bounds.Set("x", Napi::Number::New(env, d.bounds.x));
        bounds.Set("y", Napi::Number::New(env, d.bounds.y));
        bounds.Set("width", Napi::Number::New(env, d.bounds.width));
        bounds.Set("height", Napi::Number::New(env, d.bounds.height));
        obj.Set("bounds", bounds);

        obj.Set("zOrder", Napi::Number::New(env, d.zOrder));
        obj.Set("isVisible", Napi::Boolean::New(env, d.isVisible));
        
        arr.Set(i, obj);
    }
    return arr;
}

Napi::Array getWindowsSummary (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };
    auto data = FetchWindowData();
    return ConvertToJs(env, data);
}

// ---------------------------------------------------------
// Monitoring Logic
// ---------------------------------------------------------

void ProcessUpdate() {
    // 1. Fetch data on background thread
    auto* data = new std::vector<WindowData>(FetchWindowData());

    // 2. Queue for JS thread
    auto callback = [](Napi::Env env, Napi::Function jsCallback, void* rawData) {
        auto* data = static_cast<std::vector<WindowData>*>(rawData);
        if (data) {
            Napi::Array arr = ConvertToJs(env, *data);
            jsCallback.Call({ arr });
            delete data;
        }
    };

    if (g_tsfn) {
        napi_status status = g_tsfn.NonBlockingCall(data, callback);
        if (status != napi_ok) {
            delete data;
        }
    } else {
        delete data;
    }
}

void CALLBACK TimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    if (id == g_timerId) {
        KillTimer(NULL, g_timerId);
        g_timerId = 0;
        g_lastUpdateTime = std::chrono::steady_clock::now();
        ProcessUpdate();
    }
}

void CheckAndUpdate() {
    auto now = std::chrono::steady_clock::now();
    
    // First update check
    if (g_lastUpdateTime.time_since_epoch().count() == 0) {
        g_lastUpdateTime = now;
        ProcessUpdate();
        return;
    }

    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastUpdateTime).count();

    if (diff < THROTTLE_MS) {
        if (g_timerId == 0) {
            // Schedule trailing update
            UINT delay = static_cast<UINT>(THROTTLE_MS - diff);
            if (delay < 10) delay = 10;
            // Use NULL HWND to associate with thread message queue
            g_timerId = SetTimer(NULL, 0, delay, TimerProc);
        }
        return;
    }

    // Cancel pending timer if we are updating now
    if (g_timerId != 0) {
        KillTimer(NULL, g_timerId);
        g_timerId = 0;
    }

    g_lastUpdateTime = now;
    ProcessUpdate();
}

Napi::Object getMonitorInfo (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    auto handle{ getValueFromCallbackData<HMONITOR> (info, 0) };

    MONITORINFO mInfo;
    mInfo.cbSize = sizeof (MONITORINFO);
    GetMonitorInfoA (handle, &mInfo);

    Napi::Object bounds{ Napi::Object::New (env) };

    bounds.Set ("x", mInfo.rcMonitor.left);
    bounds.Set ("y", mInfo.rcMonitor.top);
    bounds.Set ("width", mInfo.rcMonitor.right - mInfo.rcMonitor.left);
    bounds.Set ("height", mInfo.rcMonitor.bottom - mInfo.rcMonitor.top);

    Napi::Object workArea{ Napi::Object::New (env) };

    workArea.Set ("x", mInfo.rcWork.left);
    workArea.Set ("y", mInfo.rcWork.top);
    workArea.Set ("width", mInfo.rcWork.right - mInfo.rcWork.left);
    workArea.Set ("height", mInfo.rcWork.bottom - mInfo.rcWork.top);

    Napi::Object obj{ Napi::Object::New (env) };

    obj.Set ("bounds", bounds);
    obj.Set ("workArea", workArea);
    obj.Set ("isPrimary", (mInfo.dwFlags & MONITORINFOF_PRIMARY) != 0);

    return obj;
}

Napi::Boolean hideInstantly (const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env ();

    if (info.Length () < 1 || !info[0].IsNumber ()) {
        Napi::TypeError::New (env, "Number expected").ThrowAsJavaScriptException ();
    }

    uint32_t handleNumber = info[0].As<Napi::Number> ().Uint32Value ();
    HWND handle = reinterpret_cast<HWND> (handleNumber);

    // Get the current window styles
    LONG styles = GetWindowLong (handle, GWL_STYLE);
    LONG exStyles = GetWindowLong (handle, GWL_EXSTYLE);

    // Remove the WS_EX_LAYERED, WS_EX_TRANSPARENT and WS_OVERLAPPEDWINDOW styles
    SetWindowLong (handle, GWL_STYLE, styles & ~WS_OVERLAPPEDWINDOW);
    SetWindowLong (handle, GWL_EXSTYLE, exStyles & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT));

    BOOL result = ShowWindow (handle, SW_HIDE);

    return Napi::Boolean::New (env, result);
}


Napi::Boolean forceWindowPaint (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };

    if (info.Length () < 1 || !info[0].IsNumber ()) {
        Napi::TypeError::New (env, "Number expected").ThrowAsJavaScriptException ();
    }

    uint32_t handleNumber = info[0].As<Napi::Number> ().Uint32Value ();
    HWND handle = reinterpret_cast<HWND> (handleNumber);

    BOOL b{ RedrawWindow (handle, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW) };

    return Napi::Boolean::New (env, b);
}

Napi::Boolean setWindowAsPopup (const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env ();

    if (info.Length () < 1 || !info[0].IsNumber ()) {
        Napi::TypeError::New (env, "Number expected").ThrowAsJavaScriptException ();
    }

    uint32_t handleNumber = info[0].As<Napi::Number> ().Uint32Value ();
    HWND handle = reinterpret_cast<HWND> (handleNumber);

    // Get the current window style
    LONG lStyle = GetWindowLongPtr (handle, GWL_STYLE);

    // Modify the window style to a pop-up window
    lStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU);
    lStyle |= WS_POPUP;

    // Apply the new style
    SetWindowLongPtr (handle, GWL_STYLE, lStyle);

    // Redraw the window so the new style takes effect
    // SetWindowPos(handle, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    return Napi::Boolean::New (env, true);
}

// The enum flag for DwmSetWindowAttribute's second parameter, which tells the function what attribute to set.
enum DWMWINDOWATTRIBUTE { DWMWA_WINDOW_CORNER_PREFERENCE = 33 };

// The DWM_WINDOW_CORNER_PREFERENCE enum for DwmSetWindowAttribute's third parameter, which tells
// the function what value of the enum to set.
enum DWM_WINDOW_CORNER_PREFERENCE {
    DWMWCP_DEFAULT = 0,
    DWMWCP_DONOTROUND = 1,
    DWMWCP_ROUND = 2,
    DWMWCP_ROUNDSMALL = 3
};

// Import dwmapi.dll and define DwmSetWindowAttribute in C++ corresponding to the native function.
extern "C" __declspec (dllimport) HRESULT DwmSetWindowAttribute (HWND hwnd,
                                                                 DWMWINDOWATTRIBUTE dwAttribute,
                                                                 LPCVOID pvAttribute,
                                                                 DWORD cbAttribute);

Napi::Boolean setWindowAsPopupWithRoundedCorners (const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env ();

    if (info.Length () < 1 || !info[0].IsNumber ()) {
        Napi::TypeError::New (env, "Number expected").ThrowAsJavaScriptException ();
        return Napi::Boolean::New (env, false);
    }

    uint32_t handleNumber = info[0].As<Napi::Number> ().Uint32Value ();
    HWND handle = reinterpret_cast<HWND> (handleNumber);

    // Get the current window styles
    LONG lStyle = GetWindowLongPtr (handle, GWL_STYLE);
    LONG lExStyle = GetWindowLongPtr (handle, GWL_EXSTYLE);

    // Modify the window style to a pop-up window and apply WS_EX_COMPOSITED
    lStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU);
    lStyle |= WS_POPUP;
    lExStyle |= WS_EX_COMPOSITED;

    // Apply the new styles
    SetWindowLongPtr (handle, GWL_STYLE, lStyle);
    SetWindowLongPtr (handle, GWL_EXSTYLE, lExStyle);

    // Set the corner preference to be rounded
    DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute (handle, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof (preference));

    // Redraw the window so the new style takes effect
    RedrawWindow (handle, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);

    return Napi::Boolean::New (env, true);
}

// This isn't working. Still trying to figure out how to disable all the zoom/fade animations on windows caused by these global settings:
// "Animate windows when minimizing and maximizing"
// "Animate controls and elements inside windows"
Napi::Boolean showInstantly (const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env ();

    if (info.Length () < 1 || !info[0].IsNumber ()) {
        Napi::TypeError::New (env, "Number expected").ThrowAsJavaScriptException ();
        return Napi::Boolean::New (env, false);
    }

    uint32_t handleNumber = info[0].As<Napi::Number> ().Uint32Value ();
    HWND handle = reinterpret_cast<HWND> (handleNumber);

    // Disable the "Animate controls and elements inside windows" animation
    ANIMATIONINFO animationInfo = { sizeof (animationInfo) };
    animationInfo.iMinAnimate = 0;
    SystemParametersInfo (SPI_SETANIMATION, sizeof (animationInfo), &animationInfo, 0);

    // Show the window instantly without any animation
    SetWindowPos (handle, NULL, 0, 0, 0, 0,
                  SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW | SWP_FRAMECHANGED);

    // Bring the window to the foreground and activate it
    SetForegroundWindow (handle);
    SetActiveWindow (handle);

    // Restore the animation settings
    animationInfo.iMinAnimate = 1;
    SystemParametersInfo (SPI_SETANIMATION, sizeof (animationInfo), &animationInfo, 0);


    // Redraw window
    RedrawWindow (handle, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);

    return Napi::Boolean::New (env, true);
}

// Windows event hook callback
void CALLBACK WinEventProc(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD dwEventThread,
    DWORD dwmsEventTime
) {
    if (!g_monitoring || !g_tsfn) {
        return;
    }

    // Only process window-level events (not child controls)
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }

    CheckAndUpdate();
}

void MonitorThreadProc() {
    g_monitorThreadId = GetCurrentThreadId();

    // Force creation of message queue
    MSG msg;
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    // Set up the event hook for multiple events
    g_hooks.push_back(SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_LOCATIONCHANGE,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT
    ));

    g_hooks.push_back(SetWinEventHook(
        EVENT_OBJECT_REORDER,
        EVENT_OBJECT_REORDER,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT
    ));

    g_hooks.push_back(SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_CREATE,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT
    ));

    g_hooks.push_back(SetWinEventHook(
        EVENT_OBJECT_DESTROY,
        EVENT_OBJECT_DESTROY,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT
    ));

    g_hooks.push_back(SetWinEventHook(
        EVENT_SYSTEM_MOVESIZEEND,
        EVENT_SYSTEM_MOVESIZEEND,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT
    ));

    g_hooks.push_back(SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT
    ));

    g_hooks.push_back(SetWinEventHook(
        EVENT_SYSTEM_MINIMIZESTART,
        EVENT_SYSTEM_MINIMIZESTART,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT
    ));

    g_hooks.push_back(SetWinEventHook(
        EVENT_SYSTEM_MINIMIZEEND,
        EVENT_SYSTEM_MINIMIZEEND,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT
    ));

    // Message loop
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_QUIT) break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup hooks
    for (auto hook : g_hooks) {
        if (hook) UnhookWinEvent(hook);
    }
    g_hooks.clear();
}

Napi::Value startWindowsMonitoring(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (g_monitoring) {
        return env.Undefined();
    }

    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Function callback expected").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Function callback = info[0].As<Napi::Function>();

    // Create thread-safe function
    g_tsfn = Napi::ThreadSafeFunction::New(
        env,
        callback,
        "WindowsMonitoringCallback",
        0,
        1,
        [](Napi::Env) {
            // Cleanup when TSFN is finalized
            // We handle cleanup in stopWindowsMonitoring mainly
        }
    );

    g_monitoring = true;
    
    // Start the monitor thread
    g_monitorThread = new std::thread(MonitorThreadProc);

    return env.Undefined();
}

Napi::Value stopWindowsMonitoring(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!g_monitoring) {
        return env.Undefined();
    }

    // Signal thread to exit
    if (g_monitorThreadId != 0) {
        PostThreadMessage(g_monitorThreadId, WM_QUIT, 0, 0);
    }

    if (g_monitorThread) {
        if (g_monitorThread->joinable()) {
            g_monitorThread->join();
        }
        delete g_monitorThread;
        g_monitorThread = nullptr;
    }
    g_monitorThreadId = 0;

    if (g_timerId != 0) {
        // We can't use KillTimer here because it must be called from the same thread that called SetTimer.
        // But since the thread is joined and destroyed, the timer is gone.
        // Just reset the ID.
        g_timerId = 0;
    }

    if (g_tsfn) {
        g_tsfn.Release();
    }

    g_monitoring = false;

    return env.Undefined();
}

Napi::Object Init (Napi::Env env, Napi::Object exports) {
    exports.Set (Napi::String::New (env, "getActiveWindow"), Napi::Function::New (env, getActiveWindow));
    exports.Set (Napi::String::New (env, "getMonitorFromWindow"), Napi::Function::New (env, getMonitorFromWindow));
    exports.Set (Napi::String::New (env, "getMonitorScaleFactor"),
                 Napi::Function::New (env, getMonitorScaleFactor));
    exports.Set (Napi::String::New (env, "setWindowBounds"), Napi::Function::New (env, setWindowBounds));
    exports.Set (Napi::String::New (env, "showWindow"), Napi::Function::New (env, showWindow));
    exports.Set (Napi::String::New (env, "bringWindowToTop"), Napi::Function::New (env, bringWindowToTop));
    exports.Set (Napi::String::New (env, "redrawWindow"), Napi::Function::New (env, redrawWindow));
    exports.Set (Napi::String::New (env, "isWindow"), Napi::Function::New (env, isWindow));
    exports.Set (Napi::String::New (env, "isWindowVisible"), Napi::Function::New (env, isWindowVisible));
    exports.Set (Napi::String::New (env, "setWindowOpacity"), Napi::Function::New (env, setWindowOpacity));
    exports.Set (Napi::String::New (env, "toggleWindowTransparency"),
                 Napi::Function::New (env, toggleWindowTransparency));
    exports.Set (Napi::String::New (env, "setWindowParent"), Napi::Function::New (env, setWindowParent));
    exports.Set (Napi::String::New (env, "initWindow"), Napi::Function::New (env, initWindow));
    exports.Set (Napi::String::New (env, "getWindowBounds"), Napi::Function::New (env, getWindowBounds));
    exports.Set (Napi::String::New (env, "getWindowTitle"), Napi::Function::New (env, getWindowTitle));
    exports.Set (Napi::String::New (env, "getWindowName"), Napi::Function::New (env, getWindowName));
    exports.Set (Napi::String::New (env, "getWindowOwner"), Napi::Function::New (env, getWindowOwner));
    exports.Set (Napi::String::New (env, "getWindowOpacity"), Napi::Function::New (env, getWindowOpacity));
    exports.Set (Napi::String::New (env, "getMonitorInfo"), Napi::Function::New (env, getMonitorInfo));
    exports.Set (Napi::String::New (env, "getWindows"), Napi::Function::New (env, getWindows));
    exports.Set (Napi::String::New (env, "getMonitors"), Napi::Function::New (env, getMonitors));
    exports.Set (Napi::String::New (env, "createProcess"), Napi::Function::New (env, createProcess));
    exports.Set (Napi::String::New (env, "getProcessMainWindow"), Napi::Function::New (env, getProcessMainWindow));
    exports.Set (Napi::String::New (env, "forceWindowPaint"), Napi::Function::New (env, forceWindowPaint));
    exports.Set (Napi::String::New (env, "hideInstantly"), Napi::Function::New (env, hideInstantly));
    exports.Set (Napi::String::New (env, "setWindowAsPopup"), Napi::Function::New (env, setWindowAsPopup));
    exports.Set (Napi::String::New (env, "setWindowAsPopupWithRoundedCorners"),
                 Napi::Function::New (env, setWindowAsPopupWithRoundedCorners));
    exports.Set (Napi::String::New (env, "showInstantly"), Napi::Function::New (env, showInstantly));
    exports.Set (Napi::String::New (env, "getWindowZOrder"), Napi::Function::New (env, getWindowZOrder));
    exports.Set (Napi::String::New (env, "getWindowsSummary"), Napi::Function::New (env, getWindowsSummary));
    exports.Set (Napi::String::New (env, "startWindowsMonitoring"), Napi::Function::New (env, startWindowsMonitoring));
    exports.Set (Napi::String::New (env, "stopWindowsMonitoring"), Napi::Function::New (env, stopWindowsMonitoring));
    return exports;
}

NODE_API_MODULE (addon, Init)
