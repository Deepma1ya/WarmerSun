// WarmerSun - shared configuration, logging and utility layer.
#include "ws_common.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <shlobj.h>

namespace ws {
namespace {

std::mutex  g_logMutex;
std::wstring g_logPath;
bool         g_logOpen = false;

std::string format_args(const char* fmt, va_list args) {
    char stack[1024];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(stack, sizeof(stack), fmt, copy);
    va_end(copy);
    if (n < 0) return std::string();
    if (static_cast<size_t>(n) < sizeof(stack)) return std::string(stack, static_cast<size_t>(n));

    std::vector<char> heap(static_cast<size_t>(n) + 1);
    vsnprintf(heap.data(), heap.size(), fmt, args);
    return std::string(heap.data(), static_cast<size_t>(n));
}

std::wstring join(const std::wstring& dir, const wchar_t* name) {
    if (dir.empty()) return name;
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    out += name;
    return out;
}

bool file_exists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_writable(const std::wstring& dir) {
    std::wstring probe = join(dir, L".warmer_sun_write_test");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return std::wstring();
    std::wstring p(buf, n);
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
}

struct Ini {
    std::map<std::string, std::map<std::string, std::string>> sections;

    bool has(const std::string& section, const std::string& key) const {
        auto s = sections.find(section);
        if (s == sections.end()) return false;
        return s->second.find(key) != s->second.end();
    }
    std::string get(const std::string& section, const std::string& key,
                    const std::string& fallback = std::string()) const {
        auto s = sections.find(section);
        if (s == sections.end()) return fallback;
        auto k = s->second.find(key);
        return k == s->second.end() ? fallback : k->second;
    }
};

bool parse_ini(const std::wstring& path, Ini& out) {
    std::ifstream in(path.c_str());
    if (!in.is_open()) return false;

    std::string section = "warmersun";
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[') {
            size_t close = t.find(']');
            if (close != std::string::npos) section = lower(trim(t.substr(1, close - 1)));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(t.substr(0, eq)));
        std::string val = trim(t.substr(eq + 1));
        // strip an inline comment only when preceded by whitespace
        for (size_t i = 1; i < val.size(); ++i) {
            if ((val[i] == ';' || val[i] == '#') && (val[i - 1] == ' ' || val[i - 1] == '\t')) {
                val = trim(val.substr(0, i));
                break;
            }
        }
        out.sections[section][key] = val;
    }
    return true;
}

float f(const Ini& ini, const char* section, const char* key, float fallback) {
    std::string s = ini.get(section, key);
    if (s.empty()) return fallback;
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    return (end && end != s.c_str()) ? v : fallback;
}

int i(const Ini& ini, const char* section, const char* key, int fallback) {
    std::string s = ini.get(section, key);
    if (s.empty()) return fallback;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 0);
    return (end && end != s.c_str()) ? static_cast<int>(v) : fallback;
}

bool b(const Ini& ini, const char* section, const char* key, bool fallback) {
    std::string s = lower(ini.get(section, key));
    if (s.empty()) return fallback;
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 0);
    return (end && end != s.c_str()) ? v != 0 : fallback;
}

} // namespace

// ---------------------------------------------------------------------------
// strings
// ---------------------------------------------------------------------------

std::string trim(const std::string& s) {
    size_t a = 0, z = s.size();
    while (a < z && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (z > a && std::isspace(static_cast<unsigned char>(s[z - 1]))) --z;
    return s.substr(a, z - a);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

std::wstring module_dir(HMODULE module) {
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD n = GetModuleFileNameW(module, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return exe_dir();
    std::wstring p(buf, n);
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
}

std::wstring data_dir(HMODULE module) {
    std::wstring dir = module_dir(module);
    if (!dir.empty() && dir_writable(dir)) return dir;

    std::wstring out;
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                      static_cast<DWORD>(std::size(buffer)));
    if (n > 0 && n < std::size(buffer)) {
        out = std::wstring(buffer, n) + L"\\WarmerSun";
    }
    if (out.empty()) out = dir;
    CreateDirectoryW(out.c_str(), nullptr);
    return out;
}

std::wstring find_config(HMODULE module) {
    std::wstring beside = join(module_dir(module), L"WarmerSun.ini");
    if (file_exists(beside)) return beside;

    std::wstring exe = join(exe_dir(), L"WarmerSun.ini");
    if (file_exists(exe)) return exe;
    return beside; // canonical location even when missing
}

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

void log_init(HMODULE module) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logOpen) return;
    std::wstring dir = data_dir(module);
    g_logPath = join(dir, L"WarmerSun.log");
    std::ofstream out(g_logPath.c_str(), std::ios::app);
    if (out.is_open()) {
        g_logOpen = true;
        SYSTEMTIME st;
        GetLocalTime(&st);
        out << "\r\n=== WarmerSun session " << st.wYear << "-" << st.wMonth << "-" << st.wDay
            << " " << st.wHour << ":" << st.wMinute << ":" << st.wSecond << " ===\r\n";
    }
}

void log_shutdown() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logOpen = false;
    g_logPath.clear();
}

void trace_raw(const char* msg) {
    std::wstring path = data_dir(nullptr) + L"\\WarmerSun.trace";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
    WriteFile(h, "\r\n", 2, &written, nullptr);
    CloseHandle(h);
}

void log_write(const char* fmt, ...) {
    // A log call must never recurse: a failure inside the stream or the CRT
    // formatter would otherwise exhaust the stack while we are trying to report it.
    static thread_local int depth = 0;
    if (depth > 0) {
        OutputDebugStringA("[WarmerSun] recursive log suppressed\n");
        return;
    }
    ++depth;

    va_list args;
    va_start(args, fmt);
    std::string line = format_args(fmt, args);
    va_end(args);

    OutputDebugStringA("[WarmerSun] ");
    OutputDebugStringA(line.c_str());
    OutputDebugStringA("\n");

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logOpen || g_logPath.empty()) { --depth; return; }
    std::ofstream out(g_logPath.c_str(), std::ios::app);
    if (!out.is_open()) { --depth; return; }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
    out << stamp << line << "\r\n";
    --depth;
}

// ---------------------------------------------------------------------------
// presets
// ---------------------------------------------------------------------------

void apply_builtin_preset(char preset, ColorParams& c) {
    switch (preset) {
    case 'A':
        c = ColorParams{};
        c.temperature = 3.0f;  c.tint = 0.0f;
        c.contrast = 1.02f;    c.saturation = 1.03f; c.vibrance = 0.10f;
        c.highlights = -0.06f; c.shadows = 0.03f;    c.whites = 0.00f; c.blacks = 0.00f;
        c.nightExposure = 0.25f; c.sunWarmth = 0.20f;
        break;
    case 'B':
        c = ColorParams{};
        c.temperature = 6.0f;  c.tint = 0.0f;
        c.contrast = 1.10f;    c.saturation = 1.10f; c.vibrance = 0.20f;
        c.highlights = -0.14f; c.shadows = 0.08f;    c.whites = 0.02f; c.blacks = -0.04f;
        c.nightExposure = 0.35f; c.sunWarmth = 0.35f;
        break;
    case 'C':
        c = ColorParams{};
        c.temperature = 2.0f;  c.tint = 0.0f;
        c.contrast = 1.04f;    c.saturation = 1.02f; c.vibrance = 0.12f;
        c.highlights = -0.08f; c.shadows = 0.10f;    c.whites = 0.00f; c.blacks = -0.02f;
        c.nightExposure = 0.70f; c.sunWarmth = 0.15f;
        break;
    case 'D':
    default:
        c = ColorParams{};
        break;
    }
}

void apply_builtin_dynamic(char preset, DynamicParams& d) {
    d = DynamicParams{};
    if (preset == 'C') { d.adaptSpeed = 0.7f; d.key = 0.20f; d.maxEV = 2.0f; }
    else if (preset == 'B') { d.adaptSpeed = 1.4f; d.key = 0.15f; }
    else if (preset == 'A') { d.adaptSpeed = 1.0f; d.key = 0.18f; d.maxEV = 1.0f; }
}

void apply_builtin_sharpen(char preset, SharpenParams& s) {
    s = SharpenParams{};
    s.edges = 0.40f;
    if (preset == 'A') s.sharpness = 0.15f;
    else if (preset == 'B') s.sharpness = 0.35f;
    else if (preset == 'C') s.sharpness = 0.25f;
    else s.sharpness = 0.20f;
}

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------

int parse_key(const std::string& text) {
    std::string t = lower(trim(text));
    if (t.empty()) return 0;
    if (t.size() == 1) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    if (t.rfind("vk_", 0) == 0) t = t.substr(3);
    if (t.size() >= 2 && t[0] == 'f' && std::isdigit(static_cast<unsigned char>(t[1]))) {
        int n = std::atoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }
    struct Named { const char* name; int vk; };
    static const Named named[] = {
        {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE},
        {"ins", VK_INSERT}, {"insert", VK_INSERT},
        {"del", VK_DELETE}, {"delete", VK_DELETE},
        {"home", VK_HOME}, {"end", VK_END},
        {"pgup", VK_PRIOR}, {"pageup", VK_PRIOR},
        {"pgdn", VK_NEXT}, {"pagedown", VK_NEXT},
        {"pause", VK_PAUSE}, {"scroll", VK_SCROLL},
        {"space", VK_SPACE}, {"tab", VK_TAB},
        {"numpad0", VK_NUMPAD0}, {"numpad1", VK_NUMPAD1}, {"numpad2", VK_NUMPAD2},
        {"numpad3", VK_NUMPAD3}, {"numpad4", VK_NUMPAD4}, {"numpad5", VK_NUMPAD5},
        {"numpad6", VK_NUMPAD6}, {"numpad7", VK_NUMPAD7}, {"numpad8", VK_NUMPAD8},
        {"numpad9", VK_NUMPAD9},
        {"none", 0}, {"off", 0}, {"disabled", 0},
    };
    for (const auto& n : named) {
        if (t == n.name) return n.vk;
    }
    char* end = nullptr;
    long v = std::strtol(t.c_str(), &end, 0);
    return (end && end != t.c_str()) ? static_cast<int>(v) : 0;
}

bool load_config(const std::wstring& path, Config& out, char presetOverride) {
    Ini ini;
    bool present = parse_ini(path, ini);
    if (!present) {
        log_write("config not found at %ls; using built-in preset %c",
                  path.c_str(), out.preset);
        apply_builtin_preset(out.preset, out.color);
        apply_builtin_dynamic(out.preset, out.dynamic);
        apply_builtin_sharpen(out.preset, out.sharpen);
        return false;
    }

    out.sourcePath = path;

    std::string p = ini.get("warmersun", "preset", std::string(1, out.preset));
    if (!p.empty()) out.preset = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
    if (presetOverride) out.preset = presetOverride;

    out.enabled = !b(ini, "warmersun", "disable", false) &&
                  b(ini, "warmersun", "enabled", out.enabled);
    out.safeMode = b(ini, "warmersun", "safemode", out.safeMode);
    out.verbose = b(ini, "warmersun", "verbose", out.verbose);
    out.dumpFrames = b(ini, "warmersun", "dumpframes", out.dumpFrames);
    out.dumpAfter = i(ini, "warmersun", "dumpafter", out.dumpAfter);
    out.dumpCount = i(ini, "warmersun", "dumpcount", out.dumpCount);

    std::string kToggle = ini.get("warmersun", "keytoggle");
    std::string kPreset = ini.get("warmersun", "keypreset");
    std::string kReload = ini.get("warmersun", "keyreload");
    if (!kToggle.empty()) out.keyToggle = parse_key(kToggle);
    if (!kPreset.empty()) out.keyPreset = parse_key(kPreset);
    if (!kReload.empty()) out.keyReload = parse_key(kReload);

    // Start from the built-in values for the active preset, then let the ini
    // override them. This means a partial preset section is still valid.
    apply_builtin_preset(out.preset, out.color);
    apply_builtin_dynamic(out.preset, out.dynamic);
    apply_builtin_sharpen(out.preset, out.sharpen);

    char section[32];
    snprintf(section, sizeof(section), "preset.%c", out.preset);
    std::string sec = lower(section);
    if (ini.sections.find(sec) == ini.sections.end()) sec = "color";

    out.color.temperature   = f(ini, sec.c_str(), "temperature",   out.color.temperature);
    out.color.tint          = f(ini, sec.c_str(), "tint",          out.color.tint);
    out.color.preExposure   = f(ini, sec.c_str(), "preexposure",   out.color.preExposure);
    out.color.exposure      = f(ini, sec.c_str(), "exposure",      out.color.exposure);
    out.color.preContrast   = f(ini, sec.c_str(), "precontrast",   out.color.preContrast);
    out.color.contrast      = f(ini, sec.c_str(), "contrast",      out.color.contrast);
    out.color.saturation    = f(ini, sec.c_str(), "saturation",    out.color.saturation);
    out.color.vibrance      = f(ini, sec.c_str(), "vibrance",      out.color.vibrance);
    out.color.highlights    = f(ini, sec.c_str(), "highlights",    out.color.highlights);
    out.color.shadows       = f(ini, sec.c_str(), "shadows",       out.color.shadows);
    out.color.whites        = f(ini, sec.c_str(), "whites",        out.color.whites);
    out.color.blacks        = f(ini, sec.c_str(), "blacks",        out.color.blacks);
    out.color.nightExposure = f(ini, sec.c_str(), "nightexposure", out.color.nightExposure);
    out.color.sunWarmth     = f(ini, sec.c_str(), "sunwarmth",     out.color.sunWarmth);

    out.dynamic.enabled    = b(ini, sec.c_str(), "dynamic", out.dynamic.enabled);
    out.dynamic.adaptSpeed = f(ini, sec.c_str(), "adaptspeed", out.dynamic.adaptSpeed);
    out.dynamic.key        = f(ini, sec.c_str(), "key", out.dynamic.key);
    out.dynamic.minEV      = f(ini, sec.c_str(), "minev", out.dynamic.minEV);
    out.dynamic.maxEV      = f(ini, sec.c_str(), "maxev", out.dynamic.maxEV);

    out.sharpen.enabled   = b(ini, sec.c_str(), "sharpen", out.sharpen.enabled);
    out.sharpen.sharpness = f(ini, sec.c_str(), "sharpness", out.sharpen.sharpness);
    out.sharpen.edges     = f(ini, sec.c_str(), "sharpenedges", out.sharpen.edges);

    return true;
}

bool save_config(const std::wstring& path, const Config& cfg) {
    std::ofstream out(path.c_str(), std::ios::trunc);
    if (!out.is_open()) return false;

    char buf[64];
    auto f = [&](const char* key, float value) {
        snprintf(buf, sizeof(buf), "%.3f", value);
        out << key << "=" << buf << "\n";
    };
    auto i = [&](const char* key, int value) { out << key << "=" << value << "\n"; };

    out << "[WarmerSun]\n";
    out << "Enabled=" << (cfg.enabled ? 1 : 0) << "\n";
    out << "Preset=" << cfg.preset << "\n";
    out << "KeyToggle=F10\nKeyPreset=F11\nKeyReload=F12\n";
    out << "DumpFrames=" << (cfg.dumpFrames ? 1 : 0) << "\n\n";

    for (char p : {'A', 'B', 'C', 'D'}) {
        ColorParams col = cfg.color;
        DynamicParams dyn = cfg.dynamic;
        SharpenParams sh = cfg.sharpen;
        if (p != cfg.preset) {
            apply_builtin_preset(p, col);
            apply_builtin_dynamic(p, dyn);
            apply_builtin_sharpen(p, sh);
        }
        out << "[Preset." << p << "]\n";
        f("Temperature", col.temperature);
        f("Tint", col.tint);
        f("PreExposure", col.preExposure);
        f("Exposure", col.exposure);
        f("PreContrast", col.preContrast);
        f("Contrast", col.contrast);
        f("Saturation", col.saturation);
        f("Vibrance", col.vibrance);
        f("Highlights", col.highlights);
        f("Shadows", col.shadows);
        f("Whites", col.whites);
        f("Blacks", col.blacks);
        f("NightExposure", col.nightExposure);
        f("SunWarmth", col.sunWarmth);
        i("Dynamic", dyn.enabled ? 1 : 0);
        f("AdaptSpeed", dyn.adaptSpeed);
        f("Key", dyn.key);
        f("MinEV", dyn.minEV);
        f("MaxEV", dyn.maxEV);
        i("Sharpen", sh.enabled ? 1 : 0);
        f("Sharpness", sh.sharpness);
        f("SharpenEdges", sh.edges);
        out << "\n";
    }
    return out.good();
}

} // namespace ws





