// WarmerSun - present-level D3D11 grading pipeline.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "ws_common.h"

namespace ws {

// Re-applies the D3D11 vtable hooks; the runtime replaces them during init.
void refresh_shader_hooks();

class Renderer {
public:
    bool init(IDXGISwapChain* swapChain);
    void shutdown();

    // Returns true when the frame was modified and the swap chain needs to
    // present the new contents. False means "present untouched".
    bool present(IDXGISwapChain* swapChain, UINT presentFlags);

    void notify_resize();

    // Applies the grade to the back buffer. Called either from the tonemap
    // draw hook (before the UI) or from Present as a fallback.
    bool grade_now();
    bool graded() const { return graded_; }

    bool ready() const { return ready_; }
    void set_config(const Config& cfg);
    const Config& config() const { return config_; }
    void reload_config();

    // Hotkey handling; call once per frame.
    void tick_input();

private:
    bool ensure_size();
    void release_size_dependent();
    bool create_pipeline();
    void destroy_pipeline();
    void update_constants(float dt, bool dynamicEnabled);
    void dump_backbuffer(ID3D11Texture2D* backbuffer);

    bool compile_shaders();
    bool create_states();
    bool create_size_dependent();

    ID3D11Device*        device_  = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain*      swap_    = nullptr;
    HWND                 hwnd_    = nullptr;

    UINT        width_     = 0;
    UINT        height_    = 0;
    DXGI_FORMAT format_    = DXGI_FORMAT_UNKNOWN;
    bool        inputEncoded_  = true;
    bool        outputEncoded_ = true;

    // size dependent
    ID3D11Texture2D*          scene_     = nullptr;
    ID3D11ShaderResourceView* sceneSRV_  = nullptr;
    ID3D11Texture2D*          backTexture_ = nullptr;
    ID3D11RenderTargetView*   backRTV_   = nullptr;
    ID3D11Texture2D*          lum64_     = nullptr;
    ID3D11RenderTargetView*   lum64RTV_  = nullptr;
    ID3D11ShaderResourceView* lum64SRV_  = nullptr;
    ID3D11Texture2D*          lum1_      = nullptr;
    ID3D11RenderTargetView*   lum1RTV_   = nullptr;
    ID3D11ShaderResourceView* lum1SRV_   = nullptr;

    // persistent
    ID3D11Texture2D*          exposure_[2]    = {nullptr, nullptr};
    ID3D11RenderTargetView*   exposureRTV_[2] = {nullptr, nullptr};
    ID3D11ShaderResourceView* exposureSRV_[2] = {nullptr, nullptr};
    int                       exposureIndex_  = 0;

    ID3D11VertexShader* vsFull_   = nullptr;
    ID3D11PixelShader*  psLum64_  = nullptr;
    ID3D11PixelShader*  psLum1_   = nullptr;
    ID3D11PixelShader*  psAdapt_  = nullptr;
    ID3D11PixelShader*  psFinal_  = nullptr;

    ID3D11SamplerState* samplerLin_ = nullptr;
    ID3D11SamplerState* samplerPt_  = nullptr;
    ID3D11Buffer*       constants_  = nullptr;
    ID3D11BlendState*   blend_      = nullptr;
    ID3D11RasterizerState* raster_  = nullptr;
    ID3D11DepthStencilState* depth_ = nullptr;

    bool  ready_        = false;
    bool  graded_       = false;
    bool  failed_       = false;
    ULONGLONG lastTick_ = 0;
    unsigned long long frame_ = 0;
    unsigned dumped_    = 0;

    Config config_;
    bool   keysDown_[3] = {false, false, false};
};

extern Renderer g_renderer;

} // namespace ws





