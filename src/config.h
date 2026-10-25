#pragma once

#include <limits>
#include <string>

#include "log.h"

namespace mhwdlss {

// The filter of the sharpening pass after DLSS (see sharpen.h).
enum class Sharpener {
    NVSharpen,  // NVIDIA Image Scaling's NVSharpen
    Vanilla,    // the filter of the game's original DLSS sharpening (DLISP's "DLSS Sharpen")
    Rcas,       // AMD FidelityFX RCAS
};

// NVIDIA Reflex Low Latency mode (see reflex.h).
enum class ReflexMode {
    Off,
    On,
    Boost,  // On, plus the graphics card kept at full clock speed
};

// No Ultra Performance: the game clamps its render scale to 0.5 and above.
enum class QualityMode {
    DLAA,
    Quality,
    Balanced,
    Performance,
    Custom,
};

struct Config {
    bool enabled = true;
    LogLevel logLevel = LogLevel::Info;

    QualityMode mode = QualityMode::DLAA;
    float customScale = 1.0f;  // render scale used when mode == Custom
    // DLSS render preset (0 = default, otherwise NVSDK_NGX_DLSS_Hint_Render_Preset value, K = 11).
    unsigned int preset = 11;
    std::wstring dllPath;  // nvngx_dlss.dll to use instead of the driver's (folder or file)
    bool autoExposure = false;
    // Sharpening after DLSS, 0..1; negative = the game's DLSS Sharpness option.
    float sharpness = 0.33f;
    Sharpener sharpener = Sharpener::NVSharpen;

    ReflexMode reflex = ReflexMode::On;
    float frameLimit = 0.0f;  // [Advanced] frames per second through Reflex, 0 = no limit

    // Input conventions, measured for this game (see game.cpp and ngx_shim.cpp).
    bool depthInverted = true;
    bool mvJittered = false;
    // The game's dense velocity is the clip-space difference times the render size, without the
    // 0.5 and the y flip of a pixel offset. Measured with tools/motion_check.py on frames dumped
    // while walking and turning.
    float mvScaleX = 0.5f;
    float mvScaleY = -0.5f;
    // The camera jitter value h moves the image by (h.x / 2, -h.y / 2) pixels.
    float jitterScaleX = 0.5f;
    float jitterScaleY = -0.5f;
    bool forceJitter = true;      // keep the TAA jitter on while DLSS is active
    bool fullPixelJitter = true;  // double the game's half-pixel jitter while DLSS is active
    // Hold the game's 2x2 reflection history swap (the scene's mSSLRGBufferJitter) off while DLSS
    // runs (see game.cpp).
    bool reflectionFix = true;
    bool cutReset = true;         // reset the DLSS history on camera cuts (the game does not)
    // Jitter phases while DLSS runs: -1 = from the render scale (8 / scale^2, at least the game's
    // 20), 0 = the game's 20-entry sequence, otherwise that many.
    int jitterPhases = -1;
    // Texture mip bias while DLSS runs, in mip levels: NaN = log2(render scale) - 1 (NVIDIA's
    // recommendation), 0 = the game's own. The game has banks for 0, -1 and -log2(2.5 .. 5).
    float mipBias = std::numeric_limits<float>::quiet_NaN();
    // Under Wine: DXVK_NVAPI_GPU_ARCH for the plugin's NGX init. Empty = from the card's PCI device
    // id (a value from the launch options is kept), "OFF" = leave the variable alone, otherwise one
    // of dxvk-nvapi's names (TU100, GA100, AD100, GB200, ...), upper case.
    std::string gpuArch;
    int hdr = -1;                // -1 = from the game / input format, 0 or 1 = force
    float exposureScale = 1.0f;
    float preExposure = 1.0f;

    // Diagnostics.
    bool ngxLogging = false;
    bool verboseFrames = false;
    bool bypass = false;          // DLAA mode: keep the game's TAA result (A/B comparison)
    bool debugProbe = false;      // log statistics of the DLSS inputs after a reload (D3D12)
    int debugDumpFrames = 0;      // dump raw input frames + motion + depth + jitter after a reload (D3D12)
    float debugDumpDelay = 5.0f;  // seconds between the reload and the dump
    bool debugDumpOutput = false; // write one DLSS output frame to output.bin after a reload (D3D11)
    bool debugFixedJitter = false; // replace the camera jitter with a constant (camera units)
    float debugJitterX = 0.0f;
    float debugJitterY = 0.0f;
    bool debugNgxFail = false;    // act as if NGX could not be initialised (the Proton failure)
    bool debugNoNvidia = false;   // act as if the PC had no NVIDIA graphics card
    bool latencyStats = false;    // log NVIDIA's latency report every few seconds (Reflex markers)
    bool reflexSleep = true;      // call NvAPI_D3D_Sleep each frame (0: markers only)

    // Render scale implied by the mode (1.0 for DLAA).
    float RenderScale() const;
    // Sharpening strength in effect, 0..1 (from the ini or the game's option).
    float Sharpness() const;
    // The render preset as NVIDIA names it (K for 11), '-' for the default.
    char PresetLetter() const { return preset ? static_cast<char>('A' + preset - 1) : '-'; }
};

extern Config g_config;

// Loads <iniPath>. Missing keys keep their defaults; a missing file is written out with defaults.
void LoadConfig(const std::wstring& iniPath);

// Reloads the ini loaded by LoadConfig when its timestamp changed. Returns true on reload.
bool ReloadConfigIfChanged();

// The game's graphics_option.ini, read for its DLSS_Sharpness value. The game rewrites the file
// when graphics options are applied; the reload returns true when the value changed.
void LoadGameOptions(const std::wstring& path);
bool ReloadGameOptionsIfChanged();
float GameSharpness();
// The game's Frame Rate option: the cap in frames per second, 0 for No Limit.
int GameFrameRate();

const char* QualityModeName(QualityMode mode);
const char* SharpenerName(Sharpener sharpener);
const char* ReflexModeName(ReflexMode mode);

}  // namespace mhwdlss
