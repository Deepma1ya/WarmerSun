// WarmerSun - TAA shader smoke test.
//
// Creates a D3D11 device, builds the same resource set the engine's TAA pass
// binds, renders one full-screen triangle with the WarmerSun resolver and
// reads the result back. This exercises the driver's shader JIT on this
// machine, which is where a bad shader shows up as a crash rather than an
// error code.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdarg>
#include <vector>

#include "ws_taa.h"

// The resolver only needs logging; print instead of writing a file.
namespace ws {
void log_write(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}
} // namespace ws

static const char* kVS = R"HLSL(
struct VS_OUT { float4 pos : SV_POSITION; float4 uv : TEXCOORD0; };
VS_OUT main(uint id : SV_VertexID) {
    VS_OUT o;
    float2 p = float2((id == 1) ? 3.0 : -1.0, (id == 2) ? 3.0 : -1.0);
    o.pos = float4(p, 0.0, 1.0);
    o.uv  = float4(p * 0.5 + 0.5, 0.0, 0.0);
    o.uv.y = 1.0 - o.uv.y;
    return o;
}
)HLSL";

static const UINT kSize = 256;

static ID3D11Texture2D* make_tex(ID3D11Device* dev, DXGI_FORMAT fmt, const void* data,
                                 UINT pitch, ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = kSize;
    d.Height = kSize;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = data;
    sd.SysMemPitch = pitch;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, data ? &sd : nullptr, &tex))) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = fmt;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(tex, &sv, srv))) { tex->Release(); return nullptr; }
    return tex;
}

int main() {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = {};
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 1,
                                 D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
        printf("device creation failed\n");
        return 1;
    }
    printf("device ok, feature level 0x%x\n", fl);
    ID3D11InfoQueue* iq = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&iq))) {
        iq->SetMuteDebugOutput(FALSE);
        printf("debug layer attached\n");
    } else {
        printf("debug layer unavailable\n");
    }
    struct Drain {
        ID3D11InfoQueue* q;
        void operator()(const char* where) {
            if (!q) return;
            UINT64 n = q->GetNumStoredMessages();
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0;
                q->GetMessage(i, nullptr, &len);
                if (!len) continue;
                std::vector<char> buf(len);
                D3D11_MESSAGE* msg = (D3D11_MESSAGE*)buf.data();
                if (SUCCEEDED(q->GetMessage(i, msg, &len)))
                    printf("  [%s] %s\n", where, msg->pDescription);
            }
            q->ClearStoredMessages();
        }
    } drain{iq};

    if (!ws::taa::install(dev, nullptr) || !ws::taa::replacement()) {
        printf("shader install failed\n");
        return 1;
    }
    printf("resolver installed\n");
    ws::taa::set_params(0.92f, 0.15f, 0.35f, 8, 12.0f);

    // colour textures: a smooth gradient in real floats
    std::vector<float> colour(kSize * kSize * 4);
    for (UINT y = 0; y < kSize; ++y) {
        for (UINT x = 0; x < kSize; ++x) {
            const UINT i = (y * kSize + x) * 4;
            colour[i + 0] = 0.2f + 0.6f * x / kSize;
            colour[i + 1] = 0.2f + 0.6f * y / kSize;
            colour[i + 2] = 0.25f;
            colour[i + 3] = 1.0f;
        }
    }

    // uint textures: encoded depth ~10 and a tolerance nibble
    std::vector<unsigned int> depth(kSize * kSize, 9822u);
    std::vector<unsigned int> packed(kSize * kSize, 9822u | (0x15u << 4));
    std::vector<unsigned int> flags(kSize * kSize, 0u);

    ID3D11ShaderResourceView *s0 = nullptr, *s1 = nullptr, *s3 = nullptr, *s4 = nullptr, *s15 = nullptr;
    ID3D11Texture2D* t0 = make_tex(dev, DXGI_FORMAT_R32G32B32A32_FLOAT, colour.data(),
                                   kSize * 16, &s0);
    ID3D11Texture2D* t1 = make_tex(dev, DXGI_FORMAT_R32G32B32A32_FLOAT, colour.data(),
                                   kSize * 16, &s1);
    ID3D11Texture2D* t3 = make_tex(dev, DXGI_FORMAT_R32_UINT, depth.data(), kSize * 4, &s3);
    ID3D11Texture2D* t4 = make_tex(dev, DXGI_FORMAT_R32_UINT, packed.data(), kSize * 4, &s4);
    ID3D11Texture2D* t15 = make_tex(dev, DXGI_FORMAT_R32_UINT, flags.data(), kSize * 4, &s15);
    if (!t0 || !t1 || !t3 || !t4 || !t15) { printf("texture creation failed\n"); return 1; }
    printf("resources ok\n");

    // render targets
    D3D11_TEXTURE2D_DESC rd = {};
    rd.Width = kSize; rd.Height = kSize; rd.MipLevels = 1; rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; rd.SampleDesc.Count = 1;
    rd.Usage = D3D11_USAGE_DEFAULT;
    rd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* rt[2] = {nullptr, nullptr};
    ID3D11RenderTargetView* rtv[2] = {nullptr, nullptr};
    for (int i = 0; i < 2; ++i) {
        if (FAILED(dev->CreateTexture2D(&rd, nullptr, &rt[i]))) { printf("rt fail\n"); return 1; }
        if (FAILED(dev->CreateRenderTargetView(rt[i], nullptr, &rtv[i]))) { printf("rtv fail\n"); return 1; }
    }

    // constant buffers
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 224;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    std::vector<float> cb(56, 0.0f);
    cb[44] = 1.0f; cb[45] = 1.0f;         // cb0[11].xy
    cb[46] = 1.0f / kSize; cb[47] = 1.0f / kSize;
    cb[36] = 1.0f; cb[37] = 1.0f / kSize; cb[38] = 1.0f / kSize;
    D3D11_SUBRESOURCE_DATA cbd = {};
    cbd.pSysMem = cb.data();
    ID3D11Buffer* cbuf = nullptr;
    if (FAILED(dev->CreateBuffer(&bd, &cbd, &cbuf))) { printf("cb fail\n"); return 1; }

    ID3D11Buffer* tune = ws::taa::tune_buffer();
    ws::taa::write_tune(ctx);

    // vertex shader
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    typedef HRESULT(WINAPI* PFN)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                 LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<PFN>(GetProcAddress(comp, "D3DCompile"));
    ID3DBlob* vsb = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(compile(kVS, strlen(kVS), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &err))) {
        printf("vs compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "");
        return 1;
    }
    ID3D11VertexShader* vs = nullptr;
    HRESULT vsHr = dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                           nullptr, &vs);
    printf("vertex shader hr=0x%08lx ptr=%p\n", vsHr, (void*)vs);
    if (!vs) return 1;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* smp = nullptr;
    dev->CreateSamplerState(&sd, &smp);

    D3D11_RASTERIZER_DESC rdesc = {};
    rdesc.FillMode = D3D11_FILL_SOLID;
    rdesc.CullMode = D3D11_CULL_NONE;
    rdesc.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rstate = nullptr;
    dev->CreateRasterizerState(&rdesc, &rstate);
    ctx->RSSetState(rstate);

    D3D11_TEXTURE2D_DESC stageDesc = rd;
    stageDesc.BindFlags = 0;
    stageDesc.Usage = D3D11_USAGE_STAGING;
    stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stage = nullptr;
    dev->CreateTexture2D(&stageDesc, nullptr, &stage);

    D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)kSize, (float)kSize, 0.0f, 1.0f};

    // Does the plumbing work at all? Clear, read back, then swap in a trivial
    // pixel shader before trusting the resolver result.
    {
        const float clear[4] = {0.11f, 0.22f, 0.33f, 1.0f};
        ctx->ClearRenderTargetView(rtv[0], clear);
        ctx->CopyResource(stage, rt[0]);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
            const float* px = (const float*)((const char*)m.pData + (kSize / 2 * m.RowPitch) + kSize / 2 * 16);
            printf("clear readback: %.3f %.3f %.3f\n", px[0], px[1], px[2]);
            ctx->Unmap(stage, 0);
        }
    }

    // sanity: a trivial pixel shader must reach the target
    {
        const char* src = "float4 main(float4 p : SV_POSITION, float4 uv : TEXCOORD0,"
                          " out float4 o1 : SV_Target1) : SV_Target0"
                          "{ float4 c = float4(0.7,0.4,0.1,1); o1 = c; return c; }";
        ID3DBlob* b = nullptr; ID3DBlob* e = nullptr;
        HRESULT hr = compile(src, strlen(src), "t", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &e);
        printf("trivial ps compile hr=0x%08lx %s\n", hr, e ? (const char*)e->GetBufferPointer() : "");
        ID3D11PixelShader* tps = nullptr;
        if (b) { HRESULT ph = dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &tps); printf("trivial ps create hr=0x%08lx ptr=%p\n", ph, (void*)tps); b->Release(); }
        ctx->OMSetRenderTargets(2, rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(tps, nullptr, 0);
        ctx->Draw(3, 0);
        ctx->Flush();
        drain("after trivial draw");
        ctx->CopyResource(stage, rt[0]);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
            const float* px = (const float*)((const char*)m.pData + (kSize / 2 * m.RowPitch) + kSize / 2 * 16);
            printf("trivial ps readback: %.3f %.3f %.3f\n", px[0], px[1], px[2]);
            ctx->Unmap(stage, 0);
        }
        if (tps) tps->Release();
    }

    // two passes: one with a zeroed matrix (the safe path the game may hit)
    // and one with plausible values so the reprojection code actually runs.
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            for (int i = 0; i < 16; ++i) cb[i] = (i % 5 == 0) ? 0.5f : 0.05f;
            cb[4] = cb[5] = cb[6] = cb[7] = 0.0f;
            cb[0] = 1.0f; cb[5] = 1.0f; cb[10] = 1.0f; cb[15] = 1.0f;
            ctx->UpdateSubresource(cbuf, 0, nullptr, cb.data(), 0, 0);
        }
        ctx->OMSetRenderTargets(2, rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ID3D11ShaderResourceView* srvs[16] = {};
        srvs[0] = s0; srvs[1] = s1; srvs[3] = s3; srvs[4] = s4; srvs[15] = s15;
        ctx->PSSetShaderResources(0, 16, srvs);
        ID3D11SamplerState* samplers[2] = {smp, smp};
        ctx->PSSetSamplers(0, 2, samplers);
        ctx->PSSetConstantBuffers(0, 1, &cbuf);
        ctx->PSSetConstantBuffers(1, 1, &tune);
        ctx->PSSetShader(ws::taa::replacement(), nullptr, 0);
        ctx->Draw(3, 0);
        ctx->Flush();

        ctx->CopyResource(stage, rt[0]);
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &map))) {
            const float* px = (const float*)((const char*)map.pData +
                                             (kSize / 2 * map.RowPitch) +
                                             (kSize / 2 * 16));
            float r = px[0], g = px[1], b = px[2];
            printf("pass %d centre pixel: %.4f %.4f %.4f\n", pass, r, g, b);
            if (!(r == r) || !(g == g) || !(b == b)) { printf("PASS %d NaN\n", pass); return 1; }
            ctx->Unmap(stage, 0);
        }
    }

    printf("SMOKE TEST PASS: shader ran on the driver without a fault\n");
    return 0;
}








