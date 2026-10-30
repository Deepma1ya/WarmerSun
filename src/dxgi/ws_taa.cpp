// WarmerSun - temporal anti-aliasing and camera motion blur.
#include "ws_taa.h"

#include "ws_taa_hashes.h"
#include "ws_common.h"

#include <d3dcompiler.h>
#include <cstring>

namespace ws {
namespace taa {
namespace {

// ---------------------------------------------------------------------------
// The replacement pixel shader.
//
// Interface copied from the engine's "def.taa.compute" pass:
//   b0  float4 cb0[14]      engine supplied, never modified
//   b1  float4 tune0/tune1  ours
//   t0  float4  current colour
//   t1  float4  previous frame colour
//   t3  uint4   previous frame depth (log encoded)
//   t4  uint4   .x current depth, .y jitter index + depth tolerance
//   t15 uint4   tile flags (unused; the depth test is enough)
//   s0, s1      linear/clamp samplers
//   o0  resolved colour, o1 processed copy (matches the engine)
//
// cb0 layout, as used by the engine:
//   0..3  previous view-projection, applied to a view space position
//   6     previous view row 2 (depth compare)
//   8     x/y previous jitter offset, z/w history weight bounds
//   9     x depth scale, y/z texel step in uv
//   10    xy/zw screen to ndc scale
//   11    xy/zw pixel count
//   13    unprojection offset/scale
// ---------------------------------------------------------------------------
const char* kSource = R"HLSL(
cbuffer WS_CB0  : register(b0) { float4 cb0[14]; };
cbuffer WS_TUNE : register(b1) { float4 tune0; float4 tune1; };

Texture2D<float4> t0  : register(t0);
Texture2D<float4> t1  : register(t1);
Texture2D<uint4>  t3  : register(t3);
Texture2D<uint4>  t4  : register(t4);
Texture2D<uint4>  t15 : register(t15);

SamplerState s0 : register(s0);
SamplerState s1 : register(s1);

struct PS_IN {
    float4 pos : SV_POSITION;
    float4 uv  : TEXCOORD0;
};

float ws_depth(uint v)      { return exp(float(v) * (1.0 / 4096.0)) - 1.0; }
float ws_tolerance(uint v)  { return exp(float(v) * 0.001465) - 1.0; }
float3 ws_sample(Texture2D<float4> tex, SamplerState smp, float2 uv) {
    return tex.SampleLevel(smp, uv, 0.0).rgb;
}

float4 main(PS_IN i, out float4 o1 : SV_Target1) : SV_Target0
{
    const float2 uv = i.uv.xy;

    // pixel <-> uv factors (mirrors the engine's cb0[11] usage)
    float2 toPix = cb0[11].xy / max(cb0[11].zw, 1e-6);
    toPix = max(toPix, 1.0);
    float2 texel = float2(cb0[9].y, cb0[9].z);
    if (texel.x <= 0.0 || texel.y <= 0.0) texel = 1.0 / toPix;

    const int2 ip = int2(uv * toPix);
    const uint4 packed = t4.Load(int3(ip, 0));

    // ---- reconstruct the current pixel in view space ----------------------
    const float depthScaled = ws_depth(packed.x) * cb0[9].x;
    const float2 jitterIdx  = float2((packed.y) & 3u, (packed.y >> 2) & 3u) - 1.0;
    const float2 uvJ        = uv + jitterIdx * texel;

    const float2 ray = uvJ * cb0[10].zw * cb0[13].zw + cb0[13].xy;
    const float4 vp  = float4(-depthScaled * ray, 0.0, 1.0);

    const float4 clip  = float4(dot(cb0[0], vp), dot(cb0[1], vp), 0.0, dot(cb0[3], vp));
    const float  prevZ = dot(cb0[6], vp);

    const bool inFrustum = (clip.w > 1e-6) && (abs(clip.x) < clip.w) && (abs(clip.y) < clip.w);
    float2 prevUv = uvJ;
    if (inFrustum) {
        prevUv = (clip.xy / clip.w) * float2(0.5, -0.5) + 0.5 + cb0[8].xy;
    }

    // ---- depth validation against the previous frame ----------------------
    const uint  prevEnc    = t3.Load(int3(int2(prevUv * toPix), 0)).x;
    const float prevDepth  = ws_depth(prevEnc);
    const float tolerance  = ws_tolerance(packed.y >> 4) * 0.75;
    const bool  tooClose   = min(depthScaled, prevDepth * cb0[9].x) < 0.25;
    const bool  matchesPre = abs(prevDepth + prevZ) < tolerance;
    const bool  valid      = inFrustum && (tooClose || matchesPre);

    const float2 velPx = (prevUv - uv) * toPix;
    const float  velLen = length(velPx);

    // ---- camera motion blur ----------------------------------------------
    float3 cur = ws_sample(t0, s0, uv);
    const float mbAmount = tune0.z;
    if (mbAmount > 0.001 && velLen > 1.0) {
        const int taps = (int)clamp(tune0.w, 2.0, 16.0);
        const float strength = saturate(mbAmount * (velLen - 1.0) * 0.12);
        const float2 step = (prevUv - uv) * (strength / taps);

        float3 acc = cur;
        float  n   = 1.0;
        [unroll(16)]
        for (int k = 1; k < 16; ++k) {
            if (k >= taps) break;
            acc += ws_sample(t0, s0, clamp(uv + step * k, 0.0, 1.0));
            n += 1.0;
        }
        cur = acc / n;
    }

    // ---- temporal resolve -------------------------------------------------
    const float2 histUv = uv + cb0[8].xy;
    float3 hist = ws_sample(t1, s1, histUv);

    // neighbourhood box of the history, widened by the current pixel so a
    // fast change is never clamped away completely
    float3 lo = hist;
    float3 hi = hist;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float3 c = ws_sample(t1, s1, histUv + float2(x, y) * texel);
            lo = min(lo, c);
            hi = max(hi, c);
        }
    }
    lo = min(lo, cur);
    hi = max(hi, cur);
    hist = clamp(hist, lo, hi);

    float weight = saturate(tune0.x);
    weight *= valid ? 1.0 : 0.0;
    weight *= saturate(1.0 - velLen / max(tune1.y, 1.0));
    float3 outc = lerp(cur, hist, weight);

    // ---- clarity ----------------------------------------------------------
    const float clarity = saturate(tune0.y);
    if (clarity > 0.001) {
        float3 blur = 0.0;
        float  sum  = 0.0;
        [unroll] for (int y2 = -1; y2 <= 1; ++y2) {
            [unroll] for (int x2 = -1; x2 <= 1; ++x2) {
                const float w = (x2 == 0 && y2 == 0) ? 4.0
                              : ((abs(x2) + abs(y2) == 1) ? 2.0 : 1.0);
                blur += ws_sample(t0, s0, uv + float2(x2, y2) * texel) * w;
                sum  += w;
            }
        }
        blur /= sum;
        outc = lerp(outc, outc + (outc - blur) * 0.8, clarity);
    }

    outc = max(outc, 0.0);

    // mirror the engine's second target: sane colour plus a validity flag
    const bool broken = isnan(outc.x) || isnan(outc.y) || isnan(outc.z) ||
                        isinf(outc.x) || isinf(outc.y) || isinf(outc.z);
    const bool zero   = (outc.x == 0.0) && (outc.y == 0.0) && (outc.z == 0.0);
    o1 = float4(broken || zero ? 0.0 : outc, (broken || zero) ? 0.0 : 1.0);

    return float4(broken ? 0.0 : outc, 1.0);
}
)HLSL";

typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                        ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob**, ID3DBlob**);

ID3D11PixelShader* g_replacement = nullptr;
ID3D11Buffer*      g_tune = nullptr;
ID3D11PixelShader* g_family[32] = {};
int                g_familyCount = 0;
bool               g_enabled = true;
bool               g_tried = false;

struct Tune {
    float strength;
    float clarity;
    float motionBlur;
    float samples;
    float unused0;
    float motionRange;
    float unused1;
    float unused2;
};
Tune g_tuneData = {0.92f, 0.15f, 0.0f, 8.0f, 0.0f, 12.0f, 0.0f, 0.0f};

void upload_tune(ID3D11DeviceContext* ctx) {
    if (!g_tune || !ctx) return;
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (SUCCEEDED(ctx->Map(g_tune, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, &g_tuneData, sizeof(g_tuneData));
        ctx->Unmap(g_tune, 0);
    }
}

} // namespace

bool available() { return g_replacement != nullptr; }

void write_tune(ID3D11DeviceContext* ctx) { upload_tune(ctx); }

// The tuning buffer is written with the immediate context at draw time, so the
// caller does not need one here.
bool install(ID3D11Device* device, const wchar_t* log_dir) {
    (void)log_dir;
    if (g_tried) return g_replacement != nullptr;
    g_tried = true;
    if (!device) return false;

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler && log_dir) {
        std::wstring path(log_dir);
        path += L"\\d3dcompiler_47.dll";
        compiler = LoadLibraryW(path.c_str());
    }
    if (!compiler) {
        ws::log_write("taa: d3dcompiler_47.dll unavailable, keeping the engine TAA");
        return false;
    }
    auto compile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(compiler, "D3DCompile"));
    if (!compile) {
        ws::log_write("taa: D3DCompile missing, keeping the engine TAA");
        return false;
    }

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT hr = compile(kSource, std::strlen(kSource), "warmer_sun_taa", nullptr, nullptr,
                         "main", "ps_5_0", flags, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        ws::log_write("taa: compile failed (hr=0x%08lx) %s", hr,
                      errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        if (code) code->Release();
        return false;
    }

    hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                   &g_replacement);
    code->Release();
    if (errors) errors->Release();
    if (FAILED(hr) || !g_replacement) {
        ws::log_write("taa: CreatePixelShader failed (hr=0x%08lx)", hr);
        g_replacement = nullptr;
        return false;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Tune);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &g_tune))) {
        ws::log_write("taa: tuning buffer creation failed");
        g_replacement->Release();
        g_replacement = nullptr;
        return false;
    }

    ws::log_write("taa: replacement shader compiled (strength=%.2f clarity=%.2f blur=%.2f)",
                  g_tuneData.strength, g_tuneData.clarity, g_tuneData.motionBlur);
    return true;
}

void note_shader(ID3D11PixelShader* ps) {
    if (!ps) return;
    for (int i = 0; i < g_familyCount; ++i) {
        if (g_family[i] == ps) return;
    }
    if (g_familyCount < 32) {
        // Hold a reference: pointer comparisons against a released shader could
        // otherwise match a different object allocated at the same address.
        ps->AddRef();
        g_family[g_familyCount++] = ps;
    }
}

bool is_taa(ID3D11PixelShader* ps) {
    if (!ps || !g_replacement || !g_enabled) return false;
    for (int i = 0; i < g_familyCount; ++i) {
        if (g_family[i] == ps) return true;
    }
    return false;
}

ID3D11PixelShader* replacement() { return g_replacement; }
ID3D11Buffer*      tune_buffer() { return g_tune; }

void set_enabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }

void set_params(float strength, float clarity, float motion_blur,
                int mb_samples, float motion_range) {
    g_tuneData.strength = strength;
    g_tuneData.clarity = clarity;
    g_tuneData.motionBlur = motion_blur;
    g_tuneData.samples = static_cast<float>(mb_samples);
    g_tuneData.motionRange = motion_range;
}

void shutdown() {
    if (g_tune) { g_tune->Release(); g_tune = nullptr; }
    if (g_replacement) { g_replacement->Release(); g_replacement = nullptr; }
    for (int i = 0; i < g_familyCount; ++i) {
        if (g_family[i]) g_family[i]->Release();
        g_family[i] = nullptr;
    }
    g_familyCount = 0;
    g_tried = false;
}

} // namespace taa
} // namespace ws
