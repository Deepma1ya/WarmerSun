// WarmerSun - dxgi.dll proxy and swap-chain hook.
//
// The game resolves dxgi.dll from its own folder first, so this module becomes
// the DXGI implementation seen by the process. Every export forwards to the
// real %SystemRoot%\System32\dxgi.dll and the factory/swap-chain vtables are
// patched so presents run the WarmerSun grade.
#include <windows.h>
#include <dxgi1_2.h>

#include <string>
#include <cstring>

#include "ws_common.h"
#include "ws_renderer.h"
#include "ws_taa.h"
#include "ws_taa_hashes.h"
#include "ws_tonemap_hashes.h"
#include "ws_ui_hashes.h"
#include "MinHook.h"
#include <d3dcompiler.h>

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

// Minimal append-only trace that does not depend on log_init or formatting.
void trace_raw(const char* msg) {
    std::wstring path = ws::module_dir(g_self) + L"\\WarmerSun.trace";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
    WriteFile(h, "\r\n", 2, &written, nullptr);
    CloseHandle(h);
}

std::wstring system_dxgi_path() {
    wchar_t dir[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return std::wstring();
    std::wstring path(dir, n);
    path += L"\\dxgi.dll";
    return path;
}

bool ensure_real() {
    trace_raw("ensure: begin");
    if (g_real) return true;

    // Loading "%SystemRoot%\System32\dxgi.dll" by name resolves to *this*
    // module when the proxy is already loaded as "dxgi.dll" (same base name),
    // which recurses forever. Copy the system DLL to a unique name first.
    std::wstring sys = system_dxgi_path();
    if (sys.empty()) return false;

    std::wstring dir = ws::module_dir(g_self);
    std::wstring copy = dir + L"\\WarmerSun.real.dxgi.dll";
    if (GetFileAttributesW(copy.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileW(sys.c_str(), copy.c_str(), FALSE)) {
            ws::log_write("could not copy system dxgi to %ls (%lu)", copy.c_str(), GetLastError());
            copy.clear();
        }
    }

    trace_raw("ensure: loading copy");
    if (!copy.empty()) g_real = LoadLibraryW(copy.c_str());
    if (!g_real) g_real = LoadLibraryW(sys.c_str());
    if (!g_real) {
        ws::log_write("failed to load system dxgi (%lu)", GetLastError());
        return false;
    }
    trace_raw("ensure: loaded");
    if (g_real == g_self) {
        ws::log_write("real dxgi resolved to the proxy itself; hooking disabled");
        g_real = nullptr;
        return false;
    }
    trace_raw("ensure: exports");
    ws::log_write("real dxgi: %ls", (copy.empty() ? sys : copy).c_str());

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
// D3D11 shader hooks. The grade runs right after the engine's tonemap draw,
// before the UI is composited, so menus and HUD keep their original colours.
// ---------------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePixelShader)(ID3D11Device*, const void*,
                                                          SIZE_T, ID3D11ClassLinkage*,
                                                          ID3D11PixelShader**);
typedef void(STDMETHODCALLTYPE* PFN_PSSetShader)(ID3D11DeviceContext*, ID3D11PixelShader*,
                                                 ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);

PFN_CreatePixelShader g_origCreatePS = nullptr;
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDeferredContext)(ID3D11Device*, UINT,
                                                              ID3D11DeviceContext**);
PFN_CreateDeferredContext g_origCreateDeferred = nullptr;
bool g_loggedDeferred = false;
PFN_PSSetShader       g_origPSSetShader = nullptr;
PFN_Draw              g_origDraw = nullptr;
PFN_DrawIndexed       g_origDrawIndexed = nullptr;
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandList)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
PFN_ExecuteCommandList g_origExecuteCommandList = nullptr;
bool g_loggedExecute = false;

PFN_DrawInstanced        g_origDrawInstanced = nullptr;
PFN_DrawIndexedInstanced g_origDrawIndexedInstanced = nullptr;
ID3D11PixelShader*    g_tonemapShaders[256] = {};
int                   g_tonemapCount = 0;
ID3D11PixelShader*    g_currentPS = nullptr;
bool                  g_shaderHooks = false;
bool                  g_inGrade = false;
bool                  g_loggedPreUi = false;
bool                  g_loggedStateTrigger = false;
ID3D11PixelShader*    g_uiShaders[64] = {};
int                   g_uiCount = 0;
bool                  g_loggedUi = false;
bool                  g_loggedTaa = false;
ID3D11PixelShader*    g_taaSavedPS = nullptr;
ID3D11Buffer*         g_taaSavedCB = nullptr;
bool                  g_loggedCapture = false;
ID3D11Device*         g_hookDevice = nullptr;
ID3D11DeviceContext*  g_hookContext = nullptr;
bool                  g_loggedRefreshDevice = false;
bool                  g_loggedRefreshContext = false;

bool is_ui_shader(ID3D11PixelShader* ps) {
    if (!ps) return false;
    for (int i = 0; i < g_uiCount; ++i) {
        if (g_uiShaders[i] == ps) return true;
    }
    return false;
}

bool is_tonemap_shader(ID3D11PixelShader* ps) {
    if (!ps) return false;
    for (int i = 0; i < g_tonemapCount; ++i) {
        if (g_tonemapShaders[i] == ps) return true;
    }
    return false;
}

typedef HRESULT(WINAPI* PFN_D3DReflect)(LPCVOID, SIZE_T, REFIID, void**);
PFN_D3DReflect g_d3dReflect = nullptr;

// 8d536ca1-0cca-4956-a837-786963755584
static const GUID kWsShaderReflectionIid = {
    0x8d536ca1, 0x0cca, 0x4956, {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}};

bool looks_like_tonemap(const void* code, size_t size) {
    if (!g_d3dReflect || !code || !size) return false;
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(g_d3dReflect(code, size, kWsShaderReflectionIid,
                            reinterpret_cast<void**>(&refl))) || !refl) {
        return false;
    }
    D3D11_SHADER_DESC desc = {};
    bool match = false;
    if (SUCCEEDED(refl->GetDesc(&desc)) && desc.BoundResources == 2 &&
        desc.ConstantBuffers == 1) {
        bool tex0 = false, tex1 = false;
        for (UINT i = 0; i < desc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC b = {};
            if (FAILED(refl->GetResourceBindingDesc(i, &b))) continue;
            if (b.Type == D3D_SIT_TEXTURE && b.BindPoint == 0) tex0 = true;
            if (b.Type == D3D_SIT_TEXTURE && b.BindPoint == 1) tex1 = true;
        }
        ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(0);
        D3D11_SHADER_BUFFER_DESC bd = {};
        const bool cbSize = cb && SUCCEEDED(cb->GetDesc(&bd)) && bd.Size == 144;
        match = tex0 && tex1 && cbSize;
    }
    refl->Release();
    return match;
}

// The engine's TAA pass binds five textures: two colour targets and three
// uint targets (previous depth, current depth + jitter, tile flags), one
// 224-byte constant buffer, and it writes two targets.
bool looks_like_taa(const void* code, size_t size) {
    if (!g_d3dReflect || !code || !size) return false;
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(g_d3dReflect(code, size, kWsShaderReflectionIid,
                            reinterpret_cast<void**>(&refl))) || !refl) {
        return false;
    }
    D3D11_SHADER_DESC desc = {};
    bool match = false;
    if (SUCCEEDED(refl->GetDesc(&desc)) && desc.BoundResources == 5 &&
        desc.ConstantBuffers == 1 && desc.OutputParameters == 2) {
        bool colour = false, uintTex = false, t3 = false, t4 = false, t15 = false;
        for (UINT i = 0; i < desc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC b = {};
            if (FAILED(refl->GetResourceBindingDesc(i, &b))) continue;
            if (b.Type != D3D_SIT_TEXTURE) continue;
            if (b.BindPoint == 0 || b.BindPoint == 1) {
                colour = b.ReturnType == D3D_RETURN_TYPE_FLOAT || b.ReturnType == D3D_RETURN_TYPE_MIXED;
            }
            if (b.BindPoint == 3) { t3 = true; uintTex = b.ReturnType == D3D_RETURN_TYPE_UINT || b.ReturnType == D3D_RETURN_TYPE_MIXED; }
            if (b.BindPoint == 4) t4 = true;
            if (b.BindPoint == 15) t15 = true;
        }
        ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(0);
        D3D11_SHADER_BUFFER_DESC bd = {};
        const bool cbSize = cb && SUCCEEDED(cb->GetDesc(&bd)) && bd.Size == 224;
        match = colour && uintTex && t3 && t4 && t15 && cbSize;
    }
    refl->Release();
    return match;
}

uint64_t fnv1a64(const void* data, size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

bool fn_in_d3d11(void* fn) {
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11 || !fn) return false;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(fn), &owner)) {
        return false;
    }
    return owner == d3d11;
}

int g_drawSeen = 0;

void maybe_grade(ID3D11DeviceContext* ctx) {
    if (g_inGrade) return;
    ++g_drawSeen;
    if (ws::g_renderer.graded() || !ws::g_renderer.ready()) return;

    // The engine's tonemap pass binds exactly two textures (scene + bloom)
    // and a 144-byte constant buffer. That combination identifies it without
    // relying on bytecode hashes, which the engine patches at load time.
    ID3D11ShaderResourceView* srvs[2] = {};
    ctx->PSGetShaderResources(0, 2, srvs);
    ID3D11Buffer* cb = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &cb);
    D3D11_BUFFER_DESC bd = {};
    if (cb) cb->GetDesc(&bd);
    const bool stateMatch = srvs[0] && srvs[1] && cb && bd.ByteWidth == 144;
    if (srvs[0]) srvs[0]->Release();
    if (srvs[1]) srvs[1]->Release();
    if (cb) cb->Release();

    ID3D11PixelShader* bound = nullptr;
    ctx->PSGetShader(&bound, nullptr, nullptr);
    const bool shaderMatch = is_tonemap_shader(bound);
    if (bound) bound->Release();
    if (!stateMatch && !shaderMatch) return;
    if (!g_loggedStateTrigger) {
        g_loggedStateTrigger = true;
        ws::log_write("grade trigger: state=%d shader=%d draw=%d", stateMatch ? 1 : 0,
                      shaderMatch ? 1 : 0, g_drawSeen);
    }

    g_inGrade = true;
    ID3D11PixelShader* saved = g_currentPS;
    if (ws::g_renderer.grade_now()) {
        if (!g_loggedPreUi) {
            g_loggedPreUi = true;
            ws::log_write("grade: applied at the tonemap pass (UI untouched)");
        }
    }
    g_currentPS = saved;
    g_inGrade = false;
}

HRESULT STDMETHODCALLTYPE HookCreateDeferredContext(ID3D11Device* self, UINT flags,
                                                       ID3D11DeviceContext** out) {
    HRESULT hr = g_origCreateDeferred(self, flags, out);
    if (!g_loggedDeferred) {
        g_loggedDeferred = true;
        ws::log_write("deferred context created (hr=0x%08lx)", hr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(ID3D11Device* self, const void* code,
                                                SIZE_T size, ID3D11ClassLinkage* linkage,
                                                ID3D11PixelShader** out) {
    HRESULT hr = g_origCreatePS(self, code, size, linkage, out);
    const uint64_t shaderHash = (code && size) ? fnv1a64(code, size) : 0;
    const bool byHash = shaderHash && ws::is_tonemap_shader_hash(shaderHash);
    if (SUCCEEDED(hr) && out && *out && shaderHash && ws::is_ui_shader_hash(shaderHash)) {
        if (g_uiCount < 64) g_uiShaders[g_uiCount++] = *out;
    }
    if (SUCCEEDED(hr) && out && *out && (byHash || looks_like_tonemap(code, size))) {
        bool exists = false;
        for (int i = 0; i < g_tonemapCount; ++i) {
            if (g_tonemapShaders[i] == *out) { exists = true; break; }
        }
        if (!exists && g_tonemapCount < 256) {
            g_tonemapShaders[g_tonemapCount++] = *out;
        }
        if (!g_loggedCapture) {
            g_loggedCapture = true;
            ws::log_write("tonemap shader family captured");
        }
    }
    if (SUCCEEDED(hr) && out && *out) {
        const bool taa = (shaderHash && ws::is_taa_shader_hash(shaderHash)) ||
                         looks_like_taa(code, size);
        if (taa) {
            ws::taa::note_shader(*out);
            if (!g_loggedTaa) {
                g_loggedTaa = true;
                ws::log_write("taa: engine pass found (hash=%d) - using the WarmerSun shader",
                              shaderHash && ws::is_taa_shader_hash(shaderHash) ? 1 : 0);
            }
        }
    }
    return hr;
}

void STDMETHODCALLTYPE HookPSSetShader(ID3D11DeviceContext* self, ID3D11PixelShader* ps,
                                       ID3D11ClassInstance* const* instances, UINT count) {
    g_currentPS = ps;
    g_origPSSetShader(self, ps, instances, count);
}

// ---------------------------------------------------------------------------
// TAA / motion blur: swap the engine's TAA pixel shader for ours while its
// draw runs. Every binding the engine made stays untouched; only the shader
// and the (otherwise unused) b1 slot change.
// ---------------------------------------------------------------------------
bool taa_begin(ID3D11DeviceContext* ctx) {
    if (!ws::taa::enabled() || !ws::taa::replacement()) return false;
    if (!ws::taa::is_taa(g_currentPS)) return false;

    // g_currentPS is only a hint (it is not owned), so ask the context which
    // shader is really bound. PSGetShader hands back a reference, which is what
    // we restore later - never the unowned hint.
    ID3D11PixelShader* bound = nullptr;
    ctx->PSGetShader(&bound, nullptr, nullptr);
    if (!bound || bound != g_currentPS || !ws::taa::is_taa(bound)) {
        if (bound) bound->Release();
        return false;
    }
    g_taaSavedPS = bound;

    static bool logged = false;
    if (!logged) {
        logged = true;
        ws::log_write("taa: resolving with the WarmerSun shader");
    }
    ctx->PSGetConstantBuffers(1, 1, &g_taaSavedCB);
    ID3D11Buffer* tune = ws::taa::tune_buffer();
    if (tune) {
        ws::taa::write_tune(ctx);
        ctx->PSSetConstantBuffers(1, 1, &tune);
    }
    g_origPSSetShader(ctx, ws::taa::replacement(), nullptr, 0);
    static bool loggedSwap = false;
    if (!loggedSwap) {
        loggedSwap = true;
        ws::log_write("taa: resolver bound, drawing");
    }
    return true;
}

void taa_end(ID3D11DeviceContext* ctx) {
    if (g_taaSavedPS) {
        static bool loggedRestore = false;
        if (!loggedRestore) {
            loggedRestore = true;
            ws::log_write("taa: draw finished, restoring the engine shader");
        }
        g_origPSSetShader(ctx, g_taaSavedPS, nullptr, 0);
        g_taaSavedPS->Release();
        g_taaSavedPS = nullptr;
    }
    if (g_taaSavedCB) {
        ctx->PSSetConstantBuffers(1, 1, &g_taaSavedCB);
        g_taaSavedCB->Release();
        g_taaSavedCB = nullptr;
    } else {
        ctx->PSSetConstantBuffers(1, 1, nullptr);
    }
}

void maybe_grade_before_ui(ID3D11DeviceContext* ctx) {
    if (g_inGrade || ws::g_renderer.graded() || !ws::g_renderer.ready()) return;
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    const bool ui = is_ui_shader(ps);
    if (ps) ps->Release();
    if (!ui) return;
    g_inGrade = true;
    if (ws::g_renderer.grade_now() && !g_loggedUi) {
        g_loggedUi = true;
        ws::log_write("grade: applied before the UI shader (scene only)");
    }
    g_inGrade = false;
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* self, UINT count, UINT start) {
    maybe_grade_before_ui(self);
    const bool taa = taa_begin(self);
    g_origDraw(self, count, start);
    if (taa) taa_end(self);
    maybe_grade(self);
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* self, UINT count, UINT start,
                                       INT base) {
    maybe_grade_before_ui(self);
    const bool taa = taa_begin(self);
    g_origDrawIndexed(self, count, start, base);
    if (taa) taa_end(self);
    maybe_grade(self);
}

void STDMETHODCALLTYPE HookExecuteCommandList(ID3D11DeviceContext* self,
                                                 ID3D11CommandList* list, BOOL restore) {
    g_origExecuteCommandList(self, list, restore);
    if (!g_loggedExecute) {
        g_loggedExecute = true;
        ws::log_write("deferred command list executed");
    }
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* self, UINT vertexCount,
                                         UINT instanceCount, UINT startVertex,
                                         UINT startInstance) {
    maybe_grade_before_ui(self);
    const bool taa = taa_begin(self);
    g_origDrawInstanced(self, vertexCount, instanceCount, startVertex, startInstance);
    if (taa) taa_end(self);
    maybe_grade(self);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* self, UINT indexCount,
                                                UINT instanceCount, UINT startIndex,
                                                INT baseVertex, UINT startInstance) {
    maybe_grade_before_ui(self);
    const bool taa = taa_begin(self);
    g_origDrawIndexedInstanced(self, indexCount, instanceCount, startIndex, baseVertex,
                               startInstance);
    if (taa) taa_end(self);
    maybe_grade(self);
}

void install_shader_hooks(ID3D11Device* device) {
    if (g_shaderHooks || !device) return;
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) return;

    void** dv = *reinterpret_cast<void***>(device);
    void** cv = *reinterpret_cast<void***>(ctx);
    if (!fn_in_d3d11(dv[15]) || !fn_in_d3d11(cv[9]) || !fn_in_d3d11(cv[13]) ||
        !fn_in_d3d11(cv[12])) {
        ws::log_write("shader vtables not recognised; pre-UI grade disabled");
        ctx->Release();
        return;
    }

    if (MH_Initialize() != MH_OK) {
        ws::log_write("minhook initialization failed; pre-UI grade disabled");
        ctx->Release();
        return;
    }

    MH_CreateHook(reinterpret_cast<void*>(dv[15]),
                  reinterpret_cast<void*>(&HookCreatePixelShader),
                  reinterpret_cast<void**>(&g_origCreatePS));
    MH_CreateHook(reinterpret_cast<void*>(cv[13]), reinterpret_cast<void*>(&HookDraw),
                  reinterpret_cast<void**>(&g_origDraw));
    MH_CreateHook(reinterpret_cast<void*>(cv[12]), reinterpret_cast<void*>(&HookDrawIndexed),
                  reinterpret_cast<void**>(&g_origDrawIndexed));
    MH_CreateHook(reinterpret_cast<void*>(cv[21]), reinterpret_cast<void*>(&HookDrawInstanced),
                  reinterpret_cast<void**>(&g_origDrawInstanced));
    MH_CreateHook(reinterpret_cast<void*>(cv[20]),
                  reinterpret_cast<void*>(&HookDrawIndexedInstanced),
                  reinterpret_cast<void**>(&g_origDrawIndexedInstanced));
    MH_CreateHook(reinterpret_cast<void*>(cv[9]), reinterpret_cast<void*>(&HookPSSetShader),
                  reinterpret_cast<void**>(&g_origPSSetShader));

    const MH_STATUS enable = MH_EnableHook(MH_ALL_HOOKS);
    if (enable != MH_OK) {
        ws::log_write("minhook enable failed (%d)", static_cast<int>(enable));
        ctx->Release();
        return;
    }

    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (comp) {
        g_d3dReflect = reinterpret_cast<PFN_D3DReflect>(GetProcAddress(comp, "D3DReflect"));
    }
    g_hookDevice = device;
    g_hookContext = ctx;
    g_shaderHooks = true;
    ws::taa::install(device, nullptr);
    ws::log_write("shader hooks installed via MinHook (reflect=%d)", g_d3dReflect ? 1 : 0);
}


// ---------------------------------------------------------------------------
// swap chain hooks
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync, UINT flags) {
    static bool first = false;
    if (!first) { first = true; ws::log_write("hook: Present called (flags=0x%x)", flags); }
    ws::g_renderer.present(self, flags);
    return g_origPresent(self, sync, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain* self, UINT sync, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    static bool first = false;
    if (!first) { first = true; ws::log_write("hook: Present1 called (flags=0x%x)", flags); }
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
    trace_raw("swapchain: patch");
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

    // Slots from dxgi1_4.h IDXGISwapChain3Vtbl: Present 8, ResizeBuffers 13,
    // Present1 22, ResizeBuffers1 39.
    g_origPresent = reinterpret_cast<PFN_Present>(vt[8]);
    vt[8] = reinterpret_cast<void*>(&HookPresent);

    if (belongs_to_real_dxgi(vt[13])) {
        g_origResizeBuffers = reinterpret_cast<PFN_ResizeBuffers>(vt[13]);
        vt[13] = reinterpret_cast<void*>(&HookResizeBuffers);
    }
    if (belongs_to_real_dxgi(vt[22])) {
        g_origPresent1 = reinterpret_cast<PFN_Present1>(vt[22]);
        vt[22] = reinterpret_cast<void*>(&HookPresent1);
    }
    if (belongs_to_real_dxgi(vt[39])) {
        g_origResizeBuffers1 = reinterpret_cast<PFN_ResizeBuffers1>(vt[39]);
        vt[39] = reinterpret_cast<void*>(&HookResizeBuffers1);
    }

    VirtualProtect(vt, span, old, &old);
    g_patchedSwapchain = true;
    ws::log_write("swap chain hooked (Present%s%s)", g_origPresent1 ? "/Present1" : "",
                  g_origResizeBuffers1 ? "/ResizeBuffers1" : "");

    ID3D11Device* device = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device),
                                reinterpret_cast<void**>(&device))) && device) {
        install_shader_hooks(device);
        device->Release();
    }
}

// ---------------------------------------------------------------------------
// factory hooks
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(void* self, IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) {
    trace_raw("hook: CreateSwapChain");
    ws::log_write("hook: CreateSwapChain");
    HRESULT hr = g_origCreateSwapChain(self, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) patch_swapchain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(
    void* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict, IDXGISwapChain1** out) {
    ws::log_write("hook: CreateSwapChainForHwnd");
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
    trace_raw("factory: patch");
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

    // Slots from dxgi1_2.h IDXGIFactory2Vtbl: CreateSwapChain 10,
    // CreateSwapChainForHwnd 15, ...ForCoreWindow 16, ...ForComposition 24.
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
    if (belongs_to_real_dxgi(vt[24])) {
        g_origCreateSwapChainComp = reinterpret_cast<PFN_CreateSwapChainForComposition>(vt[24]);
        vt[24] = reinterpret_cast<void*>(&HookCreateSwapChainForComposition);
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

using PFN_D3D11CreateDevice = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
PFN_D3D11CreateDevice g_origD3D11CreateDevice = nullptr;

HRESULT WINAPI HookD3D11CreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context) {
    HRESULT hr = g_origD3D11CreateDevice(adapter, driverType, software, flags, levels,
                                         levelCount, sdkVersion, device, featureLevel, context);
    if (SUCCEEDED(hr) && device && *device) install_shader_hooks(*device);
    return hr;
}

HRESULT WINAPI HookD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context) {
    HRESULT hr = g_origD3D11CreateSwap(adapter, driverType, software, flags, levels, levelCount,
                                       sdkVersion, desc, swapChain, device, featureLevel, context);
    if (SUCCEEDED(hr) && swapChain && *swapChain) patch_swapchain(*swapChain);
    if (SUCCEEDED(hr) && device && *device) install_shader_hooks(*device);
    return hr;
}

typedef FARPROC(WINAPI* PFN_GetProcAddress)(HMODULE, LPCSTR);
PFN_GetProcAddress g_origGetProcAddress = nullptr;

FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name);

void patch_getprocaddress_once() {
    if (g_origGetProcAddress) return;
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
        if (_stricmp(dll, "kernel32.dll") != 0) continue;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->FirstThunk);
        auto* names = imports->OriginalFirstThunk
                          ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->OriginalFirstThunk)
                          : nullptr;
        for (; thunk->u1.Function; ++thunk) {
            if (names && !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                auto* hint = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(hint->Name), "GetProcAddress") == 0) {
                    auto** slot = reinterpret_cast<PFN_GetProcAddress*>(&thunk->u1.Function);
                    DWORD old = 0;
                    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                        g_origGetProcAddress = *slot;
                        *slot = &HookGetProcAddress;
                        VirtualProtect(slot, sizeof(void*), old, &old);
                        ws::log_write("IAT hooked: GetProcAddress");
                        return;
                    }
                }
            }
            if (names) ++names;
        }
    }
}

FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name) {
    FARPROC fn = g_origGetProcAddress ? g_origGetProcAddress(module, name) : nullptr;
    if (!name || !fn) return fn;
    if (strcmp(name, "D3D11CreateDevice") == 0) {
        g_origD3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(fn);
        ws::log_write("GetProcAddress: D3D11CreateDevice redirected");
        return reinterpret_cast<FARPROC>(&HookD3D11CreateDevice);
    }
    if (strcmp(name, "D3D11CreateDeviceAndSwapChain") == 0) {
        g_origD3D11CreateSwap = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(fn);
        ws::log_write("GetProcAddress: D3D11CreateDeviceAndSwapChain redirected");
        return reinterpret_cast<FARPROC>(&HookD3D11CreateDeviceAndSwapChain);
    }
    return fn;
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
                const char* importName = reinterpret_cast<const char*>(hint->Name);
                if (strcmp(importName, "D3D11CreateDeviceAndSwapChain") == 0) {
                    auto** slot = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain*>(&thunk->u1.Function);
                    DWORD old = 0;
                    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                        g_origD3D11CreateSwap = *slot;
                        *slot = &HookD3D11CreateDeviceAndSwapChain;
                        VirtualProtect(slot, sizeof(void*), old, &old);
                        ws::log_write("IAT hooked: D3D11CreateDeviceAndSwapChain");
                        return;
                    }
                } else if (strcmp(importName, "D3D11CreateDevice") == 0) {
                    auto** slot = reinterpret_cast<PFN_D3D11CreateDevice*>(&thunk->u1.Function);
                    DWORD old = 0;
                    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                        g_origD3D11CreateDevice = *slot;
                        *slot = &HookD3D11CreateDevice;
                        VirtualProtect(slot, sizeof(void*), old, &old);
                        ws::log_write("IAT hooked: D3D11CreateDevice");
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
        patch_getprocaddress_once();
        patch_iat_once();
        if (g_origGetProcAddress) break;
        Sleep(50);
    }
    return 0;
}

} // namespace

namespace ws {
void refresh_shader_hooks() {
    // MinHook patches function code; no refresh needed.
}
} // namespace ws

// ---------------------------------------------------------------------------
// exported proxy functions
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory) {
    trace_raw("export: CreateDXGIFactory");
    if (!ensure_real()) return E_FAIL;
    HRESULT hr = g_origCreateFactory(riid, factory);
    if (SUCCEEDED(hr) && factory && *factory) patch_factory(reinterpret_cast<IUnknown*>(*factory));
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
    trace_raw("export: CreateDXGIFactory1");
    if (!ensure_real()) return E_FAIL;
    HRESULT hr = g_origCreateFactory1 ? g_origCreateFactory1(riid, factory)
                                      : g_origCreateFactory(riid, factory);
    if (SUCCEEDED(hr) && factory && *factory) patch_factory(reinterpret_cast<IUnknown*>(*factory));
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    trace_raw("export: CreateDXGIFactory2");
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
































