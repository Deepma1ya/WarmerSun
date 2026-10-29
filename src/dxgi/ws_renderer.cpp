// WarmerSun - present-level D3D11 grading pipeline.
#include "ws_renderer.h"
#include "ws_menu.h"
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <d3dcompiler.h>

namespace ws {

Renderer g_renderer;

namespace {

HMODULE self_module() {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self_module), &mod);
    return mod;
}

bool is_srgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

bool is_float_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

const char* kShaderCommon = R"HLSL(
cbuffer Params : register(b0)
{
    float4 g0; // preExposure+exposure, preContrast, contrast, saturation
    float4 g1; // vibrance, highlights, shadows, whites
    float4 g2; // blacks, temperature, tint, sunWarmth
    float4 g3; // nightExposure, nightHi, nightLo, sharpness
    float4 g4; // texelX, texelY, dt, dynamicEnabled
    float4 g5; // adaptSpeed, key, minEV, maxEV
    float4 g6; // downsample scaleX, scaleY, inputEncoded, outputEncoded
    float4 g7; // sharpen edges (0..1)
};

SamplerState Lin : register(s0);
SamplerState Pt  : register(s1);

static const float PIVOT = 0.18;

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 srgb_to_linear(float3 c)
{
    c = saturate(c);
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return lerp(lo, hi, step(0.04045, c));
}

float3 linear_to_srgb(float3 c)
{
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, c));
}

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSFull(uint id : SV_VertexID)
{
    VSOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv  = p;
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)HLSL";

const char* kShaderLum64 = R"HLSL(
Texture2D<float4> Src : register(t0);

float4 PSLum64(VSOut i) : SV_Target
{
    float2 off = g4.xy * g6.xy * 0.5;
    float3 a = Src.Sample(Lin, i.uv + float2(-off.x, -off.y)).rgb;
    float3 b = Src.Sample(Lin, i.uv + float2( off.x, -off.y)).rgb;
    float3 c = Src.Sample(Lin, i.uv + float2(-off.x,  off.y)).rgb;
    float3 d = Src.Sample(Lin, i.uv + float2( off.x,  off.y)).rgb;
    float3 col = (a + b + c + d) * 0.25;
    if (g6.z > 0.5) col = srgb_to_linear(col);
    return log2(max(luminance(col), 1e-5));
}
)HLSL";

const char* kShaderLum1 = R"HLSL(
Texture2D<float> Src : register(t0);

float4 PSLum1(VSOut i) : SV_Target
{
    float sum = 0.0;
    [unroll] for (int y = 0; y < 16; ++y)
    {
        [unroll] for (int x = 0; x < 16; ++x)
        {
            float2 uv = (float2(x, y) + 0.5) / 16.0;
            sum += Src.SampleLevel(Pt, uv, 0);
        }
    }
    return sum / 256.0;
}
)HLSL";

const char* kShaderAdapt = R"HLSL(
Texture2D<float>  Lum  : register(t0);
Texture2D<float2> Prev : register(t1);

float2 PSAdapt(VSOut i) : SV_Target
{
    float avgLog = Lum.SampleLevel(Pt, float2(0.5, 0.5), 0);
    float avgLin = exp2(avgLog);

    float targetEV = log2(max(g5.y, 1e-4)) - avgLog;
    targetEV = clamp(targetEV, g5.z, g5.w);

    float2 prev = Prev.SampleLevel(Pt, float2(0.5, 0.5), 0);
    float alpha = 1.0 - exp(-max(g5.x, 0.01) * max(g4.z, 0.0005));
    float ev = (g4.w > 0.5) ? lerp(prev.x, targetEV, saturate(alpha)) : 0.0;

    return float2(ev, avgLin);
}
)HLSL";

const char* kShaderFinal = R"HLSL(
Texture2D<float4> Src      : register(t0);
Texture2D<float2> Exposure : register(t1);

float3 fetch_linear(float2 uv)
{
    float3 c = Src.SampleLevel(Lin, uv, 0).rgb;
    return (g6.z > 0.5) ? srgb_to_linear(c) : c;
}

float4 PSFinal(VSOut i) : SV_Target
{
    float2 texel = g4.xy;
    float3 c = fetch_linear(i.uv);

    // Contrast adaptive sharpening (CAS) on the linear signal.
    if (g3.w > 0.0001)
    {
        float3 n = fetch_linear(i.uv + float2(0.0, -texel.y));
        float3 s = fetch_linear(i.uv + float2(0.0,  texel.y));
        float3 e = fetch_linear(i.uv + float2( texel.x, 0.0));
        float3 w = fetch_linear(i.uv + float2(-texel.x, 0.0));

        float3 mn = min(c, min(min(n, s), min(e, w)));
        float3 mx = max(c, max(max(n, s), max(e, w)));
        float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));
        float edgeGain = lerp(0.70, 1.30, saturate(g7.x));
        float peak = -1.0 / lerp(8.0, 5.0, saturate(g3.w)) * edgeGain;
        float3 weight = amp * peak;
        c = saturate((c + (n + s + e + w) * weight) / (1.0 + 4.0 * weight));
    }

    // Exposure: manual, dynamic, and a night lift keyed off scene luminance.
    float2 exposure = Exposure.SampleLevel(Pt, float2(0.5, 0.5), 0);
    float night = 1.0 - smoothstep(g3.z, g3.y, exposure.y);
    float ev = g0.x + exposure.x + g3.x * night;
    c *= exp2(ev);

    // White balance (temperature / tint), luminance normalised.
    float t  = g2.y / 100.0;
    float ti = g2.z / 100.0;
    float3 gain = float3(1.0 + 0.14 * t, 1.0, 1.0 - 0.14 * t);
    gain.y *= 1.0 - 0.07 * ti;
    gain.x *= 1.0 + 0.035 * ti;
    gain.z *= 1.0 + 0.035 * ti;
    gain /= max(luminance(gain), 1e-4);
    c *= gain;

    // Pre-contrast, then tonal zones.
    c = PIVOT * pow(max(c, 0.0) / PIVOT, max(g0.y, 0.01));

    float l = luminance(c);
    float shadowMask = 1.0 - smoothstep(0.0, 0.35, l);
    float highMask   = smoothstep(0.35, 1.0, l);
    c *= (1.0 + g1.z * shadowMask + g1.y * highMask);
    c += g1.w * pow(saturate(l), 4.0) * 0.25;
    c += g2.x * pow(1.0 - saturate(l), 4.0) * 0.25;

    // Vibrance (protects already saturated colours) then saturation.
    float l2 = luminance(c);
    float chroma = length(c - l2) / max(l2, 1e-4);
    float satAmount = saturate(chroma);
    float satScale = g0.w * (1.0 + g1.x * (1.0 - satAmount));
    c = lerp(float3(l2, l2, l2), c, satScale);

    // Contrast (power curve, pivot 0.18).
    c = PIVOT * pow(max(c, 0.0) / PIVOT, max(g0.z, 0.01));

    // Signature warm highlights.
    float lh = luminance(c);
    float3 warm = float3(1.06, 1.0, 0.90);
    c *= lerp(float3(1.0, 1.0, 1.0), warm, smoothstep(0.55, 1.0, lh) * saturate(g2.w));

    // Soft shoulder: pulls down only what would otherwise clip.
    float peak = max(max(c.r, c.g), c.b);
    c = c / (1.0 + max(peak - 1.0, 0.0) * 0.6);

    float3 outc = (g6.w > 0.5) ? linear_to_srgb(c) : c;

    // Ordered-ish dither to keep gradients clean after 8-bit output.
    float dither = frac(sin(dot(i.uv * g4.xy, float2(12.9898, 78.233))) * 43758.5453);
    outc += (dither - 0.5) / 255.0;

    return float4(saturate(outc), 1.0);
}
)HLSL";

typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                        ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob**, ID3DBlob**);

struct Compiler {
    HMODULE module = nullptr;
    PFN_D3DCompile compile = nullptr;

    bool load() {
        if (compile) return true;
        module = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!module) {
            wchar_t sys[MAX_PATH] = {};
            UINT n = GetSystemDirectoryW(sys, MAX_PATH);
            if (n && n < MAX_PATH) {
                std::wstring path(sys, n);
                path += L"\\d3dcompiler_47.dll";
                module = LoadLibraryW(path.c_str());
            }
        }
        if (!module) return false;
        compile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(module, "D3DCompile"));
        return compile != nullptr;
    }
};

Compiler g_compiler;

struct SavedState {
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    UINT viewportCount = 0;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT scissorCount = 0;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11Buffer* psCBs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11Buffer* vsCBs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11RasterizerState* raster = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
};

void save_state(ID3D11DeviceContext* ctx, SavedState& s) {
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs, &s.dsv);
    ctx->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
    ctx->OMGetDepthStencilState(&s.depth, &s.stencilRef);

    s.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&s.viewportCount, s.viewports);
    s.scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&s.scissorCount, s.scissors);
    ctx->RSGetState(&s.raster);

    ctx->IAGetInputLayout(&s.layout);
    ctx->IAGetPrimitiveTopology(&s.topology);

    ctx->VSGetShader(&s.vs, nullptr, nullptr);
    ctx->PSGetShader(&s.ps, nullptr, nullptr);
    ctx->GSGetShader(&s.gs, nullptr, nullptr);
    ctx->HSGetShader(&s.hs, nullptr, nullptr);
    ctx->DSGetShader(&s.ds, nullptr, nullptr);

    ctx->PSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, s.samplers);
    ctx->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, s.srvs);
    ctx->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, s.psCBs);
    ctx->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, s.vsCBs);
}

void restore_state(ID3D11DeviceContext* ctx, SavedState& s) {
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs, s.dsv);
    ctx->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask);
    ctx->OMSetDepthStencilState(s.depth, s.stencilRef);
    if (s.viewportCount) ctx->RSSetViewports(s.viewportCount, s.viewports);
    if (s.scissorCount) ctx->RSSetScissorRects(s.scissorCount, s.scissors);
    ctx->RSSetState(s.raster);
    ctx->IASetInputLayout(s.layout);
    ctx->IASetPrimitiveTopology(s.topology);
    ctx->VSSetShader(s.vs, nullptr, 0);
    ctx->PSSetShader(s.ps, nullptr, 0);
    ctx->GSSetShader(s.gs, nullptr, 0);
    ctx->HSSetShader(s.hs, nullptr, 0);
    ctx->DSSetShader(s.ds, nullptr, 0);
    ctx->PSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, s.samplers);
    ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, s.srvs);
    ctx->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, s.psCBs);
    ctx->VSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, s.vsCBs);

    for (auto*& p : s.samplers) if (p) { p->Release(); p = nullptr; }
    for (auto*& p : s.srvs) if (p) { p->Release(); p = nullptr; }
    for (auto*& p : s.psCBs) if (p) { p->Release(); p = nullptr; }
    for (auto*& p : s.vsCBs) if (p) { p->Release(); p = nullptr; }
    for (auto*& p : s.rtvs) if (p) { p->Release(); p = nullptr; }
    if (s.dsv) { s.dsv->Release(); s.dsv = nullptr; }
    if (s.layout) { s.layout->Release(); s.layout = nullptr; }
    if (s.raster) { s.raster->Release(); s.raster = nullptr; }
    if (s.blend) { s.blend->Release(); s.blend = nullptr; }
    if (s.depth) { s.depth->Release(); s.depth = nullptr; }
    if (s.vs) { s.vs->Release(); s.vs = nullptr; }
    if (s.ps) { s.ps->Release(); s.ps = nullptr; }
    if (s.gs) { s.gs->Release(); s.gs = nullptr; }
    if (s.hs) { s.hs->Release(); s.hs = nullptr; }
    if (s.ds) { s.ds->Release(); s.ds = nullptr; }
}

} // namespace

// ---------------------------------------------------------------------------
// setup / teardown
// ---------------------------------------------------------------------------

void Renderer::set_config(const Config& cfg) {
    config_ = cfg;
}

void Renderer::reload_config() {
    HMODULE mod = self_module();
    Config fresh;
    fresh.preset = config_.preset;
    std::wstring path = find_config(mod);
    load_config(path, fresh, 0);
    // Keep command-line-ish runtime state that is not in the file.
    fresh.enabled = fresh.enabled && !fresh.safeMode;
    config_ = fresh;
    log_write("config reloaded (preset %c, enabled=%d)", config_.preset, config_.enabled ? 1 : 0);
}

void Renderer::notify_resize() {
    release_size_dependent();
}

bool Renderer::init(IDXGISwapChain* sc) {
    ws::trace_raw("init: enter");
    if (ready_) return true;
    if (failed_) return false;
    swap_ = sc;
    log_write("init: begin");

    HRESULT hr = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device_));
    if (FAILED(hr) || !device_) {
        log_write("GetDevice failed (hr=0x%08lx) - WarmerSun disabled", hr);
        failed_ = true;
        return false;
    }
    log_write("init: device ok");
    device_->GetImmediateContext(&context_);
    if (context_) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1),
                                         reinterpret_cast<void**>(&sc1))) && sc1) {
            sc1->GetHwnd(&hwnd_);
            sc1->Release();
        }
    }
    if (!context_) {
        log_write("GetImmediateContext failed - WarmerSun disabled");
        failed_ = true;
        return false;
    }

    log_write("init: context ok");
    if (!g_compiler.load()) {
        log_write("d3dcompiler_47.dll unavailable - WarmerSun disabled");
        failed_ = true;
        return false;
    }
    log_write("init: compiler ok");
    if (!compile_shaders() || !create_states()) {
        failed_ = true;
        return false;
    }

    log_write("init: shaders+states ok");

    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(float) * 4 * 8;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cb, nullptr, &constants_))) {
        log_write("constant buffer creation failed");
        failed_ = true;
        return false;
    }

    log_write("init: constant buffer ok");

    // Ping-pong exposure targets (EV, average luminance).
    for (int i = 0; i < 2; ++i) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &exposure_[i]))) {
            log_write("exposure target creation failed");
            failed_ = true;
            return false;
        }
        device_->CreateRenderTargetView(exposure_[i], nullptr, &exposureRTV_[i]);
        device_->CreateShaderResourceView(exposure_[i], nullptr, &exposureSRV_[i]);

        const float zero[4] = {0, 0, 0, 0};
        context_->ClearRenderTargetView(exposureRTV_[i], zero);
    }

    log_write("init: exposure targets ok");
    if (hwnd_) ws::menu_init(hwnd_, device_, context_, find_config(self_module()));

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(sc->GetDesc(&desc))) {
        log_write("swap chain: %ux%u fmt=%d buffers=%u", desc.BufferDesc.Width,
                  desc.BufferDesc.Height, static_cast<int>(desc.BufferDesc.Format),
                  desc.BufferCount);
    }

    ready_ = true;
    log_write("WarmerSun renderer ready (preset %c)", config_.preset);
    return true;
}

void Renderer::shutdown() {
    ws::menu_shutdown();
    release_size_dependent();
    destroy_pipeline();
    for (int i = 0; i < 2; ++i) {
        if (exposureSRV_[i]) { exposureSRV_[i]->Release(); exposureSRV_[i] = nullptr; }
        if (exposureRTV_[i]) { exposureRTV_[i]->Release(); exposureRTV_[i] = nullptr; }
        if (exposure_[i]) { exposure_[i]->Release(); exposure_[i] = nullptr; }
    }
    if (constants_) { constants_->Release(); constants_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    ready_ = false;
}

bool Renderer::compile_shaders() {
    auto build = [&](const char* src, const char* entry, const char* target) -> ID3DBlob* {
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        std::string full = std::string(kShaderCommon) + src;
        HRESULT hr = g_compiler.compile(full.c_str(), full.size(), "warmer_sun",
                                        nullptr, nullptr, entry, target,
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3 |
                                            D3DCOMPILE_ENABLE_STRICTNESS,
                                        0, &blob, &errors);
        if (FAILED(hr)) {
            log_write("shader %s failed: %s", entry,
                      errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
            if (errors) errors->Release();
            if (blob) blob->Release();
            return nullptr;
        }
        if (errors) errors->Release();
        return blob;
    };

    ID3DBlob* vs = build("", "VSFull", "vs_5_0");
    if (!vs) return false;
    HRESULT hr = device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                                             nullptr, &vsFull_);
    vs->Release();
    if (FAILED(hr)) return false;

    struct Entry { const char* src; const char* name; ID3D11PixelShader** out; };
    Entry entries[] = {
        {kShaderLum64, "PSLum64", &psLum64_},
        {kShaderLum1, "PSLum1", &psLum1_},
        {kShaderAdapt, "PSAdapt", &psAdapt_},
        {kShaderFinal, "PSFinal", &psFinal_},
    };
    for (auto& e : entries) {
        ID3DBlob* ps = build(e.src, e.name, "ps_5_0");
        if (!ps) return false;
        hr = device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(),
                                        nullptr, e.out);
        ps->Release();
        if (FAILED(hr)) return false;
    }
    return true;
}

bool Renderer::create_states() {
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &samplerLin_))) return false;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(device_->CreateSamplerState(&sd, &samplerPt_))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bd, &blend_))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rd, &raster_))) return false;

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    if (FAILED(device_->CreateDepthStencilState(&dd, &depth_))) return false;

    return true;
}

void Renderer::destroy_pipeline() {
    if (vsFull_) { vsFull_->Release(); vsFull_ = nullptr; }
    if (psLum64_) { psLum64_->Release(); psLum64_ = nullptr; }
    if (psLum1_) { psLum1_->Release(); psLum1_ = nullptr; }
    if (psAdapt_) { psAdapt_->Release(); psAdapt_ = nullptr; }
    if (psFinal_) { psFinal_->Release(); psFinal_ = nullptr; }
    if (samplerLin_) { samplerLin_->Release(); samplerLin_ = nullptr; }
    if (samplerPt_) { samplerPt_->Release(); samplerPt_ = nullptr; }
    if (blend_) { blend_->Release(); blend_ = nullptr; }
    if (raster_) { raster_->Release(); raster_ = nullptr; }
    if (depth_) { depth_->Release(); depth_ = nullptr; }
}

void Renderer::release_size_dependent() {
    if (sceneSRV_) { sceneSRV_->Release(); sceneSRV_ = nullptr; }
    if (scene_) { scene_->Release(); scene_ = nullptr; }
    if (backRTV_) { backRTV_->Release(); backRTV_ = nullptr; }
    if (backTexture_) { backTexture_->Release(); backTexture_ = nullptr; }
    if (lum64SRV_) { lum64SRV_->Release(); lum64SRV_ = nullptr; }
    if (lum64RTV_) { lum64RTV_->Release(); lum64RTV_ = nullptr; }
    if (lum64_) { lum64_->Release(); lum64_ = nullptr; }
    if (lum1SRV_) { lum1SRV_->Release(); lum1SRV_ = nullptr; }
    if (lum1RTV_) { lum1RTV_->Release(); lum1RTV_ = nullptr; }
    if (lum1_) { lum1_->Release(); lum1_ = nullptr; }
    width_ = height_ = 0;
}

bool Renderer::create_size_dependent() {
    ID3D11Texture2D* back = nullptr;
    HRESULT hr = swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    if (FAILED(hr) || !back) {
        log_write("GetBuffer failed (hr=0x%08lx)", hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC bd = {};
    back->GetDesc(&bd);
    width_ = bd.Width;
    height_ = bd.Height;
    format_ = bd.Format;

    inputEncoded_ = !is_srgb(bd.Format) && !is_float_format(bd.Format);
    outputEncoded_ = inputEncoded_;

    hr = device_->CreateRenderTargetView(back, nullptr, &backRTV_);
    if (FAILED(hr)) {
        log_write("backbuffer RTV failed (hr=0x%08lx)", hr);
        back->Release();
        return false;
    }
    backTexture_ = back; // keep the reference; released in release_size_dependent

    // A swap-chain buffer is render-target only, so it cannot be sampled.
    // Copy it (identical format) into a shader-readable texture instead.
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = width_;
    sd.Height = height_;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = bd.Format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_DEFAULT;
    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, &scene_))) return false;
    if (FAILED(device_->CreateShaderResourceView(scene_, nullptr, &sceneSRV_))) return false;

    auto make1 = [&](UINT w, UINT h, ID3D11Texture2D** tex, ID3D11RenderTargetView** rtv,
                     ID3D11ShaderResourceView** srv) -> bool {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, tex))) return false;
        if (FAILED(device_->CreateRenderTargetView(*tex, nullptr, rtv))) return false;
        if (FAILED(device_->CreateShaderResourceView(*tex, nullptr, srv))) return false;
        return true;
    };

    if (!make1(64, 64, &lum64_, &lum64RTV_, &lum64SRV_)) return false;
    if (!make1(1, 1, &lum1_, &lum1RTV_, &lum1SRV_)) return false;

    log_write("render targets created: %ux%u fmt=%d inputEncoded=%d outputEncoded=%d", width_,
              height_, static_cast<int>(format_), inputEncoded_ ? 1 : 0,
              outputEncoded_ ? 1 : 0);
    return true;
}

bool Renderer::ensure_size() {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) ||
        !back) {
        return false;
    }
    D3D11_TEXTURE2D_DESC bd = {};
    back->GetDesc(&bd);
    back->Release();

    if (scene_ && bd.Width == width_ && bd.Height == height_ && bd.Format == format_) {
        return true;
    }
    release_size_dependent();
    return create_size_dependent();
}

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------

void Renderer::update_constants(float dt, bool dynamicEnabled) {
    const ColorParams& c = config_.color;
    float data[32] = {};
    data[0] = c.preExposure + c.exposure;
    data[1] = c.preContrast;
    data[2] = c.contrast;
    data[3] = c.saturation;

    data[4] = c.vibrance;
    data[5] = c.highlights;
    data[6] = c.shadows;
    data[7] = c.whites;

    data[8] = c.blacks;
    data[9] = c.temperature;
    data[10] = c.tint;
    data[11] = c.sunWarmth;

    data[12] = c.nightExposure;
    data[13] = 0.12f;
    data[14] = 0.015f;
    data[15] = config_.sharpen.enabled ? config_.sharpen.sharpness : 0.0f;

    data[16] = width_ ? 1.0f / static_cast<float>(width_) : 0.0f;
    data[17] = height_ ? 1.0f / static_cast<float>(height_) : 0.0f;
    data[18] = dt;
    data[19] = dynamicEnabled ? 1.0f : 0.0f;

    data[20] = config_.dynamic.adaptSpeed;
    data[21] = config_.dynamic.key;
    data[22] = config_.dynamic.minEV;
    data[23] = config_.dynamic.maxEV;

    data[24] = width_ ? static_cast<float>(width_) / 64.0f : 1.0f;
    data[25] = height_ ? static_cast<float>(height_) / 64.0f : 1.0f;
    data[26] = inputEncoded_ ? 1.0f : 0.0f;
    data[27] = outputEncoded_ ? 1.0f : 0.0f;

    data[28] = config_.sharpen.edges;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context_->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, data, sizeof(data));
        context_->Unmap(constants_, 0);
    }
}

void Renderer::tick_input() {
    static bool endDown = false;
    const bool endNow = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    if (endNow && !endDown) ws::menu_toggle();
    endDown = endNow;

    const int keys[3] = {config_.keyToggle, config_.keyPreset, config_.keyReload};
    for (int i = 0; i < 3; ++i) {
        bool down = keys[i] && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !keysDown_[i]) {
            if (i == 0) {
                config_.enabled = !config_.enabled;
                log_write("toggled: enabled=%d", config_.enabled ? 1 : 0);
            } else if (i == 1) {
                char next = config_.preset;
                switch (config_.preset) {
                case 'A': next = 'B'; break;
                case 'B': next = 'C'; break;
                case 'C': next = 'D'; break;
                default:  next = 'A'; break;
                }
                HMODULE mod = self_module();
                Config fresh;
                fresh.preset = next;
                load_config(find_config(mod), fresh, next);
                fresh.enabled = config_.enabled;
                config_ = fresh;
                log_write("preset -> %c", next);
            } else {
                reload_config();
            }
        }
        keysDown_[i] = down;
    }
}

bool Renderer::grade_now() {
    if (!ready_ || !context_) return false;
    if (graded_) return true;
    if (!ensure_size()) return false;

    { static bool once = false; if (!once) { once = true; void** cv = *reinterpret_cast<void***>(context_); ws::log_write("grade context vtable=%p slot13=%p", static_cast<void*>(cv), cv[13]); } }
    ULONGLONG now = GetTickCount64();
    float dt = lastTick_ ? static_cast<float>(now - lastTick_) / 1000.0f : 1.0f / 60.0f;
    lastTick_ = now;
    dt = std::clamp(dt, 0.001f, 0.1f);

    update_constants(dt, config_.dynamic.enabled);

    SavedState saved;
    save_state(context_, saved);

    auto set_viewport = [&](UINT w, UINT h) {
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(w);
        vp.Height = static_cast<float>(h);
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);
    };

    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vsFull_, nullptr, 0);
    context_->GSSetShader(nullptr, nullptr, 0);
    context_->HSSetShader(nullptr, nullptr, 0);
    context_->DSSetShader(nullptr, nullptr, 0);
    context_->OMSetBlendState(blend_, nullptr, 0xFFFFFFFF);
    context_->OMSetDepthStencilState(depth_, 0);
    context_->RSSetState(raster_);
    context_->PSSetConstantBuffers(0, 1, &constants_);

    ID3D11ShaderResourceView* nullSRV[2] = {nullptr, nullptr};
    ID3D11RenderTargetView* rtv = nullptr;

    // 1. snapshot the back buffer into a shader-readable copy (same format)
    context_->CopyResource(scene_, backTexture_);

    // 2. log-luminance chain
    ID3D11ShaderResourceView* sceneSRV = sceneSRV_;
    rtv = lum64RTV_;
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    set_viewport(64, 64);
    context_->PSSetShader(psLum64_, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &sceneSRV);
    context_->PSSetSamplers(0, 1, &samplerLin_);
    context_->Draw(3, 0);

    context_->PSSetShaderResources(0, 2, nullSRV);
    ID3D11ShaderResourceView* lum64SRV = lum64SRV_;
    rtv = lum1RTV_;
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    set_viewport(1, 1);
    context_->PSSetShader(psLum1_, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &lum64SRV);
    context_->PSSetSamplers(0, 1, &samplerPt_);
    context_->Draw(3, 0);

    // 3. temporal exposure adaptation
    context_->PSSetShaderResources(0, 2, nullSRV);
    int cur = exposureIndex_;
    int next = 1 - cur;
    ID3D11ShaderResourceView* adaptSRV[2] = {lum1SRV_, exposureSRV_[cur]};
    rtv = exposureRTV_[next];
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    set_viewport(1, 1);
    context_->PSSetShader(psAdapt_, nullptr, 0);
    context_->PSSetShaderResources(0, 2, adaptSRV);
    context_->Draw(3, 0);
    exposureIndex_ = next;

    // 4. grade into the backbuffer
    context_->PSSetShaderResources(0, 2, nullSRV);
    ID3D11ShaderResourceView* finalSRV[2] = {sceneSRV, exposureSRV_[exposureIndex_]};
    rtv = backRTV_;
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    set_viewport(width_, height_);
    context_->PSSetShader(psFinal_, nullptr, 0);
    context_->PSSetShaderResources(0, 2, finalSRV);
    context_->PSSetSamplers(0, 1, &samplerLin_);
    context_->Draw(3, 0);

    context_->PSSetShaderResources(0, 2, nullSRV);
    restore_state(context_, saved);
    graded_ = true;
    return true;
}

bool Renderer::present(IDXGISwapChain* sc, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return false;
    ws::refresh_shader_hooks();

    tick_input();
    if (!config_.enabled || config_.safeMode) return false;
    if (!ready_ && !init(sc)) return false;
    if (!ensure_size()) return false;

    // Preferred path: the tonemap draw hook already graded the scene before
    // the UI was drawn. Fallback: grade here (UI will be affected).
    if (!graded_) grade_now();

    if (ws::menu_visible() && backRTV_) {
        ID3D11RenderTargetView* bb = backRTV_;
        context_->OMSetRenderTargets(1, &bb, nullptr);
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(width_);
        vp.Height = static_cast<float>(height_);
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);
        ws::menu_render(context_, config_);
    }

    if (config_.dumpFrames && frame_ >= static_cast<unsigned long long>(config_.dumpAfter) &&
        dumped_ < static_cast<unsigned>(config_.dumpCount)) {
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(&back))) &&
            back) {
            dump_backbuffer(back);
            back->Release();
        }
        ++dumped_;
    }

    graded_ = false;
    ++frame_;
    return true;
}

void Renderer::dump_backbuffer(ID3D11Texture2D* back) {
    D3D11_TEXTURE2D_DESC d = {};
    back->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        log_write("frame dump skipped: unsupported backbuffer format %d",
                  static_cast<int>(d.Format));
        return;
    }

    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging))) return;
    context_->CopyResource(staging, back);
    context_->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        return;
    }

    wchar_t name[128];
    swprintf(name, 128, L"WarmerSun_frame_%05u.dds", dumped_);
    std::wstring path = data_dir(self_module());
    path += L"\\";
    path += name;

    FILE* fp = _wfopen(path.c_str(), L"wb");
    if (fp) {
        unsigned char header[128] = {};
        memcpy(header, "DDS ", 4);
        unsigned u = 124;
        memcpy(header + 4, &u, 4);
        unsigned flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000; // caps|height|width|pitch|mipcount
        memcpy(header + 8, &flags, 4);
        memcpy(header + 12, &d.Height, 4);
        memcpy(header + 16, &d.Width, 4);
        unsigned pitch = d.Width * 4;
        memcpy(header + 20, &pitch, 4);
        memcpy(header + 28, &d.MipLevels, 4);
        unsigned pfFlags = 0x1 | 0x40; // alphapixels | rgb
        unsigned pfSize = 32;
        memcpy(header + 76, &pfSize, 4);
        memcpy(header + 80, &pfFlags, 4);
        unsigned fourcc = 0;
        memcpy(header + 84, &fourcc, 4);
        unsigned bits = 32;
        memcpy(header + 88, &bits, 4);
        unsigned masks[4] = {0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000};
        if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            masks[0] = 0x000000ff; masks[1] = 0x0000ff00; masks[2] = 0x00ff0000; masks[3] = 0xff000000;
        }
        memcpy(header + 92, masks, 16);
        unsigned caps = 0x1000;
        memcpy(header + 108, &caps, 4);
        fwrite(header, 1, 128, fp);

        const unsigned char* src = static_cast<const unsigned char*>(mapped.pData);
        std::vector<unsigned char> row(pitch);
        for (UINT y = 0; y < d.Height; ++y) {
            memcpy(row.data(), src + static_cast<size_t>(y) * mapped.RowPitch, pitch);
            fwrite(row.data(), 1, pitch, fp);
        }
        fclose(fp);
        log_write("frame dumped: %ls", path.c_str());
    }

    context_->Unmap(staging, 0);
    staging->Release();
}

} // namespace ws















