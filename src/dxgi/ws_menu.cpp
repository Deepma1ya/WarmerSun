// WarmerSun - in-game settings panel.
#include "ws_menu.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "ws_taa.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace ws {
namespace {

HWND      g_hwnd = nullptr;
WNDPROC   g_origProc = nullptr;
bool      g_visible = false;
bool      g_initialized = false;
float     g_scale = 1.0f;
std::wstring g_configPath;
std::string  g_status;

// Virtual cursor: the game clips/locks the hardware cursor, so the panel is
// driven by raw mouse deltas instead of the absolute position.
float g_cursorX = 0.0f;
float g_cursorY = 0.0f;
bool  g_mouseDown = false;

void toggle_impl();

LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_visible) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        switch (msg) {
        case WM_INPUT: {
            RAWINPUT raw = {};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) == size &&
                raw.header.dwType == RIM_TYPEMOUSE) {
                g_cursorX += static_cast<float>(raw.data.mouse.lLastX);
                g_cursorY += static_cast<float>(raw.data.mouse.lLastY);
                RECT rc = {};
                GetClientRect(hwnd, &rc);
                const float w = static_cast<float>(rc.right - rc.left);
                const float h = static_cast<float>(rc.bottom - rc.top);
                if (g_cursorX < 0.0f) g_cursorX = 0.0f;
                if (g_cursorY < 0.0f) g_cursorY = 0.0f;
                if (g_cursorX > w) g_cursorX = w;
                if (g_cursorY > h) g_cursorY = h;
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
            g_mouseDown = true;
            return 0;
        case WM_LBUTTONUP:
            g_mouseDown = false;
            return 0;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_CHAR:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            if (msg == WM_SYSKEYDOWN && wp == VK_F4) break; // keep Alt+F4
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        default:
            break;
        }
    }

    return CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
}

void toggle_impl() {
    if (!g_initialized) return;
    g_visible = !g_visible;
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_visible;
    if (g_visible) {
        RECT rc = {};
        GetClientRect(g_hwnd, &rc);
        g_cursorX = (rc.right - rc.left) * 0.5f;
        g_cursorY = (rc.bottom - rc.top) * 0.5f;
        g_mouseDown = false;
        ClipCursor(nullptr);
        g_status.clear();
    }
}

void preset_buttons(Config& cfg) {
    const char* labels[4] = {"A  Natural", "B  Cinematic", "C  Night", "D  WarmerSun"};
    const char letters[4] = {'A', 'B', 'C', 'D'};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        const bool active = cfg.preset == letters[i];
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.98f, 0.55f, 0.22f, 1.0f));
        if (ImGui::Button(labels[i], ImVec2(112.0f * g_scale, 0.0f))) {
            cfg.preset = letters[i];
            apply_builtin_preset(letters[i], cfg.color);
            apply_builtin_dynamic(letters[i], cfg.dynamic);
            apply_builtin_sharpen(letters[i], cfg.sharpen);
            g_status = std::string("Loaded preset ") + letters[i];
        }
        if (active) ImGui::PopStyleColor();
    }
}

void draw_panel(Config& cfg) {
    ImGui::SetNextWindowSize(ImVec2(520.0f * g_scale, 600.0f * g_scale),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(48.0f * g_scale, 48.0f * g_scale),
                            ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("WarmerSun", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (!open) {
        ImGui::End();
        toggle_impl();
        return;
    }

    ImGui::TextColored(ImVec4(0.98f, 0.66f, 0.32f, 1.0f), "WarmerSun");
    ImGui::SameLine();
    ImGui::TextDisabled("lighting grade - ETS2 / ATS");
    ImGui::Separator();

    ImGui::Checkbox("Grade enabled", &cfg.enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(F10 toggles)");

    preset_buttons(cfg);
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Colour")) {
            ImGui::Spacing();
            ImGui::SliderFloat("Temperature", &cfg.color.temperature, -100.0f, 100.0f, "%.1f");
            ImGui::SliderFloat("Tint", &cfg.color.tint, -100.0f, 100.0f, "%.1f");
            ImGui::SliderFloat("Saturation", &cfg.color.saturation, 0.0f, 2.0f, "%.3f");
            ImGui::SliderFloat("Vibrance", &cfg.color.vibrance, -1.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("Contrast", &cfg.color.contrast, 0.5f, 2.0f, "%.3f");
            ImGui::SliderFloat("Pre-contrast", &cfg.color.preContrast, 0.5f, 2.0f, "%.3f");
            ImGui::SliderFloat("Sun warmth", &cfg.color.sunWarmth, 0.0f, 1.0f, "%.3f");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Tone")) {
            ImGui::Spacing();
            ImGui::SliderFloat("Exposure", &cfg.color.exposure, -3.0f, 3.0f, "%.2f EV");
            ImGui::SliderFloat("Pre-exposure", &cfg.color.preExposure, -3.0f, 3.0f, "%.2f EV");
            ImGui::SliderFloat("Highlights", &cfg.color.highlights, -1.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("Shadows", &cfg.color.shadows, -1.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("Whites", &cfg.color.whites, -1.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("Blacks", &cfg.color.blacks, -1.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("Night exposure", &cfg.color.nightExposure, 0.0f, 2.0f, "%.2f EV");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Adaptation")) {
            ImGui::Spacing();
            ImGui::Checkbox("Dynamic exposure", &cfg.dynamic.enabled);
            ImGui::SliderFloat("Adapt speed", &cfg.dynamic.adaptSpeed, 0.1f, 5.0f, "%.2f /s");
            ImGui::SliderFloat("Key", &cfg.dynamic.key, 0.05f, 0.5f, "%.3f");
            ImGui::SliderFloat("Min EV", &cfg.dynamic.minEV, -4.0f, 0.0f, "%.2f");
            ImGui::SliderFloat("Max EV", &cfg.dynamic.maxEV, 0.0f, 4.0f, "%.2f");
            ImGui::TextDisabled("Night lift is driven by average scene luminance.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Detail")) {
            ImGui::Spacing();
            ImGui::Checkbox("Sharpen", &cfg.sharpen.enabled);
            ImGui::SliderFloat("Sharpness", &cfg.sharpen.sharpness, 0.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("Sharpen edges", &cfg.sharpen.edges, 0.0f, 1.0f, "%.3f");
            ImGui::TextDisabled("Contrast adaptive sharpening, applied in linear light.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AA / Motion")) {
            ImGui::Spacing();
            ImGui::Checkbox("Temporal AA (replaces the engine pass)", &cfg.taa.enabled);
            ImGui::SliderFloat("TAA strength", &cfg.taa.strength, 0.0f, 1.0f, "%.3f");
            ImGui::SliderFloat("TAA clarity", &cfg.taa.clarity, 0.0f, 1.0f, "%.3f");
            ImGui::Spacing();
            ImGui::SliderFloat("Motion blur", &cfg.taa.motionBlur, 0.0f, 4.0f, "%.2f");
            ImGui::SliderInt("Blur samples", &cfg.taa.mbSamples, 2, 16);
            ImGui::SliderFloat("Blur cut-off", &cfg.taa.motionRange, 2.0f, 48.0f, "%.1f px");
            ImGui::TextDisabled("History is dropped above the cut-off speed, so fast");
            ImGui::TextDisabled("camera moves never smear.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info")) {
            ImGui::Spacing();
            ImGui::Text("End        show / hide this panel");
            ImGui::Text("F10        toggle the grade");
            ImGui::Text("F11        cycle preset");
            ImGui::Text("F12        reload WarmerSun.ini");
            ImGui::Separator();
            ImGui::TextDisabled("WarmerSun v0.3 - no DRM, no network, no telemetry.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ws::taa::set_enabled(cfg.taa.enabled);
    ws::taa::set_params(cfg.taa.strength, cfg.taa.clarity, cfg.taa.motionBlur,
                        cfg.taa.mbSamples, cfg.taa.motionRange);
    if (ImGui::Button("Save to WarmerSun.ini", ImVec2(220.0f * g_scale, 0.0f))) {
        g_status = save_config(g_configPath, cfg) ? "Saved." : "Could not write the ini.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(120.0f * g_scale, 0.0f))) {
        ImGui::End();
        toggle_impl();
        return;
    }
    if (!g_status.empty()) {
        ImGui::TextColored(ImVec4(0.62f, 0.82f, 0.62f, 1.0f), "%s", g_status.c_str());
    }

    ImGui::End();
}

} // namespace

bool menu_init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context,
               const std::wstring& configPath) {
    if (g_initialized) return true;
    if (!hwnd || !device || !context) return false;

    g_hwnd = hwnd;
    g_configPath = configPath;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    g_scale = 1.0f;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef UINT(WINAPI* PFN_GetDpiForWindow)(HWND);
    auto getDpi = user32 ? reinterpret_cast<PFN_GetDpiForWindow>(
                               GetProcAddress(user32, "GetDpiForWindow"))
                         : nullptr;
    if (getDpi) {
        UINT dpi = getDpi(hwnd);
        if (dpi) g_scale = static_cast<float>(dpi) / 96.0f;
    }
    if (g_scale < 1.0f) g_scale = 1.0f;

    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf",
                                                16.0f * g_scale);
    if (!font) io.Fonts->AddFontDefault();

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8.0f;
    st.FrameRounding = 4.0f;
    st.GrabRounding = 4.0f;
    st.WindowBorderSize = 1.0f;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.050f, 0.060f, 0.96f);
    st.Colors[ImGuiCol_Border] = ImVec4(0.22f, 0.23f, 0.26f, 0.90f);
    st.Colors[ImGuiCol_TitleBg] = ImVec4(0.080f, 0.090f, 0.100f, 1.00f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    st.Colors[ImGuiCol_Button] = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.26f, 0.30f, 1.00f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.98f, 0.55f, 0.22f, 1.00f);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.98f, 0.62f, 0.30f, 1.00f);
    st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.72f, 0.42f, 1.00f);
    st.Colors[ImGuiCol_CheckMark] = ImVec4(0.98f, 0.62f, 0.30f, 1.00f);
    st.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.21f, 0.24f, 1.00f);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.29f, 0.33f, 1.00f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(0.98f, 0.55f, 0.22f, 1.00f);
    st.ScaleAllSizes(g_scale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);


    SetLastError(0);
    g_origProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MenuWndProc)));
    const DWORD procError = GetLastError();
    g_initialized = true;
    log_write("menu: ready (End to open) hwnd=%p orig=%p err=%lu", hwnd,
              reinterpret_cast<void*>(g_origProc), procError);
    return true;
}

void menu_shutdown() {
    if (!g_initialized) return;
    if (g_hwnd && g_origProc && IsWindow(g_hwnd)) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origProc));
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_initialized = false;
    g_visible = false;
    g_hwnd = nullptr;
    g_origProc = nullptr;
}

bool menu_visible() { return g_visible; }
void menu_toggle() { toggle_impl(); }

void menu_render(ID3D11DeviceContext* context, Config& cfg) {
    if (!g_initialized || !g_visible || !context) return;
    ClipCursor(nullptr);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(g_cursorX, g_cursorY);
    io.AddMouseButtonEvent(0, g_mouseDown);
    ImGui::NewFrame();
    draw_panel(cfg);
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

} // namespace ws





