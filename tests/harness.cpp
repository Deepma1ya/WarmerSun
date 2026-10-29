// WarmerSun test harness.
//
// Creates a windowed D3D11 swap chain in a folder that contains the WarmerSun
// dxgi.dll. d3d11.dll resolves "dxgi.dll" from the application folder, so the
// proxy is loaded exactly as it would be in the game. The harness renders a
// known test pattern, presents 160 frames and writes the pre-present back
// buffer to harness_before.dds; the mod writes its graded frame to
// WarmerSun_frame_00000.dds. tests/check_frames.py compares the two.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static const UINT kWidth = 640;
static const UINT kHeight = 360;

static const char* kShader = R"HLSL(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = i.uv;

    // horizontal ramp, deliberately warm
    float3 c = float3(uv.x, uv.x * 0.86, uv.x * 0.68);

    // colour bars along the bottom
    if (uv.y > 0.80)
    {
        float x = uv.x * 7.0;
        if (x < 1.0)      c = float3(0.85, 0.10, 0.10);
        else if (x < 2.0) c = float3(0.10, 0.75, 0.15);
        else if (x < 3.0) c = float3(0.10, 0.25, 0.90);
        else if (x < 4.0) c = float3(0.90, 0.85, 0.10);
        else if (x < 5.0) c = float3(0.10, 0.80, 0.85);
        else if (x < 6.0) c = float3(0.85, 0.15, 0.75);
        else              c = float3(0.92, 0.92, 0.92);
    }

    // checker patch for sharpening
    if (uv.x > 0.40 && uv.x < 0.60 && uv.y > 0.40 && uv.y < 0.60)
    {
        float2 g = floor(uv * float2(160.0, 90.0));
        c = fmod(g.x + g.y, 2.0) < 1.0 ? float3(0.12, 0.12, 0.12) : float3(0.88, 0.88, 0.88);
    }

    // bright blob to exercise highlight roll-off
    float d = distance(uv, float2(0.86, 0.20));
    c += float3(1.0, 0.96, 0.88) * saturate(1.0 - d * 6.0) * 0.9;

    return float4(saturate(c), 1.0);
}
)HLSL";

typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                        ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                        ID3DBlob**);

static bool write_dds(ID3D11Device* device, ID3D11DeviceContext* context,
                      ID3D11Texture2D* texture, const wchar_t* path) {
    D3D11_TEXTURE2D_DESC d = {};
    texture->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return false;

    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) return false;
    context->CopyResource(staging, texture);
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        staging->Release();
        return false;
    }

    bool ok = false;
    FILE* fp = _wfopen(path, L"wb");
    if (fp) {
        unsigned char h[128] = {};
        memcpy(h, "DDS ", 4);
        unsigned u = 124; memcpy(h + 4, &u, 4);
        unsigned flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000;
        memcpy(h + 8, &flags, 4);
        memcpy(h + 12, &d.Height, 4);
        memcpy(h + 16, &d.Width, 4);
        unsigned pitch = d.Width * 4; memcpy(h + 20, &pitch, 4);
        memcpy(h + 28, &d.MipLevels, 4);
        unsigned pf = 0x1 | 0x40; memcpy(h + 80, &pf, 4);
        unsigned bits = 32; memcpy(h + 88, &bits, 4);
        unsigned masks[4] = {0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000};
        memcpy(h + 92, masks, 16);
        unsigned caps = 0x1000; memcpy(h + 108, &caps, 4);
        fwrite(h, 1, 128, fp);
        for (UINT y = 0; y < d.Height; ++y) {
            fwrite(static_cast<unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                   1, pitch, fp);
        }
        fclose(fp);
        ok = true;
    }
    context->Unmap(staging, 0);
    staging->Release();
    return ok;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = L"WarmerSunHarness";
    RegisterClassExW(&wc);

    RECT r = {0, 0, kWidth, kHeight};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"WarmerSun harness",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance,
                                nullptr);
    if (!hwnd) return 1;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = kWidth;
    sd.BufferDesc.Height = kHeight;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &sd, &swap,
        &device, nullptr, &context);
    if (FAILED(hr)) {
        MessageBoxW(hwnd, L"D3D11CreateDeviceAndSwapChain failed", L"WarmerSun harness", MB_ICONERROR);
        return 1;
    }

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) {
        MessageBoxW(hwnd, L"d3dcompiler_47.dll missing", L"WarmerSun harness", MB_ICONERROR);
        return 1;
    }
    auto compile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(compiler, "D3DCompile"));

    ID3DBlob* errors = nullptr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (FAILED(compile(kShader, strlen(kShader), "harness", nullptr, nullptr, "VSMain", "vs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errors)) ||
        FAILED(compile(kShader, strlen(kShader), "harness", nullptr, nullptr, "PSMain", "ps_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors))) {
        if (errors) MessageBoxA(hwnd, static_cast<const char*>(errors->GetBufferPointer()),
                                "shader error", MB_ICONERROR);
        return 1;
    }

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    ID3D11Texture2D* back = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    ID3D11RenderTargetView* rtv = nullptr;
    device->CreateRenderTargetView(back, nullptr, &rtv);

    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
    context->RSSetViewports(1, &vp);

    bool dumpedBefore = false;
    for (int frame = 0; frame < 180; ++frame) {
        float clear[4] = {0, 0, 0, 1};
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->ClearRenderTargetView(rtv, clear);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->Draw(3, 0);

        if (frame == 150 && !dumpedBefore) {
            dumpedBefore = write_dds(device, context, back, L"harness_before.dds");
        }

        swap->Present(1, 0);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) frame = 1000;
        }
        Sleep(1);
    }

    rtv->Release();
    back->Release();
    ps->Release();
    vs->Release();
    psBlob->Release();
    vsBlob->Release();
    context->Release();
    device->Release();
    swap->Release();
    return dumpedBefore ? 0 : 2;
}

