#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <napi.h>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <fstream>
#include <iostream>
#include <atomic>

extern "C" AXError _AXUIElementGetWindow(AXUIElementRef, CGWindowID* out);

// CGWindowID to AXUIElementRef windows map
std::map<int, AXUIElementRef> windowsMap;

// Global variables for window monitoring
static std::thread g_monitoringThread;
static std::atomic<bool> g_monitoring(false);
static Napi::ThreadSafeFunction g_tsfn;
static std::vector<Napi::Object> g_lastWindowState;

bool _requestAccessibility(bool showDialog) {
  NSDictionary* opts = @{static_cast<id> (kAXTrustedCheckOptionPrompt): showDialog ? @YES : @NO};
  return AXIsProcessTrustedWithOptions(static_cast<CFDictionaryRef> (opts));
}

Napi::Boolean requestAccessibility(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};
  return Napi::Boolean::New(env, _requestAccessibility(true));
}

bool _requestScreenCapture() {
  bool hasScreenAccess = CGPreflightScreenCaptureAccess();
  if (!hasScreenAccess) {
      CGRequestScreenCaptureAccess();
  }
  return hasScreenAccess;
}

Napi::Boolean requestScreenCapture(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};
  return Napi::Boolean::New(env, _requestScreenCapture());
}

NSDictionary* getWindowInfo(int handle) {
  CGWindowListOption listOptions = kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements;
  CFArrayRef windowList = CGWindowListCopyWindowInfo(listOptions, kCGNullWindowID);

  for (NSDictionary *info in (NSArray *)windowList) {
    NSNumber *windowNumber = info[(id)kCGWindowNumber];

    if ([windowNumber intValue] == handle) {
        // Retain property list so it doesn't get release w. windowList
        CFRetain((CFPropertyListRef)info);
        CFRelease(windowList);
        return info;
    }
  }

  if (windowList) {
    CFRelease(windowList);
  }
  return NULL;
}

AXUIElementRef getAXWindow(int pid, int handle) {
  auto app = AXUIElementCreateApplication(pid);

  CFArrayRef windows;
  AXUIElementCopyAttributeValues(app, kAXWindowsAttribute, 0, 100, &windows);

  for (id child in  (NSArray *)windows) {
    AXUIElementRef window = (AXUIElementRef) child;

    CGWindowID windowId;
    _AXUIElementGetWindow(window, &windowId);

    if (windowId == static_cast<unsigned int>(handle)) {
      // Retain returned window so it doesn't get released with rest of list
      CFRetain(window);
      CFRelease(windows);
      return window;
    }
  }

  if (windows) {
    CFRelease(windows);
  }
  return NULL;
}

void cacheWindow(int handle, int pid) {
  if (_requestAccessibility(false)) {
    if (windowsMap.find(handle) == windowsMap.end()) {
      windowsMap[handle] = getAXWindow(pid, handle);
    }
  }
}

void cacheWindowByInfo(NSDictionary* info) {
  if (info) {
    NSNumber *ownerPid = info[(id)kCGWindowOwnerPID];
    NSNumber *windowNumber = info[(id)kCGWindowNumber];
    // Release dictionary info property since we're done with it
    CFRelease((CFPropertyListRef)info);
    cacheWindow([windowNumber intValue], [ownerPid intValue]);
  }
}

void findAndCacheWindow(int handle) {
  cacheWindowByInfo(getWindowInfo(handle));
}

AXUIElementRef getAXWindowById(int handle) {
  auto win = windowsMap[handle];

  if (!win) {
    findAndCacheWindow(handle);
    win = windowsMap[handle];
  }

  return win;
}

Napi::Array getWindows(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};

  CGWindowListOption listOptions = kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements;
  CFArrayRef windowList = CGWindowListCopyWindowInfo(listOptions, kCGNullWindowID);

  std::vector<Napi::Number> vec;

  for (NSDictionary *info in (NSArray *)windowList) {
    NSNumber *ownerPid = info[(id)kCGWindowOwnerPID];
    NSNumber *windowNumber = info[(id)kCGWindowNumber];

    auto app = [NSRunningApplication runningApplicationWithProcessIdentifier: [ownerPid intValue]];
    auto path = (app && app.bundleURL && app.bundleURL.path) ? [app.bundleURL.path UTF8String] : "";

     if (app && strcmp(path, "") != 0)  {
      vec.push_back(Napi::Number::New(env, [windowNumber intValue]));
    }
  }

  auto arr = Napi::Array::New(env, vec.size());

  for (size_t i = 0; i < vec.size(); i++) {
    arr[i] = vec[i];
  }

  if (windowList) {
    CFRelease(windowList);
  }
  
  return arr;
}

Napi::Number getActiveWindow(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};

  CGWindowListOption listOptions = kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements;
  CFArrayRef windowList = CGWindowListCopyWindowInfo(listOptions, kCGNullWindowID);

  for (NSDictionary *info in (NSArray *)windowList) {
    NSNumber *ownerPid = info[(id)kCGWindowOwnerPID];
    NSNumber *windowNumber = info[(id)kCGWindowNumber];

    auto app = [NSRunningApplication runningApplicationWithProcessIdentifier: [ownerPid intValue]];

    if (app) {
      if ([app isActive]) {
        CFRelease(windowList);
        return Napi::Number::New(env, [windowNumber intValue]);
      }
    } else {
      // std::cerr << "App is null for PID: " << [ownerPid intValue] << std::endl;
    }
  }

  if (windowList) {
    CFRelease(windowList);
  }
  return Napi::Number::New(env, 0);
}

Napi::Object initWindow(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};

  int handle = info[0].As<Napi::Number>().Int32Value();

  auto wInfo = getWindowInfo(handle);

  if (wInfo) {
    NSNumber *ownerPid = wInfo[(id)kCGWindowOwnerPID];
    NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier: [ownerPid intValue]];
    if (!app || !app.bundleURL || !app.bundleURL.path) {
      return Napi::Object::New(env);
    }

    auto obj = Napi::Object::New(env);
    obj.Set("processId", [ownerPid intValue]);
    obj.Set("path", [app.bundleURL.path UTF8String]);

    cacheWindow(handle, [ownerPid intValue]);

    return obj;
  }

  return Napi::Object::New(env);
}

Napi::String getWindowTitle(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};

  int handle = info[0].As<Napi::Number>().Int32Value();

  auto wInfo = getWindowInfo(handle);

  if (wInfo) {
    NSString *windowName = wInfo[(id)kCGWindowOwnerName];
    if (!windowName) {
      return Napi::String::New(env, "");
    }
    return Napi::String::New(env, [windowName UTF8String]);
  }

  return Napi::String::New(env, "");
}

Napi::String getWindowName(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};

  int handle = info[0].As<Napi::Number>().Int32Value();

  auto wInfo = getWindowInfo(handle);

  if (wInfo) {
    NSString *windowName = wInfo[(id)kCGWindowName];
    if (!windowName) {
      return Napi::String::New(env, "");
    }
    return Napi::String::New(env, [windowName UTF8String]);
  }

  return Napi::String::New(env, "");
}

Napi::Object getWindowBounds(const Napi::CallbackInfo &info) {
   Napi::Env env{info.Env()};

  int handle = info[0].As<Napi::Number>().Int32Value();

  auto wInfo = getWindowInfo(handle);

  if (wInfo) {
    CGRect bounds;
    CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)wInfo[(id)kCGWindowBounds], &bounds);

    auto obj = Napi::Object::New(env);
    obj.Set("x", bounds.origin.x);
    obj.Set("y", bounds.origin.y);
    obj.Set("width", bounds.size.width);
    obj.Set("height", bounds.size.height);

    return obj;
  }

  return Napi::Object::New(env);
}

Napi::Boolean setWindowBounds(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};

  auto handle = info[0].As<Napi::Number>().Int32Value();
  auto bounds = info[1].As<Napi::Object>();

  auto x = bounds.Get("x").As<Napi::Number>().DoubleValue();
  auto y = bounds.Get("y").As<Napi::Number>().DoubleValue();
  auto width = bounds.Get("width").As<Napi::Number>().DoubleValue();
  auto height = bounds.Get("height").As<Napi::Number>().DoubleValue();

  auto win = getAXWindowById(handle);

  if (win) {
    NSPoint point = NSMakePoint((CGFloat) x, (CGFloat) y);
    NSSize size = NSMakeSize((CGFloat) width, (CGFloat) height);

    CFTypeRef positionStorage = (CFTypeRef)(AXValueCreate((AXValueType)kAXValueCGPointType, (const void *)&point));
    AXUIElementSetAttributeValue(win, kAXPositionAttribute, positionStorage);

    CFTypeRef sizeStorage = (CFTypeRef)(AXValueCreate((AXValueType)kAXValueCGSizeType, (const void *)&size));
    AXUIElementSetAttributeValue(win, kAXSizeAttribute, sizeStorage);
  }

  return Napi::Boolean::New(env, true);
}

Napi::Boolean bringWindowToTop(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};

  auto handle = info[0].As<Napi::Number>().Int32Value();
  auto pid = info[1].As<Napi::Number>().Int32Value();

  auto app = AXUIElementCreateApplication(pid);
  auto win = getAXWindowById(handle);

  AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, kCFBooleanTrue);
  AXUIElementSetAttributeValue(win, kAXMainAttribute, kCFBooleanTrue);

  return Napi::Boolean::New(env, true);
}

Napi::Boolean setWindowMinimized(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};

  auto handle = info[0].As<Napi::Number>().Int32Value();
  auto toggle = info[1].As<Napi::Boolean>();

  auto win = getAXWindowById(handle);

  if (win) {
    AXUIElementSetAttributeValue(win, kAXMinimizedAttribute, toggle ? kCFBooleanTrue : kCFBooleanFalse);
  }

  return Napi::Boolean::New(env, true);
}

Napi::Boolean setWindowMaximized(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};
  auto handle = info[0].As<Napi::Number>().Int32Value();
  auto win = getAXWindowById(handle);

  if(win) {
    NSRect screenSizeRect = [[NSScreen mainScreen] frame];
    int screenWidth = screenSizeRect.size.width;
    int screenHeight = screenSizeRect.size.height;

    NSPoint point = NSMakePoint((CGFloat) 0, (CGFloat) 0);
    NSSize size = NSMakeSize((CGFloat) screenWidth, (CGFloat) screenHeight);

    CFTypeRef positionStorage = (CFTypeRef)(AXValueCreate((AXValueType)kAXValueCGPointType, (const void *)&point));
    AXUIElementSetAttributeValue(win, kAXPositionAttribute, positionStorage);

    CFTypeRef sizeStorage = (CFTypeRef)(AXValueCreate((AXValueType)kAXValueCGSizeType, (const void *)&size));
    AXUIElementSetAttributeValue(win, kAXSizeAttribute, sizeStorage);
  }

  return Napi::Boolean::New(env, true);
}


Napi::Number getWindowZOrder(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};

  int handle = info[0].As<Napi::Number>().Int32Value();

  // Build a z-ordered list of on-screen windows (front to back)
  CGWindowListOption listOptions = kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements | kCGWindowListOptionOnScreenAboveWindow;
  // Using kCGWindowListOptionOnScreenAboveWindow with the target handle will return windows ABOVE it
  CFArrayRef windowsAbove = CGWindowListCopyWindowInfo(listOptions, (CGWindowID)handle);

  // If API call fails, return -1 to indicate unknown
  if (!windowsAbove) {
    return Napi::Number::New(env, -1);
  }

  CFIndex count = CFArrayGetCount(windowsAbove);
  CFRelease(windowsAbove);

  // Number of windows above is the z-index (0 == frontmost)
  return Napi::Number::New(env, (int)count);
}

struct WindowFilter {
    std::string executableName;
    std::string titlePrefix;
};

// List of applications to ignore in the window summary
static const std::vector<WindowFilter> IGNORE_LIST = {
    { "xeester.app", "XEESTER:" },
    { "PokerTracker4.app", "MVS " },
    { "PokerTrackerHud4.app", "ptTableCover" }
};

bool shouldIgnoreWindow(const char* pathStr, const char* titleStr) {
    if (!pathStr || !titleStr) return false;
    
    std::string path(pathStr);
    std::string title(titleStr);
    
    // Extract filename from path
    size_t lastSep = path.find_last_of("/");
    std::string filename = (lastSep != std::string::npos) ? path.substr(lastSep + 1) : path;
    
    for (const auto& filter : IGNORE_LIST) {
        // Exact match for macOS app names usually preferred
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
  CGWindowListOption listOptions = kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements;
  CFArrayRef windowList = CGWindowListCopyWindowInfo(listOptions, kCGNullWindowID);

  if (!windowList) {
    return Napi::Array::New(env, 0);
  }

  // Build Z-order map (front to back, so index 0 = Z-order 0)
  std::map<int, int> zOrderMap;
  CFIndex totalWindows = CFArrayGetCount(windowList);
  for (CFIndex i = 0; i < totalWindows; i++) {
    NSDictionary *info = (NSDictionary *)CFArrayGetValueAtIndex(windowList, i);
    NSNumber *windowNumber = info[(id)kCGWindowNumber];
    if (windowNumber) {
      zOrderMap[[windowNumber intValue]] = (int)i;
    }
  }

  std::vector<Napi::Object> results;
  results.reserve(totalWindows);

  for (NSDictionary *info in (NSArray *)windowList) {
    NSNumber *ownerPid = info[(id)kCGWindowOwnerPID];
    NSNumber *windowNumber = info[(id)kCGWindowNumber];
    
    if (!ownerPid || !windowNumber) continue;

    int handle = [windowNumber intValue];
    int pid = [ownerPid intValue];

    // Get app info
    NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    if (!app || !app.bundleURL || !app.bundleURL.path) continue;

    const char* path = [app.bundleURL.path UTF8String];
    if (!path || strcmp(path, "") == 0) continue;

    // Get title (try kCGWindowName first, fallback to kCGWindowOwnerName)
    NSString *windowName = info[(id)kCGWindowName];
    if (!windowName || [windowName length] == 0) {
      windowName = info[(id)kCGWindowOwnerName];
    }
    if (!windowName || [windowName length] == 0) continue;

    const char* title = [windowName UTF8String];
    if (!title || strcmp(title, "") == 0) continue;

    // Apply filters
    if (shouldIgnoreWindow(path, title)) continue;

    // Get bounds
    CGRect bounds;
    if (!CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)info[(id)kCGWindowBounds], &bounds)) {
      continue;
    }

    // Check visibility based on window properties
    bool isVisible = true;
    
    // Check alpha (transparency) - invisible windows have alpha near 0
    NSNumber *alphaValue = info[(id)kCGWindowAlpha];
    if (alphaValue) {
      double alpha = [alphaValue doubleValue];
      if (alpha < 0.1) {
        isVisible = false;
      }
    }
    
    // Check window layer - menu bar items and other system UI are on special layers
    // Normal windows are on layer 0, menu bars and system UI are on higher layers
    NSNumber *layerValue = info[(id)kCGWindowLayer];
    if (layerValue) {
      int layer = [layerValue intValue];
      // Filter out windows on special layers (menu bar, status items, etc.)
      // Layer 0 = normal windows, Layer 25 = menu bar/status items, Layer 101 = screen saver
      if (layer != 0) {
        isVisible = false;
      }
    }
    
    // Filter out zero or very small windows (likely invisible UI elements)
    if (bounds.size.width < 1 || bounds.size.height < 1) {
      isVisible = false;
    }

    // Get Z-order from map
    int zOrder = -1;
    auto zIt = zOrderMap.find(handle);
    if (zIt != zOrderMap.end()) {
      zOrder = zIt->second;
    }

    // Create summary object
    Napi::Object summary = Napi::Object::New(env);
    summary.Set("id", Napi::Number::New(env, handle));
    summary.Set("title", Napi::String::New(env, title));
    summary.Set("path", Napi::String::New(env, path));
    summary.Set("processId", Napi::Number::New(env, pid));

    // Bounds
    Napi::Object boundsObj = Napi::Object::New(env);
    boundsObj.Set("x", Napi::Number::New(env, bounds.origin.x));
    boundsObj.Set("y", Napi::Number::New(env, bounds.origin.y));
    boundsObj.Set("width", Napi::Number::New(env, bounds.size.width));
    boundsObj.Set("height", Napi::Number::New(env, bounds.size.height));
    summary.Set("bounds", boundsObj);

    summary.Set("zOrder", Napi::Number::New(env, zOrder));
    summary.Set("isVisible", Napi::Boolean::New(env, isVisible));

    results.push_back(summary);
  }

  CFRelease(windowList);

  // Convert vector to Napi::Array
  auto arr = Napi::Array::New(env, results.size());
  for (size_t i = 0; i < results.size(); i++) {
    arr[i] = results[i];
  }

  return arr;
}

Napi::Array getWindowsSummary(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};
  return buildWindowsSummary(env);
}

// Helper to compare window states
bool windowStateChanged(const Napi::Array& current, const std::vector<Napi::Object>& previous) {
  if (current.Length() != previous.size()) {
    return true;
  }
  
  // Simple comparison - could be more sophisticated
  // For now, just check if the number of windows changed
  // In practice, the event system will fire on any change
  return true;
}

// Monitoring thread function
void monitoringThreadFunc() {
  while (g_monitoring) {
    if (g_tsfn) {
      auto callback = [](Napi::Env env, Napi::Function jsCallback) {
        Napi::Array summaries = buildWindowsSummary(env);
        jsCallback.Call({ summaries });
      };
      
      napi_status status = g_tsfn.NonBlockingCall(callback);
      if (status != napi_ok) {
        std::cerr << "Failed to call JS callback from monitoring thread" << std::endl;
      }
    }
    
    // Poll every 100ms
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

Napi::Value startWindowsMonitoring(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};
  
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
      g_monitoring = false;
    }
  );
  
  g_monitoring = true;
  
  // Start monitoring thread
  g_monitoringThread = std::thread(monitoringThreadFunc);
  
  return env.Undefined();
}

Napi::Value stopWindowsMonitoring(const Napi::CallbackInfo &info) {
  Napi::Env env{info.Env()};
  
  if (!g_monitoring) {
    return env.Undefined();
  }
  
  g_monitoring = false;
  
  if (g_monitoringThread.joinable()) {
    g_monitoringThread.join();
  }
  
  if (g_tsfn) {
    g_tsfn.Release();
  }
  
  return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set(Napi::String::New(env, "getWindows"),
                Napi::Function::New(env, getWindows));
    exports.Set(Napi::String::New(env, "getActiveWindow"),
                Napi::Function::New(env, getActiveWindow));
    exports.Set(Napi::String::New(env, "setWindowBounds"),
                Napi::Function::New(env, setWindowBounds));
    exports.Set(Napi::String::New(env, "getWindowBounds"),
                Napi::Function::New(env, getWindowBounds));
    exports.Set(Napi::String::New(env, "getWindowTitle"),
                Napi::Function::New(env, getWindowTitle));
    exports.Set(Napi::String::New(env, "getWindowName"),
                Napi::Function::New(env, getWindowName));
    exports.Set(Napi::String::New(env, "initWindow"),
                Napi::Function::New(env, initWindow));
    exports.Set(Napi::String::New(env, "bringWindowToTop"),
                Napi::Function::New(env, bringWindowToTop));
    exports.Set(Napi::String::New(env, "setWindowMinimized"),
                Napi::Function::New(env, setWindowMinimized));
    exports.Set(Napi::String::New(env, "setWindowMaximized"),
                Napi::Function::New(env, setWindowMaximized));
    exports.Set(Napi::String::New(env, "requestAccessibility"),
                Napi::Function::New(env, requestAccessibility));
    exports.Set(Napi::String::New(env, "getWindowZOrder"),
                Napi::Function::New(env, getWindowZOrder));
    exports.Set(Napi::String::New(env, "getWindowsSummary"),
                Napi::Function::New(env, getWindowsSummary));
    exports.Set(Napi::String::New(env, "startWindowsMonitoring"),
                Napi::Function::New(env, startWindowsMonitoring));
    exports.Set(Napi::String::New(env, "stopWindowsMonitoring"),
                Napi::Function::New(env, stopWindowsMonitoring));

    return exports;
}

NODE_API_MODULE(addon, Init)