// WarmerSun - dxgi.dll proxy and swap-chain hook.
//
// The game resolves dxgi.dll from its own folder first, so this module becomes
// the DXGI implementation seen by the process. Every export forwards to the
// real %SystemRoot%\System32\dxgi.dll and the factory/swap-chain vtables are
// patched so presents run the WarmerSun grade.
#include <windows.h>
#include <dxgi1_2.h>

#include <string>

#include "ws_common.h"
#include "ws_renderer.h"

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* PFN_DXGIDeclareAdapterRemovalSupport)();
typedef HRESULT(WINAPI* PFN_DXGIGetDebugInterface1)(UINT, REFIID, void**);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(void*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(
    void*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
    IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForCoreWindow)(void*, IUnknown*, IUnknown*,
                                                                    const DXGI_SWAP_CHAIN_DESC1*,
                                                                    IDXGIOutput*,
                                                                    IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForComposition)(void*, IUnknown*,
                                                                     const DXGI_SWAP_CHAIN_DESC1*,
                                                                     IDXGIOutput*,
                                                                     IDXGISwapChain1**);

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain*, UINT, UINT,
                                                const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT,
                                                     DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT,
                                                      DXGI_FORMAT, UINT, const UINT*,
                                                      IUnknown* const*);

namespace {

HMODULE g_real = nullptr;
HMODULE g_self = nullptr;
bool    g_patchedFactory = false;
bool    g_patchedSwapchain = false;

PFN_CreateDXGIFactory        g_origCreateFactory = nullptr;
PFN_CreateDXGIFactory1       g_origCreateFactory1 = nullptr;
PFN_CreateDXGIFactory2       g_origCreateFactory2 = nullptr;
PFN_DXGIDeclareAdapterRemovalSupport g_origDeclareRemoval = nullptr;
PFN_DXGIGetDebugInterface1   g_origGetDebugInterface1 = nullptr;

PFN_CreateSwapChain               g_origCreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd        g_origCreateSwapChainHwnd = nullptr;
PFN_CreateSwapChainForCoreWindow  g_origCreateSwapChainCore = nullptr;
PFN_CreateSwapChainForComposition g_origCreateSwapChainComp = nullptr;

PFN_Present        g_origPresent = nullptr;
PFN_Present1       g_origPresent1 = nullptr;
PFN_ResizeBuffers  g_origResizeBuffers = nullptr;
PFN_ResizeBuffers1 g_origResizeBuffers1 = nullptr;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

std::wstring system_dxgi_path() {
    wchar_t dir[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return std::wstring();
    std::wstring path(dir, n);
    path += L"\\dxgi.dll";
    return path;
}

bool ensure_real() {
    if (g_real) return true;
    std::wstring path = system_dxgi_path();
    if (path.empty()) return false;
    g_real = LoadLibraryW(path.c_str());
    if (!g_real) {
        ws::log_write("failed to load system dxgi (%lu)", GetLastError());
        return false;
    }
    g_origCreateFactory = reinterpret_cast<PFN_CreateDXGIFactory>(
        GetProcAddress(g_real, "CreateDXGIFactory"));
    g_origCreateFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(
        GetProcAddress(g_real, "CreateDXGIFactory1"));
    g_origCreateFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(
        GetProcAddress(g_real, "CreateDXGIFactory2"));
    g_origDeclareRemoval = reinterpret_cast<PFN_DXGIDeclareAdapterRemovalSupport>(
        GetProcAddress(g_real, "DXGIDeclareAdapterRemovalSupport"));
    g_origGetDebugInterface1 = reinterpret_cast<PFN_DXGIGetDebugInterface1>(
        GetProcAddress(g_real, "DXGIGetDebugInterface1"));
    return g_origCreateFactory != nullptr;
}

bool belongs_to_real_dxgi(void* fn) {
    if (!fn || !g_real) return false;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(fn), &owner)) {
        return false;
    }
    return owner == g_real;
}

// ---------------------------------------------------------------------------
// swap chain hooks
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync, UINT flags) {
    ws::g_renderer.present(self, flags);
    return g_origPresent(self, sync, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain* self, UINT sync, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    if (ws::g_renderer.present(self, flags)) {
        // The grade touched the whole back buffer: discard the game's dirty
        // rectangles so the complete frame is presented.
        DXGI_PRESENT_PARAMETERS clean = {};
        return g_origPresent1(self, sync, flags, &clean);
    }
    return g_origPresent1(self, sync, flags, params);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT count, UINT width,
                                             UINT height, DXGI_FORMAT format, UINT flags) {
    ws::g_renderer.notify_resize();
    return g_origResizeBuffers(self, count, width, height, format, flags);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers1(IDXGISwapChain* self, UINT count, UINT width,
                                              UINT height, DXGI_FORMAT format, UINT flags,
                                              const UINT* nodeMask, IUnknown* const* presentQueue) {
    ws::g_renderer.notify_resize();
    return g_origResizeBuffers1(self, count, width, height, format, flags, nodeMask, presentQueue);
}

void patch_swapchain(IDXGISwapChain* sc) {
    if (!sc) return;
    void** vt = *reinterpret_cast<void***>(sc);
    if (!vt) return;

    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(vt[8]), &owner) ||
        owner != g_real) {
        ws::log_write("swap chain vtable not recognised; present hook skipped");
        return;
    }
    if (g_patchedSwapchain) return;

    DWORD old = 0;
    const SIZE_T span = 40 * sizeof(void*);
    if (!VirtualProtect(vt, span, PAGE_READWRITE, &old)) {
        ws::log_write("VirtualProtect(swapchain) failed: %lu", GetLastError());
        return;
    }

    g_origPresent = reinterpret_cast<PFN_Present>(vt[8]);
    vt[8] = reinterpret_cast<void*>(&HookPresent);

    if (belongs_to_real_dxgi(vt[11])) {
        g_origResizeBuffers = reinterpret_cast<PFN_ResizeBuffers>(vt[11]);
        vt[11] = reinterpret_cast<void*>(&HookResizeBuffers);
    }
    if (belongs_to_real_dxgi(vt[20])) {
        g_origPresent1 = reinterpret_cast<PFN_Present1>(vt[20]);
        vt[20] = reinterpret_cast<void*>(&HookPresent1);
    }
    if (belongs_to_real_dxgi(vt[37])) {
        g_origResizeBuffers1 = reinterpret_cast<PFN_ResizeBuffers1>(vt[37]);
        vt[37] = reinterpret_cast<void*>(&HookResizeBuffers1);
    }

    VirtualProtect(vt, span, old, &old);
    g_patchedSwapchain = true;
    ws::log_write("swap chain hooked (Present%s%s)", g_origPresent1 ? "/Present1" : "",
                  g_origResizeBuffers1 ? "/ResizeBuffers1" : "");
}

// ---------------------------------------------------------------------------
// factory hooks
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(void* self, IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) {
    HRESULT hr = g_origCreateSwapChain(self, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) patch_swapchain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(
    void* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict, IDXGISwapChain1** out) {
    HRESULT hr = g_origCreateSwapChainHwnd(self, device, hwnd, desc, fs, restrict, out);
    if (SUCCEEDED(hr) && out && *out) patch_swapchain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForCoreWindow(void* self, IUnknown* device,
                                                           IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* restrict,
                                                           IDXGISwapChain1** out) {
    HRESULT hr = g_origCreateSwapChainCore(self, device, window, desc, restrict, out);
    if (SUCCEEDED(hr) && out && *out) patch_swapchain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForComposition(void* self, IUnknown* device,
                                                            const DXGI_SWAP_CHAIN_DESC1* desc,
                                                            IDXGIOutput* restrict,
                                                            IDXGISwapChain1** out) {
    HRESULT hr = g_origCreateSwapChainComp(self, device, desc, restrict, out);
    if (SUCCEEDED(hr) && out && *out) patch_swapchain(*out);
    return hr;
}

void patch_factory(IUnknown* factory) {
    if (!factory || g_patchedFactory) return;
    void** vt = *reinterpret_cast<void***>(factory);
    if (!vt) return;

    if (!belongs_to_real_dxgi(vt[10])) {
        ws::log_write("factory vtable not recognised; hook skipped");
        return;
    }

    DWORD old = 0;
    const SIZE_T span = 32 * sizeof(void*);
    if (!VirtualProtect(vt, span, PAGE_READWRITE, &old)) {
        ws::log_write("VirtualProtect(factory) failed: %lu", GetLastError());
        return;
    }

    g_origCreateSwapChain = reinterpret_cast<PFN_CreateSwapChain>(vt[10]);
    vt[10] = reinterpret_cast<void*>(&HookCreateSwapChain);

    if (belongs_to_real_dxgi(vt[15])) {
        g_origCreateSwapChainHwnd = reinterpret_cast<PFN_CreateSwapChainForHwnd>(vt[15]);
        vt[15] = reinterpret_cast<void*>(&HookCreateSwapChainForHwnd);
    }
    if (belongs_to_real_dxgi(vt[16])) {
        g_origCreateSwapChainCore = reinterpret_cast<PFN_CreateSwapChainForCoreWindow>(vt[16]);
        vt[16] = reinterpret_cast<void*>(&HookCreateSwapChainForCoreWindow);
    }
    if (belongs_to_real_dxgi(vt[17])) {
        g_origCreateSwapChainComp = reinterpret_cast<PFN_CreateSwapChainForComposition>(vt[17]);
        vt[17] = reinterpret_cast<void*>(&HookCreateSwapChainForComposition);
    }

    VirtualProtect(vt, span, old, &old);
    g_patchedFactory = true;
    ws::log_write("factory hooked (CreateSwapChain%s%s)", g_origCreateSwapChainHwnd ? "/ForHwnd" : "",
                  g_origCreateSwapChainComp ? "/ForComposition" : "");
}

// ---------------------------------------------------------------------------
// IAT fallback: catch D3D11CreateDeviceAndSwapChain when d3d11 builds the
// factory internally before our export is reached.
// ---------------------------------------------------------------------------

using PFN_D3D11CreateDeviceAndSwapChain = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);

PFN_D3D11CreateDeviceAndSwapChain g_origD3D11CreateSwap = nullptr;

HRESULT WINAPI HookD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context) {
    HRESULT hr = g_origD3D11CreateSwap(adapter, driverType, software, flags, levels, levelCount,
                                       sdkVersion, desc, swapChain, device, featureLevel, context);
    if (SUCCEEDED(hr) && swapChain && *swapChain) patch_swapchain(*swapChain);
    return hr;
}

void patch_iat_once() {
    if (g_origD3D11CreateSwap) return;

    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return;
    auto* base = reinterpret_cast<unsigned char*>(exe);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return;

    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imports->Name; ++imports) {
        const char* dll = reinterpret_cast<const char*>(base + imports->Name);
        if (_stricmp(dll, "d3d11.dll") != 0) continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->FirstThunk);
        auto* names = imports->OriginalFirstThunk
                          ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->OriginalFirstThunk)
                          : nullptr;
        for (; thunk->u1.Function; ++thunk) {
            if (names && !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                auto* hint = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(hint->Name),
                           "D3D11CreateDeviceAndSwapChain") == 0) {
                    auto** slot = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain*>(&thunk->u1.Function);
                    DWORD old = 0;
                    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                        g_origD3D11CreateSwap = *slot;
                        *slot = &HookD3D11CreateDeviceAndSwapChain;
                        VirtualProtect(slot, sizeof(void*), old, &old);
                        ws::log_write("IAT hooked: D3D11CreateDeviceAndSwapChain");
                        return;
                    }
                }
            }
            if (names) ++names;
        }
    }
}

// ---------------------------------------------------------------------------
// startup
// ---------------------------------------------------------------------------

DWORD WINAPI init_thread(LPVOID) {
    ws::log_init(g_self);

    ws::Config cfg;
    std::wstring path = ws::find_config(g_self);
    ws::load_config(path, cfg, 0);
    cfg.enabled = cfg.enabled && !cfg.safeMode;
    ws::g_renderer.set_config(cfg);

    ws::log_write("WarmerSun dxgi proxy loaded (enabled=%d preset=%c)", cfg.enabled ? 1 : 0,
                  cfg.preset);
    if (!cfg.sourcePath.empty()) ws::log_write("config: %ls", cfg.sourcePath.c_str());

    // d3d11.dll may not be loaded yet at process start; keep trying briefly.
    for (int i = 0; i < 100; ++i) {
        patch_iat_once();
        if (g_origD3D11CreateSwap) break;
        Sleep(50);
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// exported proxy functions
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory) {
    if (!ensure_real()) return E_FAIL;
    HRESULT hr = g_origCreateFactory(riid, factory);
    if (SUCCEEDED(hr) && factory && *factory) patch_factory(reinterpret_cast<IUnknown*>(*factory));
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
    if (!ensure_real()) return E_FAIL;
    HRESULT hr = g_origCreateFactory1 ? g_origCreateFactory1(riid, factory)
                                      : g_origCreateFactory(riid, factory);
    if (SUCCEEDED(hr) && factory && *factory) patch_factory(reinterpret_cast<IUnknown*>(*factory));
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    if (!ensure_real()) return E_FAIL;
    HRESULT hr = g_origCreateFactory2 ? g_origCreateFactory2(flags, riid, factory)
                                      : g_origCreateFactory1(riid, factory);
    if (SUCCEEDED(hr) && factory && *factory) patch_factory(reinterpret_cast<IUnknown*>(*factory));
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    if (!ensure_real() || !g_origDeclareRemoval) return E_NOTIMPL;
    return g_origDeclareRemoval();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug) {
    if (!ensure_real() || !g_origGetDebugInterface1) return E_NOTIMPL;
    return g_origGetDebugInterface1(flags, riid, debug);
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        ws::g_renderer.shutdown();
        ws::log_shutdown();
    }
    return TRUE;
}


