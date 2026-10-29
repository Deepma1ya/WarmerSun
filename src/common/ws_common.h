// WarmerSun - shared configuration, logging and utility layer.
// Used by both proxy DLLs (dxgi.dll and dinput8.dll).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace ws {

// ---------------------------------------------------------------------------
// Tunables for the grade. Ranges match the documented slider ranges.
// ---------------------------------------------------------------------------
struct ColorParams {
    float temperature   = 9.0f;   // -100..100   positive = warmer
    float tint          = 1.0f;   // -100..100   positive = magenta
    float preExposure   = 0.0f;   // EV, applied before contrast
    float exposure      = 0.0f;   // EV, applied after contrast
    float preContrast   = 1.0f;   // power curve around 0.18 pivot
    float contrast      = 1.06f;  // power curve around 0.18 pivot
    float saturation    = 1.07f;  // 0..2
    float vibrance      = 0.18f;  // -1..1
    float highlights    = -0.12f; // -1..1
    float shadows       = 0.07f;  // -1..1
    float whites        = 0.02f;  // -1..1
    float blacks        = -0.03f; // -1..1
    float nightExposure = 0.45f;  // EV added in darkness
    float sunWarmth     = 0.40f;  // 0..1 highlight warmth (signature look)
};

struct DynamicParams {
    bool  enabled    = true;
    float adaptSpeed = 1.0f;   // 1/s
    float key        = 0.16f;  // target middle grey (linear)
    float minEV      = -2.0f;
    float maxEV      = 1.5f;
};

struct SharpenParams {
    bool  enabled   = true;
    float sharpness = 0.20f;   // 0..1 peak strength
    float edges     = 0.40f;   // 0..1 edge bias
};

struct Config {
    bool  enabled      = true;
    bool  safeMode     = false;  // reads "Disable" in the ini
    bool  verbose      = true;
    bool  dumpFrames   = false;
    int   dumpAfter    = 120;    // present count before dumping
    int   dumpCount    = 1;
    char  preset       = 'D';

    ColorParams   color;
    DynamicParams dynamic;
    SharpenParams sharpen;

    // virtual-key codes for hotkeys (0 = unbound)
    int keyToggle = VK_F10;
    int keyPreset = VK_F11;
    int keyReload = VK_F12;

    // Path this config was loaded from (empty when defaults are in use).
    std::wstring sourcePath;
};

// ---------------------------------------------------------------------------
// Paths / logging
// ---------------------------------------------------------------------------

// Directory that contains the calling module (never ends with a separator).
std::wstring module_dir(HMODULE module);

// Directory where WarmerSun may write (module dir when writable, otherwise
// %LOCALAPPDATA%\WarmerSun). Created on demand.
std::wstring data_dir(HMODULE module);

// Opens WarmerSun.log. Safe to call more than once.
void log_init(HMODULE module);
void log_shutdown();
void log_write(const char* fmt, ...);

// Append-only trace that uses no CRT formatting. Safe before log_init and
// useful when a logging call itself is under suspicion.
void trace_raw(const char* msg);

// Looks for WarmerSun.ini next to the module, then next to the process exe.
std::wstring find_config(HMODULE module);

// Parses the ini. Missing keys keep their defaults. Returns false only when
// the file exists but could not be read.
bool load_config(const std::wstring& path, Config& out, char presetOverride = 0);

// Writes the active preset plus the three built-in presets back to the ini.
bool save_config(const std::wstring& path, const Config& cfg);

// Applies the built-in preset letter ('A'..'D') to the params.
void apply_builtin_preset(char preset, ColorParams& color);
void apply_builtin_dynamic(char preset, DynamicParams& dyn);
void apply_builtin_sharpen(char preset, SharpenParams& sharpen);

// "F10", "VK_F10", "0x79" or a single character -> virtual key code.
int parse_key(const std::string& text);

// Small string helpers shared by the two modules.
std::string trim(const std::string& s);
std::string lower(std::string s);

} // namespace ws




