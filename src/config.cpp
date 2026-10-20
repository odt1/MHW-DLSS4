#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

namespace mhwdlss {

Config g_config;

namespace {

constexpr wchar_t kDefaultIni[] =
    L"; MHW-DLSS4 - DLSS 4 for Monster Hunter: World\n"
    L"; Turn on \"NVIDIA DLSS\" in the game's graphics options; this plugin replaces what it runs.\n"
    L"; The file is re-read while the game runs.\n"
    L"\n"
    L"[General]\n"
    L"; 0 disables the plugin entirely (needs a game restart).\n"
    L"Enabled=1\n"
    L"; Debug, Info, Warn or Error.\n"
    L"LogLevel=Info\n"
    L"\n"
    L"[DLSS]\n"
    L"; DLAA, Quality, Balanced, Performance or Custom.\n"
    L"; DLAA renders at native resolution and replaces the game's anti-aliasing.\n"
    L"; The other modes render at a lower resolution and let DLSS scale the frame up where the\n"
    L"; game's own upscaler (DLISP) used to sit, after post-processing. The game never renders\n"
    L"; below half resolution, so there is no Ultra Performance mode.\n"
    L"Mode=DLAA\n"
    L"; Render scale used when Mode=Custom, 0.5 to 1.0.\n"
    L"RenderScale=1.0\n"
    L"; DLSS render preset: Default, J, K, L or M.\n"
    L"Preset=K\n"
    L"; Sharpening after DLSS, 0 (off) to 1. Game = the in-game \"DLSS Sharpness\" slider, same\n"
    L"; scale. Current DLSS versions do not sharpen, so the plugin runs a sharpening filter on the\n"
    L"; DLSS output.\n"
    L"Sharpness=0.33\n"
    L"; The filter:\n"
    L";   NVSharpen  NVIDIA Image Scaling (1 here is NVIDIA's 0.3; above that it gets harsh).\n"
    L";   Vanilla    the filter of the game's original DLSS sharpening. 1 is the strongest the\n"
    L";              original allowed, about NVSharpen's 0.8.\n"
    L";   RCAS       AMD FidelityFX RCAS, the filter of versions 0.2 and 0.3. About as strong as\n"
    L";              NVSharpen up to 0.5, stronger above.\n"
    L"Sharpener=NVSharpen\n"
    L"; Which nvngx_dlss.dll to use. Empty: the newest DLSS the NVIDIA driver has downloaded, or\n"
    L"; nvngx_dlss.dll in the MHW-DLSS4 folder next to this file when that one is newer.\n"
    L"; Otherwise a folder holding nvngx_dlss.dll, or the DLL file itself under any name. Relative\n"
    L"; paths start at the game folder (the one with MonsterHunterWorld.exe). Examples:\n"
    L";   DLLPath=nativePC\\plugins\\MHW-DLSS4\n"
    L";   DLLPath=nativePC\\plugins\\MHW-DLSS4\\nvngx_dlss_310.4.dll\n"
    L";   DLLPath=..\\Cyberpunk 2077\\bin\\x64\n"
    L";   DLLPath=D:\\DLSS\\310.9\\nvngx_dlss.dll\n"
    L"; Read when the game starts.\n"
    L"DLLPath=\n"
    L"; Let DLSS estimate exposure itself (presets L and M always do).\n"
    L"AutoExposure=0\n"
    L"; DLAA mode only: show the game's TAA instead of DLSS, for comparisons.\n"
    L"Bypass=0\n"
    L"\n"
    L"[Reflex]\n"
    L"; NVIDIA Reflex Low Latency: Off, On or Boost. On lets the driver start each frame just in\n"
    L"; time for the graphics card, and makes a frame rate cap set in the NVIDIA Control Panel or\n"
    L"; NVIDIA App wait at the start of the frame, where it adds the least input lag. Boost also\n"
    L"; keeps the graphics card at full clock speed: a little less input lag, more power.\n"
    L"; Works with NVIDIA DLSS on or off.\n"
    L"LowLatency=On\n";

std::wstring g_iniPath;
unsigned long long g_iniTime = 0;

std::wstring g_gameOptionsPath;
unsigned long long g_gameOptionsTime = 0;
float g_gameSharpness = 0.0f;
int g_gameFrameRate = 0;

unsigned long long LastWrite(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
           data.ftLastWriteTime.dwLowDateTime;
}

constexpr const wchar_t* kSections[] = {L"General", L"DLSS", L"Reflex", L"Advanced"};

std::wstring ReadString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    // The key's own section first. A key added under another section of the file (easy to do when
    // appending to the end) still counts.
    constexpr wchar_t kMissing[] = L"\x01";
    wchar_t buf[1024];
    GetPrivateProfileStringW(section, key, kMissing, buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
    for (const wchar_t* other : kSections) {
        if (wcscmp(buf, kMissing) != 0) break;
        if (_wcsicmp(other, section) != 0)
            GetPrivateProfileStringW(other, key, kMissing, buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
    }
    std::wstring s = wcscmp(buf, kMissing) == 0 ? def : buf;
    // Strip a trailing comment: ; or # at the start or after a space, so that paths like
    // D:\Games#2 survive.
    for (size_t i = 0; i < s.size(); ++i) {
        if ((s[i] == L';' || s[i] == L'#') && (i == 0 || std::iswspace(s[i - 1]))) {
            s.resize(i);
            break;
        }
    }
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) ++start;
    s = s.substr(start);
    // Windows drops quotes around a whole value itself, but not when a comment follows.
    if (s.size() >= 2 && (s.front() == L'"' || s.front() == L'\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return s;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool ReadBool(const std::wstring& ini, const wchar_t* section, const wchar_t* key, bool def) {
    auto s = Lower(ReadString(ini, section, key, def ? L"1" : L"0"));
    if (s == L"1" || s == L"true" || s == L"on" || s == L"yes") return true;
    if (s == L"0" || s == L"false" || s == L"off" || s == L"no") return false;
    return def;
}

float ReadFloat(const std::wstring& ini, const wchar_t* section, const wchar_t* key, float def) {
    auto s = ReadString(ini, section, key, L"");
    if (s.empty()) return def;
    try {
        return std::stof(s);
    } catch (...) {
        return def;
    }
}

}  // namespace

float Config::RenderScale() const {
    switch (mode) {
        case QualityMode::DLAA: return 1.0f;
        case QualityMode::Quality: return 2.0f / 3.0f;
        case QualityMode::Balanced: return 0.58f;
        case QualityMode::Performance: return 0.5f;
        // The game does not render below half resolution.
        case QualityMode::Custom: return std::clamp(customScale, 0.5f, 1.0f);
    }
    return 1.0f;
}

float Config::Sharpness() const { return std::clamp(sharpness < 0.0f ? g_gameSharpness : sharpness, 0.0f, 1.0f); }

const char* QualityModeName(QualityMode mode) {
    switch (mode) {
        case QualityMode::DLAA: return "DLAA";
        case QualityMode::Quality: return "Quality";
        case QualityMode::Balanced: return "Balanced";
        case QualityMode::Performance: return "Performance";
        case QualityMode::Custom: return "Custom";
    }
    return "?";
}

const char* SharpenerName(Sharpener sharpener) {
    switch (sharpener) {
        case Sharpener::NVSharpen: return "NVSharpen";
        case Sharpener::Vanilla: return "Vanilla";
        case Sharpener::Rcas: return "RCAS";
    }
    return "?";
}

const char* ReflexModeName(ReflexMode mode) {
    switch (mode) {
        case ReflexMode::Off: return "Off";
        case ReflexMode::On: return "On";
        case ReflexMode::Boost: return "Boost";
    }
    return "?";
}

void LoadConfig(const std::wstring& ini) {
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::ofstream out(ini, std::ios::binary);
        for (const wchar_t* p = kDefaultIni; *p; ++p) out.put(static_cast<char>(*p));  // plain ASCII
    }

    Config c;
    c.enabled = ReadBool(ini, L"General", L"Enabled", c.enabled);

    auto level = Lower(ReadString(ini, L"General", L"LogLevel", L"info"));
    if (level == L"debug") c.logLevel = LogLevel::Debug;
    else if (level == L"warn") c.logLevel = LogLevel::Warn;
    else if (level == L"error") c.logLevel = LogLevel::Error;
    else c.logLevel = LogLevel::Info;

    auto mode = Lower(ReadString(ini, L"DLSS", L"Mode", L"dlaa"));
    if (mode == L"quality") c.mode = QualityMode::Quality;
    else if (mode == L"balanced") c.mode = QualityMode::Balanced;
    // UltraPerformance was accepted by 0.1; the game cannot go below Performance's scale.
    else if (mode == L"performance" || mode == L"ultraperformance") c.mode = QualityMode::Performance;
    else if (mode == L"custom") c.mode = QualityMode::Custom;
    else c.mode = QualityMode::DLAA;
    c.customScale = ReadFloat(ini, L"DLSS", L"RenderScale", c.customScale);

    auto sharpness = Lower(ReadString(ini, L"DLSS", L"Sharpness", L""));
    if (sharpness == L"game")
        c.sharpness = -1.0f;
    else if (!sharpness.empty())
        c.sharpness = std::clamp(ReadFloat(ini, L"DLSS", L"Sharpness", c.sharpness), 0.0f, 1.0f);
    auto sharpener = Lower(ReadString(ini, L"DLSS", L"Sharpener", L""));
    // DLSS was 0.6's name for Vanilla.
    if (sharpener == L"vanilla" || sharpener == L"dlss") c.sharpener = Sharpener::Vanilla;
    else if (sharpener == L"rcas") c.sharpener = Sharpener::Rcas;

    auto preset = Lower(ReadString(ini, L"DLSS", L"Preset", L"k"));
    if (preset.size() == 1 && preset[0] >= L'a' && preset[0] <= L'o')
        c.preset = static_cast<unsigned int>(preset[0] - L'a') + 1;  // A=1 ... K=11, M=13
    else
        c.preset = 0;

    c.dllPath = ReadString(ini, L"DLSS", L"DLLPath", L"");
    c.autoExposure = ReadBool(ini, L"DLSS", L"AutoExposure", c.autoExposure);
    c.bypass = ReadBool(ini, L"DLSS", L"Bypass", c.bypass);

    auto lowLatency = Lower(ReadString(ini, L"Reflex", L"LowLatency", L"on"));
    if (lowLatency == L"off" || lowLatency == L"0") c.reflex = ReflexMode::Off;
    else if (lowLatency == L"boost") c.reflex = ReflexMode::Boost;
    else c.reflex = ReflexMode::On;

    // [Advanced]: developer keys, not in the default ini (docs/technical.md lists them). Inis of 0.8.1
    // and older still have the measured conventions there, with the default values.

    c.depthInverted = ReadBool(ini, L"Advanced", L"DepthInverted", c.depthInverted);
    c.mvJittered = ReadBool(ini, L"Advanced", L"MVJittered", c.mvJittered);
    // MVScaleX/Y of 0.1 and 0.2 held the wrong 1, 1 in every ini written by those versions; they are
    // ignored, the measured scale lives under new names.
    c.mvScaleX = ReadFloat(ini, L"Advanced", L"MotionScaleX", c.mvScaleX);
    c.mvScaleY = ReadFloat(ini, L"Advanced", L"MotionScaleY", c.mvScaleY);
    c.jitterScaleX = ReadFloat(ini, L"Advanced", L"JitterScaleX", c.jitterScaleX);
    c.jitterScaleY = ReadFloat(ini, L"Advanced", L"JitterScaleY", c.jitterScaleY);
    c.forceJitter = ReadBool(ini, L"Advanced", L"ForceJitter", c.forceJitter);
    c.fullPixelJitter = ReadBool(ini, L"Advanced", L"FullPixelJitter", c.fullPixelJitter);
    c.reflectionFix = ReadBool(ini, L"Advanced", L"ReflectionFix", c.reflectionFix);
    c.cutReset = ReadBool(ini, L"Advanced", L"CutReset", c.cutReset);
    c.jitterPhases = std::clamp(static_cast<int>(ReadFloat(ini, L"Advanced", L"JitterPhases", -1.0f)), -1, 256);
    auto mipBias = Lower(ReadString(ini, L"Advanced", L"MipBias", L"auto"));
    if (mipBias == L"off") c.mipBias = 0.0f;
    else if (mipBias != L"auto") c.mipBias = std::clamp(ReadFloat(ini, L"Advanced", L"MipBias", 0.0f), -3.0f, 0.0f);
    c.hdr = static_cast<int>(ReadFloat(ini, L"Advanced", L"HDR", static_cast<float>(c.hdr)));
    c.exposureScale = ReadFloat(ini, L"Advanced", L"ExposureScale", c.exposureScale);
    c.preExposure = ReadFloat(ini, L"Advanced", L"PreExposure", c.preExposure);

    c.ngxLogging = ReadBool(ini, L"Advanced", L"NGXLogging", c.ngxLogging);
    c.verboseFrames = ReadBool(ini, L"Advanced", L"VerboseFrames", c.verboseFrames);
    c.debugProbe = ReadBool(ini, L"Advanced", L"DebugProbe", c.debugProbe);
    c.debugDumpFrames = static_cast<int>(ReadFloat(ini, L"Advanced", L"DebugDumpFrames", 0.0f));
    c.debugDumpDelay = std::clamp(ReadFloat(ini, L"Advanced", L"DebugDumpDelay", c.debugDumpDelay), 0.0f, 60.0f);
    c.debugDumpOutput = ReadBool(ini, L"Advanced", L"DebugDumpOutput", c.debugDumpOutput);
    c.debugFixedJitter = ReadBool(ini, L"Advanced", L"DebugFixedJitter", c.debugFixedJitter);
    c.debugJitterX = ReadFloat(ini, L"Advanced", L"DebugJitterX", c.debugJitterX);
    c.debugJitterY = ReadFloat(ini, L"Advanced", L"DebugJitterY", c.debugJitterY);
    c.debugNgxFail = ReadBool(ini, L"Advanced", L"DebugNgxFail", c.debugNgxFail);
    c.debugNoNvidia = ReadBool(ini, L"Advanced", L"DebugNoNvidia", c.debugNoNvidia);
    c.latencyStats = ReadBool(ini, L"Advanced", L"LatencyStats", c.latencyStats);
    // Reflex frame rate cap: measured to add the same input lag as the game's own cap and the
    // driver's (with LowLatency on), so it is only kept for measurements.
    c.frameLimit = std::clamp(ReadFloat(ini, L"Advanced", L"FrameLimit", c.frameLimit), 0.0f, 1000.0f);
    c.reflexSleep = ReadBool(ini, L"Advanced", L"ReflexSleep", c.reflexSleep);

    g_config = c;
    g_iniPath = ini;
    g_iniTime = LastWrite(ini);
}

bool ReloadConfigIfChanged() {
    if (g_iniPath.empty()) return false;
    if (LastWrite(g_iniPath) == g_iniTime) return false;
    LoadConfig(g_iniPath);
    return true;
}

void LoadGameOptions(const std::wstring& path) {
    g_gameOptionsPath = path;
    g_gameOptionsTime = LastWrite(path);
    g_gameSharpness = std::clamp(ReadFloat(path, L"GraphicsOption", L"DLSS_Sharpness", 0.0f), 0.0f, 1.0f);
    // 30, 60, 90, 120, 144, 165, 240 or No Limit.
    g_gameFrameRate = std::max(0, static_cast<int>(ReadFloat(path, L"GraphicsOption", L"FrameRate", 0.0f)));
}

bool ReloadGameOptionsIfChanged() {
    if (g_gameOptionsPath.empty() || LastWrite(g_gameOptionsPath) == g_gameOptionsTime) return false;
    float before = g_gameSharpness;
    LoadGameOptions(g_gameOptionsPath);
    return g_gameSharpness != before;
}

float GameSharpness() { return g_gameSharpness; }

int GameFrameRate() { return g_gameFrameRate; }

}  // namespace mhwdlss
