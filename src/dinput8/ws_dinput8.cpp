// WarmerSun - dinput8.dll proxy.
//
// Two jobs:
//   1. Forward the DirectInput8 API to the real %SystemRoot%\System32\dinput8.dll
//      so force-feedback wheels keep working.
//   2. Bootstrap the renderer early: load the WarmerSun dxgi proxy from this
//      module's folder before the game creates its device, and honour the
//      "WarmerSun.off" panic switch.
#include <windows.h>
#include <dinput.h>

#include <string>

#include "ws_common.h"

typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT(WINAPI* PFN_DllRegisterServer)();
typedef HRESULT(WINAPI* PFN_DllUnregisterServer)();
typedef HRESULT(WINAPI* PFN_DllCanUnloadNow)();
typedef HRESULT(WINAPI* PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef LPCDIDATAFORMAT(WINAPI* PFN_GetdfDIJoystick)();

namespace {

HMODULE g_self = nullptr;
HMODULE g_real = nullptr;

PFN_DirectInput8Create g_di8create = nullptr;
PFN_DllRegisterServer g_register = nullptr;
PFN_DllUnregisterServer g_unregister = nullptr;
PFN_DllCanUnloadNow g_canunload = nullptr;
PFN_DllGetClassObject g_getclass = nullptr;
PFN_GetdfDIJoystick g_joystick = nullptr;

bool ensure_real() {
    if (g_real) return true;
    wchar_t dir[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    std::wstring path(dir, n);
    path += L"\\dinput8.dll";

    // Make sure we do not resolve back to this module.
    g_real = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_real) {
        ws::log_write("dinput8: failed to load system dinput8 (%lu)", GetLastError());
        return false;
    }
    g_di8create = reinterpret_cast<PFN_DirectInput8Create>(GetProcAddress(g_real, "DirectInput8Create"));
    g_register = reinterpret_cast<PFN_DllRegisterServer>(GetProcAddress(g_real, "DllRegisterServer"));
    g_unregister = reinterpret_cast<PFN_DllUnregisterServer>(GetProcAddress(g_real, "DllUnregisterServer"));
    g_canunload = reinterpret_cast<PFN_DllCanUnloadNow>(GetProcAddress(g_real, "DllCanUnloadNow"));
    g_getclass = reinterpret_cast<PFN_DllGetClassObject>(GetProcAddress(g_real, "DllGetClassObject"));
    g_joystick = reinterpret_cast<PFN_GetdfDIJoystick>(GetProcAddress(g_real, "GetdfDIJoystick"));
    return g_di8create != nullptr;
}

bool panic_switch_present() {
    std::wstring off = ws::module_dir(g_self) + L"\\WarmerSun.off";
    DWORD attrs = GetFileAttributesW(off.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

DWORD WINAPI bootstrap(LPVOID) {
    ws::log_init(g_self);
    ws::log_write("WarmerSun dinput8 proxy loaded");

    if (panic_switch_present()) {
        ws::log_write("WarmerSun.off found - dxgi bootstrap skipped");
        return 0;
    }

    // Loading by full path guarantees the proxy in the game folder wins over
    // any other dxgi.dll that might already be resident.
    std::wstring dxgi = ws::module_dir(g_self) + L"\\dxgi.dll";
    HMODULE module = LoadLibraryW(dxgi.c_str());
    if (!module) {
        ws::log_write("dinput8: could not load %ls (%lu)", dxgi.c_str(), GetLastError());
        return 0;
    }
    ws::log_write("dinput8: dxgi proxy resident at %ls", dxgi.c_str());
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid,
                                             LPVOID* out, LPUNKNOWN outer) {
    if (!ensure_real() || !g_di8create) return E_FAIL;
    return g_di8create(hinst, version, riid, out, outer);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllRegisterServer() {
    if (!ensure_real() || !g_register) return E_FAIL;
    return g_register();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllUnregisterServer() {
    if (!ensure_real() || !g_unregister) return E_FAIL;
    return g_unregister();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() {
    if (!ensure_real() || !g_canunload) return S_FALSE;
    return g_canunload();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    if (!ensure_real() || !g_getclass) return CLASS_E_CLASSNOTAVAILABLE;
    return g_getclass(clsid, riid, out);
}

extern "C" __declspec(dllexport) LPCDIDATAFORMAT WINAPI GetdfDIJoystick() {
    if (!ensure_real() || !g_joystick) return nullptr;
    return g_joystick();
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

