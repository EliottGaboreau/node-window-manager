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

typedef int (__stdcall* lp_GetScaleFactorForMonitor) (HMONITOR, DEVICE_SCALE_FACTOR*);

// Global variables for window monitoring
static std::vector<HWINEVENTHOOK> g_hooks;
static Napi::ThreadSafeFunction g_tsfn;
static std::atomic<bool> g_monitoring(false);
static std::thread* g_monitorThread = nullptr;
static DWORD g_monitorThreadId = 0;

// Throttling state variables
static std::atomic<DWORD> g_lastProcessedTime(0);
static std::atomic<bool> g_pendingTrailingUpdate(false);
static UINT_PTR g_throttleTimerId = 0;
static const DWORD THROTTLE_MS = 64; // ~30fps throttle interval

struct Process {
    int pid;
    std::string path;
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

struct WindowFilter {
    std::string executableName;
    std::string titlePrefix;
};

// List of applications to ignore in the window summary
static const std::vector<WindowFilter> IGNORE_LIST = {
    { "xeester.exe", "XEESTER:" },
    { "PokerTracker4.exe", "MVS " },
    { "PokerTrackerHud4.exe", "ptTableCover" },
    { "HM3Hud.exe", "MVS " },
    { "HM3HudProcess.exe", "ptTableCover" }
};

bool shouldIgnoreWindow(const std::string& path, const std::string& title) {
    if (path.empty()) return false;

    // Extract filename from path
    size_t lastSep = path.find_last_of("\\/");
    std::string filename = (lastSep != std::string::npos) ? path.substr(lastSep + 1) : path;

    for (const auto& filter : IGNORE_LIST) {
        if (filename == filter.executableName) {
            // Check title prefix
            if (title.length() >= filter.titlePrefix.length() &&
                title.compare(0, filter.titlePrefix.length(), filter.titlePrefix) == 0) {
                return true;
            }
        }
    }
    return false;
}

// Helper function to build windows summary
Napi::Array buildWindowsSummary(Napi::Env env) {
    _windows.clear ();
    EnumWindows (&EnumWindowsProc, NULL);

    // Build Z-order map once
    std::unordered_map<HWND, int> zOrderMap;
    int currentZ = 0;
    HWND walker = GetTopWindow (NULL);
    while (walker) {
        zOrderMap[walker] = currentZ++;
        walker = GetWindow (walker, GW_HWNDNEXT);
    }

    // Load dwmapi.dll once for DWM cloaking checks
    HMODULE hDwmapi = LoadLibraryA ("dwmapi.dll");
    typedef HRESULT (WINAPI *DwmGetWindowAttributeProc)(HWND, DWORD, PVOID, DWORD);
    DwmGetWindowAttributeProc pDwmGetWindowAttribute = nullptr;
    if (hDwmapi) {
        pDwmGetWindowAttribute = (DwmGetWindowAttributeProc)GetProcAddress (hDwmapi, "DwmGetWindowAttribute");
    }

    auto arr = Napi::Array::New (env);
    int resultIndex = 0;

    // Reusable buffer for window titles (most titles < 256 chars)
    std::vector<WCHAR> titleBuffer (256);

    for (auto _win : _windows) {
        HWND handle = reinterpret_cast<HWND> (_win);

        // Filter: only visible windows
        if (!IsWindowVisible (handle))
            continue;

        // Get title length first
        int titleLen = GetWindowTextLengthW (handle);
        if (titleLen == 0)
            continue;

        // Resize buffer if needed
        if (titleLen >= static_cast<int>(titleBuffer.size ())) {
            titleBuffer.resize (titleLen + 1);
        }

        // Get title into reusable buffer
        int actualLen = GetWindowTextW (handle, titleBuffer.data (), titleBuffer.size ());
        if (actualLen == 0)
            continue;

        std::string title = toUtf8 (std::wstring (titleBuffer.data (), actualLen));
        if (title.empty ())
            continue;

        // Get process info (optimized to batch process handle operations)
        DWORD pid = 0;
        GetWindowThreadProcessId (handle, &pid);
        if (pid == 0)
            continue;

        HANDLE pHandle = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (!pHandle)
            continue;

        DWORD pathSize = MAX_PATH;
        wchar_t exePath[MAX_PATH]{};
        QueryFullProcessImageNameW (pHandle, 0, exePath, &pathSize);
        CloseHandle (pHandle);

        std::string path = toUtf8 (std::wstring (exePath));
        if (path.empty ())
            continue;

        // Apply filters
        if (shouldIgnoreWindow(path, title))
            continue;

        // Get bounds
        RECT rect{};
        if (!GetWindowRect (handle, &rect))
            continue;

        // Check window visibility (more comprehensive than just IsWindowVisible)
        bool isVisible = true;
        
        // Check if window is cloaked by DWM (Windows 8+)
        // Cloaked windows are technically "visible" but hidden by the system
        if (pDwmGetWindowAttribute) {
            DWORD cloaked = 0;
            // DWMWA_CLOAKED = 14
            HRESULT hr = pDwmGetWindowAttribute (handle, 14, &cloaked, sizeof(cloaked));
            if (SUCCEEDED(hr) && cloaked != 0) {
                isVisible = false;
            }
        }
        
        // Calculate physical dimensions for filtering
        int physWidth = rect.right - rect.left;
        int physHeight = rect.bottom - rect.top;
        
        // Filter out zero or very small windows (likely invisible UI elements)
        if (physWidth < 1 || physHeight < 1) {
            isVisible = false;
        }

        // Create summary object
        Napi::Object summary = Napi::Object::New (env);
        summary.Set ("id", Napi::Number::New (env, _win));
        summary.Set ("title", Napi::String::New (env, title));
        summary.Set ("path", Napi::String::New (env, path));
        summary.Set ("processId", Napi::Number::New (env, static_cast<int> (pid)));

        // Bounds: Return raw physical coordinates - Electron handles DIP conversion
        Napi::Object bounds = Napi::Object::New (env);
        bounds.Set ("x", Napi::Number::New (env, rect.left));
        bounds.Set ("y", Napi::Number::New (env, rect.top));
        bounds.Set ("width", Napi::Number::New (env, rect.right - rect.left));
        bounds.Set ("height", Napi::Number::New (env, rect.bottom - rect.top));
        summary.Set ("bounds", bounds);

        // Z-order
        auto zIt = zOrderMap.find (handle);
        int zOrder = (zIt != zOrderMap.end ()) ? zIt->second : -1;
        summary.Set ("zOrder", Napi::Number::New (env, zOrder));
        
        // Visibility
        summary.Set ("isVisible", Napi::Boolean::New (env, isVisible));

        arr.Set (resultIndex++, summary);
    }

    // Cleanup
    if (hDwmapi) {
        FreeLibrary (hDwmapi);
    }

    return arr;
}

Napi::Array getWindowsSummary (const Napi::CallbackInfo& info) {
    Napi::Env env{ info.Env () };
    return buildWindowsSummary(env);
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

// Helper function to invoke JS callback with window summary
static void invokeWindowsSummaryCallback() {
    if (!g_monitoring || !g_tsfn) {
        return;
    }

    auto callback = [](Napi::Env env, Napi::Function jsCallback) {
        Napi::Array summaries = buildWindowsSummary(env);
        jsCallback.Call({ summaries });
    };

    g_tsfn.NonBlockingCall(callback);
}

// Forward declaration for timer callback
void CALLBACK ThrottleTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);

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

    // Throttle: check if enough time has passed since last processing
    DWORD now = GetTickCount();
    DWORD lastTime = g_lastProcessedTime.load();
    DWORD elapsed = now - lastTime;

    if (elapsed >= THROTTLE_MS) {
        // Enough time passed - process immediately
        g_lastProcessedTime.store(now);
        g_pendingTrailingUpdate.store(false);
        
        // Cancel any pending timer
        if (g_throttleTimerId) {
            KillTimer(NULL, g_throttleTimerId);
            g_throttleTimerId = 0;
        }
        
        invokeWindowsSummaryCallback();
    } else if (!g_pendingTrailingUpdate.exchange(true)) {
        // Schedule trailing-edge timer to capture final state
        UINT delay = THROTTLE_MS - elapsed;
        g_throttleTimerId = SetTimer(NULL, 0, delay, ThrottleTimerProc);
    }
    // else: timer already pending, do nothing - it will capture the final state
}

// Timer callback for trailing-edge throttle updates
void CALLBACK ThrottleTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    KillTimer(NULL, idEvent);
    g_throttleTimerId = 0;
    g_lastProcessedTime.store(GetTickCount());
    g_pendingTrailingUpdate.store(false);
    
    invokeWindowsSummaryCallback();
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

    // Cancel any pending throttle timer
    if (g_throttleTimerId) {
        KillTimer(NULL, g_throttleTimerId);
        g_throttleTimerId = 0;
    }
    g_pendingTrailingUpdate.store(false);
    g_lastProcessedTime.store(0);

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