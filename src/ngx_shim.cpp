#include "ngx_shim.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "config.h"
#include "d3d_util.h"
#include "dlss_dll.h"
#include "game.h"
#include "log.h"
#include "memory.h"
#include "ngx_params.h"
#include "probe.h"
#include "reflex.h"
#include "sharpen.h"

namespace mhwdlss::ngx {

namespace {

using Microsoft::WRL::ComPtr;

// NGX is initialised with a project id. With the game's application id the driver serves its
// per-game DLSS 1 model over the air instead of the nvngx_dlss.dll we ship.
constexpr char kProjectId[] = "3f1e9a52-6c0b-4d7e-a8f4-5b2c1d9e7a60";
constexpr unsigned kFirstHandleId = 0x4d480001;
constexpr uint64_t kDetailedEvaluations = 3;
// Frames of another render size than the feature was created for that are skipped as leftovers
// of a resolution change; past this the size is taken as the new one.
constexpr unsigned kMaxStaleFrames = 8;

enum class Api { D3D11, D3D12 };
const char* ApiName(Api api) { return api == Api::D3D12 ? "D3D12" : "D3D11"; }

// Function table inside the game's static NGX 1.3 library. The exported NVSDK_NGX_* functions
// call through it; NVSDK_NGX_*_Init fills it from the driver's NGX 1.x core.
struct Table {
    void* init;
    void* shutdown;
    void* scratch;
    void* getParameters;
    void* create;
    void* evaluate;
    void* release;
};

using PfnShutdown = NVSDK_NGX_Result(NVSDK_CONV*)();
using PfnRelease = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

struct ApiState {
    Table* table = nullptr;  // live table in the game
    Table legacy{};          // NGX 1.x core entry points that were there before us
    Table shim{};
};

// NGX 1.x handles are { unsigned int Id }; the SDK keeps the type opaque.
struct ShimHandle {
    unsigned int id = 0;
};

// One DLSS feature of the current NGX and the settings it was created with.
struct DlssInstance {
    NVSDK_NGX_Handle* handle = nullptr;
    unsigned inWidth = 0, inHeight = 0, outWidth = 0, outHeight = 0;
    int flags = 0;
    unsigned preset = 0;
    bool subrects = false;
    bool failed = false;
};

// A feature the game created through the NGX 1.x API. SuperSampling is evaluated at the TAA
// position; ImageSignalProcessing (DLISP) where the game scales the frame to the output size.
struct Feature {
    ShimHandle handle;  // its address is what the game holds
    NVSDK_NGX_Feature id = NVSDK_NGX_Feature_Reserved_Unknown;
    Api api = Api::D3D12;
    NVSDK_NGX_Handle* legacy = nullptr;  // created by the NGX 1.x core before we were loaded
    DlssInstance dlss;
    // Render size the game created the feature for (0 = unknown). Frames still in flight from
    // before a resolution change are skipped instead of building a short-lived DLSS feature.
    unsigned expectedWidth = 0, expectedHeight = 0;
    unsigned staleFrames = 0;
    bool resetNext = false;
    uint64_t evaluations = 0;
    uint64_t resets = 0;  // since the last progress line in the log

    NVSDK_NGX_Handle* Handle() { return reinterpret_cast<NVSDK_NGX_Handle*>(&handle); }
    const char* Name() const { return id == NVSDK_NGX_Feature_SuperSampling ? "DLSS" : "DLISP"; }
};

struct Modern {
    bool attempted = false;
    bool ready = false;
    Api api = Api::D3D12;
    void* device = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
};

// In the upscaling modes DLSS runs where the game calls DLISP, after post-processing. The TAA
// position call passes the jittered frame through and leaves the rest of the inputs here.
struct UpscaleInputs {
    bool valid = false;
    d3d::Mirror12 motion12, depth12;
    d3d::Mirror11 motion11, depth11;
    void* motion = nullptr;
    void* depth = nullptr;
    unsigned x = 0, y = 0, width = 0, height = 0;
    float jitterX = 0, jitterY = 0;
    int reset = 0;
    uint64_t frame = 0;
};

std::recursive_mutex g_mutex;
ApiState g_api[2];
std::vector<std::unique_ptr<Feature>> g_features;
std::atomic<unsigned> g_nextHandleId{kFirstHandleId};

ShimParameters g_params;
NVSDK_NGX_Parameter* g_legacyParams = nullptr;
bool g_contextCreated = false;
bool g_fallbackChecked = false;

Modern g_modern;
UpscaleInputs g_upscale;
std::wstring g_dataDir;
std::wstring g_gameDir;
// The one folder NGX searches for nvngx_dlss.dll, picked at the first init.
std::wstring g_dlssFolder;
const wchar_t* g_dlssFolderPtr = nullptr;
bool g_loggedModules = false;
bool g_probeRequested = false;
bool g_dumpOutputRequested = false;
// The context was adopted after the game had set DLSS up through its NGX 1.x core (see OnUpdate).
bool g_adoptedLate = false;
// NGX could not be initialised and the game still has DLSS on: OnUpdate asks it for the render
// scale again, which the optimal settings callback then refuses (see there).
bool g_dlssOffPending = false;

ApiState& State(Api api) { return g_api[api == Api::D3D12 ? 1 : 0]; }

// ---------------------------------------------------------------------------------------------
// Parameter helpers that work on any NVSDK_NGX_Parameter.

unsigned GetUI(const NVSDK_NGX_Parameter* p, const char* name, unsigned def = 0) {
    unsigned v = def;
    return NVSDK_NGX_SUCCEED(p->Get(name, &v)) ? v : def;
}
int GetI(const NVSDK_NGX_Parameter* p, const char* name, int def = 0) {
    int v = def;
    return NVSDK_NGX_SUCCEED(p->Get(name, &v)) ? v : def;
}
void* GetResource(Api api, const NVSDK_NGX_Parameter* p, const char* name) {
    if (api == Api::D3D12) {
        ID3D12Resource* r = nullptr;
        return NVSDK_NGX_SUCCEED(p->Get(name, &r)) ? r : nullptr;
    }
    ID3D11Resource* r = nullptr;
    return NVSDK_NGX_SUCCEED(p->Get(name, &r)) ? r : nullptr;
}

d3d::TextureInfo Info(Api api, void* resource) {
    return api == Api::D3D12 ? d3d::Info(static_cast<ID3D12Resource*>(resource))
                             : d3d::Info(static_cast<ID3D11Resource*>(resource));
}

void* DeviceOf(Api api, void* cmd) {
    if (!cmd) return nullptr;
    if (api == Api::D3D12) {
        ComPtr<ID3D12Device> device;
        if (FAILED(static_cast<ID3D12GraphicsCommandList*>(cmd)->GetDevice(IID_PPV_ARGS(&device)))) return nullptr;
        return device.Get();  // the game keeps the device alive
    }
    ComPtr<ID3D11Device> device;
    static_cast<ID3D11DeviceContext*>(cmd)->GetDevice(&device);
    return device.Get();
}

Feature* FindFeature(const NVSDK_NGX_Handle* handle) {
    if (!handle) return nullptr;
    for (auto& f : g_features)
        if (f->Handle() == handle || (f->legacy && f->legacy == handle)) return f.get();
    return nullptr;
}

Feature* AddFeature(Api api, NVSDK_NGX_Feature id, NVSDK_NGX_Handle* legacy) {
    auto f = std::make_unique<Feature>();
    f->handle.id = g_nextHandleId++;
    f->id = id;
    f->api = api;
    f->legacy = legacy;
    g_features.push_back(std::move(f));
    return g_features.back().get();
}

// The rectangle the game renders into, from the Rect.* parameters (whole texture if unset).
struct Rect {
    unsigned x = 0, y = 0, width = 0, height = 0;
};
Rect RectFrom(const NVSDK_NGX_Parameter* p, const d3d::TextureInfo& texture) {
    Rect r{GetUI(p, "Rect.X"), GetUI(p, "Rect.Y"), GetUI(p, "Rect.W"), GetUI(p, "Rect.H")};
    if (!r.width || !r.height) {
        r.x = r.y = 0;
        r.width = texture.width;
        r.height = texture.height;
    }
    return r;
}

bool StaleFrame(Feature& f, unsigned width, unsigned height) {
    if (!f.expectedWidth || (width == f.expectedWidth && height == f.expectedHeight)) {
        f.staleFrames = 0;
        return false;
    }
    if (++f.staleFrames > kMaxStaleFrames) {
        // Not a leftover: the game renders at another size than it asked for (it clamps the
        // render scale, for one). Skipping would leave the frame unscaled for good.
        LOGW("{}: the game renders {}x{} instead of the {}x{} it created the feature for; using {}x{}", f.Name(), width,
             height, f.expectedWidth, f.expectedHeight, width, height);
        f.expectedWidth = width;
        f.expectedHeight = height;
        f.staleFrames = 0;
        return false;
    }
    if (f.staleFrames == 1)
        LOGD("{}: skipping {}x{} frames from before the switch to {}x{}", f.Name(), width, height, f.expectedWidth,
             f.expectedHeight);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Render scale handed to the game through DLSSOptimalSettingsCallback.

NVSDK_NGX_Result NVSDK_CONV OptimalSettingsCallback(NVSDK_NGX_Parameter* p) {
    if (!p) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::lock_guard lock(g_mutex);  // g_config is replaced under it on ini reloads
    // Without NGX the game gets no settings. It then sets render scale 1 and DLSS type 0, which
    // renders without a DLSS pass, and shows its DLSS option as unavailable (+0x2383ef0, +0x4431f0).
    // Otherwise it keeps rendering below the output size for a DLISP call that cannot upscale.
    if (g_modern.attempted && !g_modern.ready) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    unsigned w = GetUI(p, NVSDK_NGX_Parameter_Width);
    unsigned h = GetUI(p, NVSDK_NGX_Parameter_Height);
    float scale = g_config.RenderScale();
    unsigned rw = w, rh = h;
    if (scale < 0.999f) {
        rw = std::max(1u, static_cast<unsigned>(std::lround(w * scale)));
        rh = std::max(1u, static_cast<unsigned>(std::lround(h * scale)));
    }
    p->Set(NVSDK_NGX_Parameter_OutWidth, rw);
    p->Set(NVSDK_NGX_Parameter_OutHeight, rh);
    p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, rw);
    p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, rh);
    p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, rw);
    p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, rh);
    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    // NGX 1.x: 1 = DLSS. Below a scale of 1 the game also creates DLISP to reach the output size.
    p->Set("DLSSMode", 1);
    static unsigned lastW = 0, lastH = 0, lastRw = 0;
    if (w != lastW || h != lastH || rw != lastRw) {
        lastW = w;
        lastH = h;
        lastRw = rw;
        LOGI("optimal settings: output {}x{} -> render {}x{} ({})", w, h, rw, rh, QualityModeName(g_config.mode));
    }
    return NVSDK_NGX_Result_Success;
}

// ---------------------------------------------------------------------------------------------
// Current NGX (the SDK linked into this plugin).

void NVSDK_CONV NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string_view msg(message ? message : "");
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.remove_suffix(1);
    LOGI("[NGX] {}", msg);
}

// The game initialised the driver's NGX 1.x core with its application id before we were loaded.
// That context is process wide: left alive, our init joins it and NGX serves the game's
// per-application OTA model (DLSS 1.2) instead of nvngx_dlss.dll. It is torn down first.
void ShutdownLegacy() {
    for (Api api : {Api::D3D11, Api::D3D12}) {
        ApiState& s = State(api);
        auto shutdown = reinterpret_cast<PfnShutdown>(s.legacy.shutdown);
        if (!shutdown) continue;
        auto release = reinterpret_cast<PfnRelease>(s.legacy.release);
        for (auto& f : g_features) {
            if (f->api != api || !f->legacy) continue;
            if (release) release(f->legacy);
            f->legacy = nullptr;
        }
        NVSDK_NGX_Result r = shutdown();
        LOGD("legacy NGX {} context shut down ({:#x})", ApiName(api), static_cast<unsigned>(r));
        s.legacy = {};
    }
    g_legacyParams = nullptr;
}

// dxgi.dll is resolved at run time so loading the plugin never pulls it in.
ComPtr<IDXGIFactory1> DxgiFactory() {
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(dxgi, "CreateDXGIFactory1"))
                       : nullptr;
    ComPtr<IDXGIFactory1> factory;
    if (!create || FAILED(create(IID_PPV_ARGS(&factory)))) return nullptr;
    return factory;
}

bool AdapterDesc(Api api, void* device, DXGI_ADAPTER_DESC* desc) {
    ComPtr<IDXGIAdapter> adapter;
    if (api == Api::D3D11) {
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(static_cast<ID3D11Device*>(device)->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
            FAILED(dxgiDevice->GetAdapter(&adapter)))
            return false;
    } else {
        ComPtr<IDXGIFactory4> factory;
        if (auto f = DxgiFactory(); !f || FAILED(f.As(&factory))) return false;
        if (FAILED(factory->EnumAdapterByLuid(static_cast<ID3D12Device*>(device)->GetAdapterLuid(),
                                              IID_PPV_ARGS(&adapter))))
            return false;
    }
    return SUCCEEDED(adapter->GetDesc(desc));
}

// dxvk-nvapi's architecture name for the PCI device id of an NVIDIA RTX card. GTX 16 (Turing
// without tensor cores: TU116 0x21xx, TU117 0x1f8x and up) has no DLSS.
const wchar_t* RtxArchitecture(UINT id) {
    if (id >= 0x2b00) return L"GB200";                 // Blackwell GB20x (RTX 50)
    if (id >= 0x2600 && id < 0x2900) return L"AD100";  // Ada (RTX 40)
    if (id >= 0x2200 && id < 0x2600) return L"GA100";  // Ampere (RTX 30)
    if (id >= 0x1e00 && id < 0x1f80) return L"TU100";  // Turing TU102/TU104/TU106 (RTX 20)
    return nullptr;
}

// dxvk-nvapi (Proton's NVAPI) reports Turing and newer GPUs as Pascal to MonsterHunterWorld.exe,
// so that the game's own DLSS 1, which crashed under Proton, stays off. NGX then refuses DLSS too
// ("unsupported hw detected"). Its override DXVK_NVAPI_GPU_ARCH is read at every
// NvAPI_GPU_GetArchInfo call, so setting it here, after the game's NGX 1.x init, reaches only the
// plugin's init. Set as a launch option it also reaches the game's DLSS 1 (the game then quit
// before its logo in the Proton report).
void UnspoofGpuArchitecture(Api api, void* device) {
    const char* wine = WineVersion();
    if (!wine) return;
    wchar_t current[32];
    if (DWORD n = GetEnvironmentVariableW(L"DXVK_NVAPI_GPU_ARCH", current, 32); n && n < 32) {
        char name[32];
        WideCharToMultiByte(CP_UTF8, 0, current, -1, name, sizeof(name), nullptr, nullptr);
        LOGI("Wine: DXVK_NVAPI_GPU_ARCH={} from the launch options", name);
        return;
    }
    DXGI_ADAPTER_DESC desc{};
    if (!AdapterDesc(api, device, &desc)) {
        LOGW("Wine: the game's graphics adapter could not be read; NVAPI keeps reporting it as it does");
        return;
    }
    const wchar_t* arch = desc.VendorId == 0x10de ? RtxArchitecture(desc.DeviceId) : nullptr;
    if (!arch) {
        LOGI("Wine: adapter {:04x}:{:04x} is no known RTX card; NVAPI keeps reporting it as it does", desc.VendorId,
             desc.DeviceId);
        return;
    }
    SetEnvironmentVariableW(L"DXVK_NVAPI_GPU_ARCH", arch);
    char name[16];
    WideCharToMultiByte(CP_UTF8, 0, arch, -1, name, sizeof(name), nullptr, nullptr);
    LOGI("Wine: DXVK_NVAPI_GPU_ARCH={} set for NGX (device {:04x}; dxvk-nvapi shows this game RTX cards as "
         "GTX 10 cards)",
         name, desc.DeviceId);
}

bool InitModern(Api api, void* device);

bool EnsureModern(Api api, void* device) {
    if (g_modern.attempted) {
        if (g_modern.ready && (g_modern.api != api || g_modern.device != device)) {
            LOGE("NGX is initialised for {} device {}, but the game now uses {} device {}", ApiName(g_modern.api),
                 g_modern.device, ApiName(api), device);
            return false;
        }
        return g_modern.ready;
    }
    bool ready = InitModern(api, device);
    game::SetDlssUnavailable(!ready);
    if (!ready) {
        g_dlssOffPending = true;
        if (const char* wine = WineVersion())
            LOGW("running under Wine {}: DLSS needs Proton 9 or newer (Proton 8: PROTON_ENABLE_NVAPI=1 %command% in the "
                 "Steam launch options) and the NVIDIA driver's nvngx.dll in the prefix's system32, which Proton "
                 "copies from the driver",
                 wine);
    }
    return ready;
}

bool InitModern(Api api, void* device) {
    g_modern = {};
    g_modern.attempted = true;
    g_modern.api = api;
    g_modern.device = device;
    if (!device) {
        LOGE("NGX init: no device");
        return false;
    }

    ShutdownLegacy();

    if (!g_dlssFolderPtr) {
        g_dlssFolder = SelectDlssFolder(g_dataDir, g_gameDir);
        g_dlssFolderPtr = g_dlssFolder.c_str();
    }
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = &g_dlssFolderPtr;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = &NgxLogCallback;
    info.LoggingInfo.MinimumLoggingLevel =
        g_config.ngxLogging ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_OFF;
    info.LoggingInfo.DisableOtherLoggingSinks = true;

    if (g_config.debugNgxFail) {
        LOGE("NGX {} init failed (DebugNgxFail)", ApiName(api));
        return false;
    }
    UnspoofGpuArchitecture(api, device);
    NVSDK_NGX_Result r =
        api == Api::D3D12
            ? NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, MHW_DLSS4_VERSION,
                                                  g_dataDir.c_str(), static_cast<ID3D12Device*>(device), &info)
            : NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, MHW_DLSS4_VERSION,
                                                  g_dataDir.c_str(), static_cast<ID3D11Device*>(device), &info);
    if (NVSDK_NGX_FAILED(r)) {
        LOGE("NGX {} init failed ({:#x})", ApiName(api), static_cast<unsigned>(r));
        return false;
    }

    r = api == Api::D3D12 ? NVSDK_NGX_D3D12_GetCapabilityParameters(&g_modern.params)
                          : NVSDK_NGX_D3D11_GetCapabilityParameters(&g_modern.params);
    if (NVSDK_NGX_FAILED(r) || !g_modern.params) {
        LOGE("NGX GetCapabilityParameters failed ({:#x})", static_cast<unsigned>(r));
        return false;
    }

    int available = 0, needsDriver = 0, initResult = 0;
    unsigned minMajor = 0, minMinor = 0;
    g_modern.params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    g_modern.params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    g_modern.params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
    g_modern.params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
    g_modern.params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
    if (!available) {
        LOGE("DLSS is not available (init result {:#x}, needs driver update {}, minimum driver {}.{}). "
             "See the nvngx_dlss.dll line above.",
             static_cast<unsigned>(initResult), needsDriver, minMajor, minMinor);
        return false;
    }
    g_modern.ready = true;
    LOGI("NGX {} ready", ApiName(api));
    return true;
}

void ShutdownModern() {
    if (!g_modern.attempted) return;
    if (g_modern.ready) {
        if (g_modern.api == Api::D3D12)
            NVSDK_NGX_D3D12_Shutdown1(static_cast<ID3D12Device*>(g_modern.device));
        else
            NVSDK_NGX_D3D11_Shutdown1(static_cast<ID3D11Device*>(g_modern.device));
    }
    g_modern = {};
}

void LogDlssModules() {
    if (g_loggedModules) return;
    g_loggedModules = true;
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(modules[i], path, MAX_PATH)) continue;
        std::filesystem::path p(path);
        if (_wcsicmp(p.filename().c_str(), L"nvngx_dlss.dll") != 0) continue;
        LOGD("loaded {} {}", p.string(), VersionString(DllVersion(path)));
    }
}

// NGX frees a feature's resources at once, so a D3D12 feature is retired until the GPU has run the
// frames that evaluated it (see d3d::Retire). Releasing it right away faulted the GPU when the
// render size changed at run time.
void ReleaseDlss(Feature& f) {
    NVSDK_NGX_Handle* handle = f.dlss.handle;
    if (!handle) return;
    f.dlss.handle = nullptr;
    if (f.api == Api::D3D12) {
        d3d::RetireFeature([handle] {
            NVSDK_NGX_Result r = NVSDK_NGX_D3D12_ReleaseFeature(handle);
            if (NVSDK_NGX_FAILED(r)) LOGW("DLSS release failed ({:#x})", static_cast<unsigned>(r));
        });
        return;
    }
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_ReleaseFeature(handle);
    if (NVSDK_NGX_FAILED(r)) LOGW("DLSS release failed ({:#x})", static_cast<unsigned>(r));
}

int CreateFlags(bool hdr) {
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (g_config.hdr >= 0) hdr = g_config.hdr != 0;
    if (hdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (g_config.depthInverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (g_config.mvJittered) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (g_config.autoExposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    return flags;
}

NVSDK_NGX_PerfQuality_Value QualityFor(unsigned in, unsigned out) {
    float ratio = out ? static_cast<float>(in) / static_cast<float>(out) : 1.0f;
    if (ratio >= 0.99f) return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (ratio >= 0.62f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ratio >= 0.55f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (ratio >= 0.45f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

bool EnsureDlss(Feature& f, void* cmd, unsigned inW, unsigned inH, unsigned outW, unsigned outH, int flags,
                bool subrects) {
    DlssInstance& d = f.dlss;
    bool same = d.inWidth == inW && d.inHeight == inH && d.outWidth == outW && d.outHeight == outH &&
                d.flags == flags && d.subrects == subrects && d.preset == g_config.preset;
    if (d.handle && same) return true;
    if (d.failed && same) return false;  // don't retry every frame
    // Settings change mid-game on resolution changes and ini edits. On D3D12 the old feature lives
    // on beside the new one until the GPU is done with it.
    ReleaseDlss(f);
    d = {};
    d.inWidth = inW;
    d.inHeight = inH;
    d.outWidth = outW;
    d.outHeight = outH;
    d.flags = flags;
    d.subrects = subrects;
    d.preset = g_config.preset;
    d.failed = true;

    NVSDK_NGX_Parameter* p = g_modern.params;
    for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(p, key, g_config.preset);

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = inW;
    create.Feature.InHeight = inH;
    create.Feature.InTargetWidth = outW;
    create.Feature.InTargetHeight = outH;
    create.Feature.InPerfQualityValue = QualityFor(inW, outW);
    create.InFeatureCreateFlags = flags;
    create.InEnableOutputSubrects = subrects;

    NVSDK_NGX_Result r =
        f.api == Api::D3D12
            ? NGX_D3D12_CREATE_DLSS_EXT(static_cast<ID3D12GraphicsCommandList*>(cmd), 1, 1, &d.handle, p, &create)
            : NGX_D3D11_CREATE_DLSS_EXT(static_cast<ID3D11DeviceContext*>(cmd), &d.handle, p, &create);
    if (NVSDK_NGX_FAILED(r) || !d.handle) {
        LOGE("DLSS create {}x{} -> {}x{} failed ({:#x})", inW, inH, outW, outH, static_cast<unsigned>(r));
        d.handle = nullptr;
        return false;
    }
    d.failed = false;
    LOGI("DLSS feature created at the {} position: {}x{} -> {}x{}, quality {}, flags {:#x}, preset {}", f.Name(), inW,
         inH, outW, outH, static_cast<int>(create.Feature.InPerfQualityValue), flags,
         g_config.preset ? static_cast<char>('A' + g_config.preset - 1) : '-');
    LogDlssModules();
    return true;
}

struct EvalInputs {
    void* color = nullptr;
    void* output = nullptr;
    void* motion = nullptr;
    void* depth = nullptr;
    unsigned colorX = 0, colorY = 0;    // render rectangle origin in the colour texture
    unsigned motionX = 0, motionY = 0;  // ... in the motion vector and depth textures
    unsigned width = 0, height = 0;     // render rectangle size
    unsigned outputX = 0, outputY = 0;
    float jitterX = 0, jitterY = 0;
    int reset = 0;
};

NVSDK_NGX_Result RunDlss(Feature& f, void* cmd, const EvalInputs& in) {
    if (f.api == Api::D3D12) {
        NVSDK_NGX_D3D12_DLSS_Eval_Params e{};
        e.Feature.pInColor = static_cast<ID3D12Resource*>(in.color);
        e.Feature.pInOutput = static_cast<ID3D12Resource*>(in.output);
        e.pInDepth = static_cast<ID3D12Resource*>(in.depth);
        e.pInMotionVectors = static_cast<ID3D12Resource*>(in.motion);
        e.InJitterOffsetX = in.jitterX;
        e.InJitterOffsetY = in.jitterY;
        e.InRenderSubrectDimensions = {in.width, in.height};
        e.InReset = in.reset;
        e.InMVScaleX = g_config.mvScaleX;
        e.InMVScaleY = g_config.mvScaleY;
        e.InColorSubrectBase = {in.colorX, in.colorY};
        e.InDepthSubrectBase = {in.motionX, in.motionY};
        e.InMVSubrectBase = {in.motionX, in.motionY};
        e.InOutputSubrectBase = {in.outputX, in.outputY};
        e.InPreExposure = g_config.preExposure;
        e.InExposureScale = g_config.exposureScale;
        return NGX_D3D12_EVALUATE_DLSS_EXT(static_cast<ID3D12GraphicsCommandList*>(cmd), f.dlss.handle,
                                           g_modern.params, &e);
    }
    NVSDK_NGX_D3D11_DLSS_Eval_Params e{};
    e.Feature.pInColor = static_cast<ID3D11Resource*>(in.color);
    e.Feature.pInOutput = static_cast<ID3D11Resource*>(in.output);
    e.pInDepth = static_cast<ID3D11Resource*>(in.depth);
    e.pInMotionVectors = static_cast<ID3D11Resource*>(in.motion);
    e.InJitterOffsetX = in.jitterX;
    e.InJitterOffsetY = in.jitterY;
    e.InRenderSubrectDimensions = {in.width, in.height};
    e.InReset = in.reset;
    e.InMVScaleX = g_config.mvScaleX;
    e.InMVScaleY = g_config.mvScaleY;
    e.InColorSubrectBase = {in.colorX, in.colorY};
    e.InDepthSubrectBase = {in.motionX, in.motionY};
    e.InMVSubrectBase = {in.motionX, in.motionY};
    e.InOutputSubrectBase = {in.outputX, in.outputY};
    e.InPreExposure = g_config.preExposure;
    e.InExposureScale = g_config.exposureScale;
    return NGX_D3D11_EVALUATE_DLSS_EXT(static_cast<ID3D11DeviceContext*>(cmd), f.dlss.handle, g_modern.params, &e);
}

// Runs DLSS into in.output (outWidth x outHeight at in.outputX/Y). With sharpening on, DLSS
// writes the plugin's texture instead and the sharpening pass writes in.output from it.
NVSDK_NGX_Result RunDlssSharpened(Feature& f, void* cmd, EvalInputs in, unsigned outWidth, unsigned outHeight) {
    float strength = g_config.Sharpness();
    void* output = in.output;
    unsigned outX = in.outputX, outY = in.outputY;
    void* sharpenInput = nullptr;
    if (strength > 0.0f) {
        auto format = static_cast<DXGI_FORMAT>(Info(f.api, output).format);
        void* device = DeviceOf(f.api, cmd);
        sharpenInput =
            f.api == Api::D3D12
                ? static_cast<void*>(sharpen::Target12(static_cast<ID3D12Device*>(device), outWidth, outHeight, format))
                : static_cast<void*>(sharpen::Target11(static_cast<ID3D11Device*>(device), outWidth, outHeight, format));
    }
    if (sharpenInput) {
        in.output = sharpenInput;
        in.outputX = in.outputY = 0;
    }
    NVSDK_NGX_Result r = RunDlss(f, cmd, in);
    if (sharpenInput && NVSDK_NGX_SUCCEED(r)) {
        bool hdr = (f.dlss.flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
        if (f.api == Api::D3D12)
            sharpen::Apply12(static_cast<ID3D12GraphicsCommandList*>(cmd), static_cast<ID3D12Resource*>(output), outX,
                             outY, g_config.sharpener, strength, hdr);
        else
            sharpen::Apply11(static_cast<ID3D11DeviceContext*>(cmd), static_cast<ID3D11Resource*>(output), outX, outY,
                             g_config.sharpener, strength, hdr);
    }
    if (g_dumpOutputRequested && f.api == Api::D3D11) {
        g_dumpOutputRequested = false;
        if (d3d::DumpTexture11(static_cast<ID3D11DeviceContext*>(cmd), static_cast<ID3D11Resource*>(output),
                               g_dataDir + L"\\output.bin"))
            LOGI("output dump written");
    }
    return r;
}

void LogEvaluation(Feature& f, NVSDK_NGX_Result r, const EvalInputs& in, const d3d::TextureInfo& color,
                   const d3d::TextureInfo& output, uint64_t frame) {
    uint64_t n = f.evaluations++;
    if (in.reset) ++f.resets;
    if (n < kDetailedEvaluations || NVSDK_NGX_FAILED(r)) {
        LOGD("{} evaluate #{}: result {:#x}; colour {}x{} fmt {}, output {}x{} fmt {}; render {}x{} at {},{}; "
             "jitter {:.4f},{:.4f} (frame {}); reset {}",
             f.Name(), n, static_cast<unsigned>(r), color.width, color.height, color.format, output.width,
             output.height, output.format, in.width, in.height, in.colorX, in.colorY, in.jitterX, in.jitterY, frame,
             in.reset);
    } else if (g_config.verboseFrames && n < 40) {
        LOGD("{} evaluate #{}: jitter {:.4f},{:.4f} (frame {}) reset {}", f.Name(), n, in.jitterX, in.jitterY, frame,
             in.reset);
    } else if ((n & (n - 1)) == 0) {
        // Resets are history restarts (camera cuts, loading); many of them look like aliasing. #512 is
        // logged at Info as the sign that DLSS runs; test scripts wait for it.
        Log(n == 512 ? LogLevel::Info : LogLevel::Debug, "{} evaluate #{} ok ({} resets since #{}; {})", f.Name(), n,
            f.resets, n / 2, game::JitterStats());
        f.resets = 0;
    }
}

// The game's DLSS 1 call inside its TAA pass. Colour, motion vectors and depth are at render
// resolution; the output is the TAA history texture the rest of the frame reads.
NVSDK_NGX_Result EvaluateAtTaa(Feature& f, void* cmd, const NVSDK_NGX_Parameter* p) {
    EvalInputs in;
    in.color = GetResource(f.api, p, NVSDK_NGX_Parameter_Color);
    in.output = GetResource(f.api, p, NVSDK_NGX_Parameter_Output);
    in.motion = GetResource(f.api, p, NVSDK_NGX_Parameter_MotionVectors);
    in.depth = GetResource(f.api, p, NVSDK_NGX_Parameter_Depth);
    if (!in.color || !in.output || !in.motion || !in.depth) {
        if (f.evaluations++ < kDetailedEvaluations)
            LOGE("DLSS evaluate: missing input (colour {}, output {}, motion {}, depth {})", in.color, in.output,
                 in.motion, in.depth);
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    d3d::TextureInfo color = Info(f.api, in.color);
    d3d::TextureInfo output = Info(f.api, in.output);
    Rect rect = RectFrom(p, color);
    in.colorX = in.motionX = in.outputX = rect.x;
    in.colorY = in.motionY = in.outputY = rect.y;
    in.width = rect.width;
    in.height = rect.height;

    // The camera jitter is normalised by the output size, so in render pixels it shrinks with the
    // render scale.
    auto jitter = game::JitterForOutput(in.output);
    float scale = game::RenderScale();
    in.jitterX = (jitter ? jitter->x : 0.0f) * g_config.jitterScaleX * scale;
    in.jitterY = (jitter ? jitter->y : 0.0f) * g_config.jitterScaleY * scale;
    // The game resets only when it rebuilds the TAA history; camera cuts come from the camera.
    in.reset = GetI(p, NVSDK_NGX_Parameter_Reset) || (jitter && jitter->cut && g_config.cutReset);
    if (f.resetNext) {
        f.resetNext = false;
        in.reset = 1;
    }
    uint64_t frame = jitter ? jitter->frame : 0;

    if (f.api == Api::D3D12) {
        auto* cl = static_cast<ID3D12GraphicsCommandList*>(cmd);
        probe::Poll();
        // In the upscaling modes the DLISP call probes its own input instead.
        if (g_probeRequested && !game::DlispActive()) {
            g_probeRequested = false;
            probe::Schedule(cl, static_cast<ID3D12Resource*>(in.color), static_cast<ID3D12Resource*>(in.motion),
                            static_cast<ID3D12Resource*>(in.depth), static_cast<ID3D12Resource*>(in.output));
        }
        probe::RecordFrame(cl, static_cast<ID3D12Resource*>(in.color), static_cast<ID3D12Resource*>(in.motion),
                           static_cast<ID3D12Resource*>(in.depth), jitter ? jitter->x : 0.0f,
                           jitter ? jitter->y : 0.0f);
    }

    if (game::DlispActive()) {
        // Upscaling: hand the jittered frame on unresolved and keep what DLSS needs for the
        // DLISP call. The game reuses motion and depth targets later, so they are mirrored.
        if (rect.x + rect.width > output.width || rect.y + rect.height > output.height) {
            // A frame from across a render size change whose target has the other size already;
            // a copy outside the target would be undefined on D3D12.
            g_upscale.valid = false;
            return NVSDK_NGX_Result_Success;
        }
        if (f.api == Api::D3D12) {
            auto* cl = static_cast<ID3D12GraphicsCommandList*>(cmd);
            d3d::CopyRect12(cl, static_cast<ID3D12Resource*>(in.output), static_cast<ID3D12Resource*>(in.color),
                            rect.x, rect.y, rect.width, rect.height);
            g_upscale.motion = g_upscale.motion12.Update(cl, static_cast<ID3D12Resource*>(in.motion));
            g_upscale.depth = g_upscale.depth12.Update(cl, static_cast<ID3D12Resource*>(in.depth));
        } else {
            auto* ctx = static_cast<ID3D11DeviceContext*>(cmd);
            d3d::CopyRect11(ctx, static_cast<ID3D11Resource*>(in.output), static_cast<ID3D11Resource*>(in.color),
                            rect.x, rect.y, rect.width, rect.height);
            g_upscale.motion = g_upscale.motion11.Update(ctx, static_cast<ID3D11Resource*>(in.motion));
            g_upscale.depth = g_upscale.depth11.Update(ctx, static_cast<ID3D11Resource*>(in.depth));
        }
        g_upscale.valid = g_upscale.motion && g_upscale.depth;
        g_upscale.x = rect.x;
        g_upscale.y = rect.y;
        g_upscale.width = rect.width;
        g_upscale.height = rect.height;
        g_upscale.jitterX = in.jitterX;
        g_upscale.jitterY = in.jitterY;
        g_upscale.reset = in.reset;
        g_upscale.frame = frame;
        if (f.evaluations++ < kDetailedEvaluations)
            LOGD("DLSS evaluate: upscaling mode, render {}x{} passed on for DLISP", rect.width, rect.height);
        return NVSDK_NGX_Result_Success;
    }

    // DLAA does not use the copies the upscaling modes keep.
    g_upscale.valid = false;
    g_upscale.motion = g_upscale.depth = nullptr;
    if (f.api == Api::D3D12) {
        g_upscale.motion12.Reset();
        g_upscale.depth12.Reset();
    } else {
        g_upscale.motion11.Reset();
        g_upscale.depth11.Reset();
    }

    if (g_config.bypass) return NVSDK_NGX_Result_Success;  // the game's TAA result stays
    if (StaleFrame(f, rect.width, rect.height)) return NVSDK_NGX_Result_Success;
    if (!EnsureModern(f.api, DeviceOf(f.api, cmd))) return NVSDK_NGX_Result_Fail;
    bool hdr = GetI(p, "Hint.HDR", 1) != 0;
    bool subrects = rect.x || rect.y || rect.width != output.width || rect.height != output.height;
    if (!EnsureDlss(f, cmd, rect.width, rect.height, rect.width, rect.height, CreateFlags(hdr), subrects))
        return NVSDK_NGX_Result_Fail;
    game::NoteDlssEvaluated();
    NVSDK_NGX_Result r = RunDlssSharpened(f, cmd, in, rect.width, rect.height);
    LogEvaluation(f, r, in, color, output, frame);
    return r;
}

// The game's DLISP call: scales the post-processed frame from the render rectangle up to the
// output texture. With DLSS it is the point where the upscale happens.
NVSDK_NGX_Result EvaluateAtDlisp(Feature& f, void* cmd, const NVSDK_NGX_Parameter* p) {
    EvalInputs in;
    in.color = GetResource(f.api, p, NVSDK_NGX_Parameter_Color);
    in.output = GetResource(f.api, p, NVSDK_NGX_Parameter_Output);
    if (!in.color || !in.output) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (!g_upscale.valid) {
        // Happens on screens without a 3D frame (the intro logos) and on the odd transition frame.
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGD("DLISP called without a TAA pass before it (intro or loading screen); frame left to the game");
        }
        return NVSDK_NGX_Result_Fail;
    }
    g_upscale.valid = false;

    d3d::TextureInfo color = Info(f.api, in.color);
    d3d::TextureInfo output = Info(f.api, in.output);
    Rect rect = RectFrom(p, color);
    in.motion = g_upscale.motion;
    in.depth = g_upscale.depth;
    in.colorX = rect.x;
    in.colorY = rect.y;
    in.motionX = g_upscale.x;
    in.motionY = g_upscale.y;
    in.width = std::min(rect.width, g_upscale.width);
    in.height = std::min(rect.height, g_upscale.height);
    in.jitterX = g_upscale.jitterX;
    in.jitterY = g_upscale.jitterY;
    in.reset = g_upscale.reset;
    if (f.resetNext) {
        f.resetNext = false;
        in.reset = 1;
    }

    if (StaleFrame(f, in.width, in.height)) return NVSDK_NGX_Result_Success;
    if (g_probeRequested && f.api == Api::D3D12) {
        g_probeRequested = false;
        probe::Schedule(static_cast<ID3D12GraphicsCommandList*>(cmd), static_cast<ID3D12Resource*>(in.color), nullptr,
                        nullptr, static_cast<ID3D12Resource*>(in.output));
    }
    if (!EnsureModern(f.api, DeviceOf(f.api, cmd))) return NVSDK_NGX_Result_Fail;
    if (!EnsureDlss(f, cmd, in.width, in.height, output.width, output.height,
                    CreateFlags(d3d::IsFloatFormat(color.format)), false))
        return NVSDK_NGX_Result_Fail;
    game::NoteDlssEvaluated();
    NVSDK_NGX_Result r = RunDlssSharpened(f, cmd, in, output.width, output.height);
    LogEvaluation(f, r, in, color, output, g_upscale.frame);
    return r;
}

// ---------------------------------------------------------------------------------------------
// The functions placed into the game's NGX table.

NVSDK_NGX_Result ShimShutdown(Api api) {
    std::lock_guard lock(g_mutex);
    LOGD("game called NGX {} Shutdown", ApiName(api));
    for (auto& f : g_features)
        if (f->api == api) ReleaseDlss(*f);
    // Only what belongs to this API: both tables are hooked, and the game may shut down the one it
    // does not render with.
    g_upscale.valid = false;
    g_upscale.motion = g_upscale.depth = nullptr;
    if (api == Api::D3D12) {
        g_upscale.motion12.Reset();
        g_upscale.depth12.Reset();
    } else {
        g_upscale.motion11.Reset();
        g_upscale.depth11.Reset();
    }
    sharpen::Release(api == Api::D3D12);
    if (g_modern.api == api) {
        // NGX would free remaining features itself when it shuts down, so the retired ones go now.
        // Textures stay retired. The game shuts NGX down when it stops rendering (seen at exit).
        d3d::RetireFlushFeatures();
        ShutdownModern();
    }
    if (auto legacy = reinterpret_cast<PfnShutdown>(State(api).legacy.shutdown)) {
        legacy();
        State(api).legacy = {};
    }
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result ShimCreate(Api api, void*, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter* params,
                            NVSDK_NGX_Handle** out) {
    std::lock_guard lock(g_mutex);
    if (!out || !params) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *out = nullptr;
    if (id != NVSDK_NGX_Feature_SuperSampling && id != NVSDK_NGX_Feature_ImageSignalProcessing) {
        LOGW("game created unsupported NGX feature {}", static_cast<int>(id));
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    Feature* f = AddFeature(api, id, nullptr);
    f->expectedWidth = GetUI(params, NVSDK_NGX_Parameter_Width);
    f->expectedHeight = GetUI(params, NVSDK_NGX_Parameter_Height);
    LOGD("game created {} ({}): {}x{} -> {}x{}, HDR {}", f->Name(), ApiName(api),
         GetUI(params, NVSDK_NGX_Parameter_Width), GetUI(params, NVSDK_NGX_Parameter_Height),
         GetUI(params, NVSDK_NGX_Parameter_OutWidth), GetUI(params, NVSDK_NGX_Parameter_OutHeight),
         GetI(params, "Hint.HDR"));
    *out = f->Handle();
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result ShimEvaluate(Api api, void* cmd, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params,
                              void*) {
    std::lock_guard lock(g_mutex);
    if (!params) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    // A frame recorded before a resolution change can still evaluate the DLISP handle the game
    // has released and cleared in the meantime.
    if (!handle) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    Feature* f = FindFeature(handle);
    if (!f || f->api != api) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOGE("game evaluated unknown NGX handle {}", static_cast<const void*>(handle));
        }
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }
    return f->id == NVSDK_NGX_Feature_SuperSampling ? EvaluateAtTaa(*f, cmd, params) : EvaluateAtDlisp(*f, cmd, params);
}

NVSDK_NGX_Result ShimRelease(Api api, NVSDK_NGX_Handle* handle) {
    std::lock_guard lock(g_mutex);
    auto it = std::find_if(g_features.begin(), g_features.end(), [&](const auto& f) {
        return f->Handle() == handle || (f->legacy && f->legacy == handle);
    });
    if (it == g_features.end()) return NVSDK_NGX_Result_Success;
    Feature& f = **it;
    LOGD("game released {} feature {:#x}", f.Name(), f.handle.id);
    ReleaseDlss(f);
    if (f.legacy) {
        if (auto release = reinterpret_cast<PfnRelease>(State(api).legacy.release)) release(f.legacy);
    }
    g_features.erase(it);
    return NVSDK_NGX_Result_Success;
}

bool IsNvidiaGpu(std::string* others = nullptr);

template <Api A>
struct Entry {
    // Reached when the game initialises NGX through the plugin's entries (see HookNgxLookups) or
    // re-initialises it. The plugin initialises its own NGX lazily; without an NVIDIA GPU the game
    // is told NGX failed, as the NGX 1.x core would.
    static NVSDK_NGX_Result NVSDK_CONV Init(unsigned long long, const wchar_t*, void*, NVSDK_NGX_Version) {
        static const bool nvidia = IsNvidiaGpu();
        if (!nvidia) LOGI("game initialised NGX {}: no NVIDIA GPU, DLSS stays off", ApiName(A));
        return nvidia ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_PlatformError;
    }
    static NVSDK_NGX_Result NVSDK_CONV Shutdown() { return ShimShutdown(A); }
    static NVSDK_NGX_Result NVSDK_CONV Scratch(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, size_t* size) {
        if (size) *size = 0;
        return NVSDK_NGX_Result_Success;
    }
    static NVSDK_NGX_Result NVSDK_CONV GetParameters(NVSDK_NGX_Parameter** out) {
        if (!out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *out = &g_params;
        return NVSDK_NGX_Result_Success;
    }
    static NVSDK_NGX_Result NVSDK_CONV Create(void* cmd, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter* p,
                                              NVSDK_NGX_Handle** out) {
        return ShimCreate(A, cmd, id, p, out);
    }
    static NVSDK_NGX_Result NVSDK_CONV Evaluate(void* cmd, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p,
                                                void* cb) {
        return ShimEvaluate(A, cmd, h, p, cb);
    }
    static NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle* h) { return ShimRelease(A, h); }

    static Table Make() {
        return Table{reinterpret_cast<void*>(&Init),          reinterpret_cast<void*>(&Shutdown),
                     reinterpret_cast<void*>(&Scratch),       reinterpret_cast<void*>(&GetParameters),
                     reinterpret_cast<void*>(&Create),        reinterpret_cast<void*>(&Evaluate),
                     reinterpret_cast<void*>(&Release)};
    }
};

// ---------------------------------------------------------------------------------------------
// Locating and patching the table.

// First `mov r64, [rip+disp32]` in the function: the table slot the export reads.
void** SlotOf(HMODULE exe, const char* exportName) {
    auto* code = reinterpret_cast<uint8_t*>(GetProcAddress(exe, exportName));
    if (!code) return nullptr;
    for (int i = 0; i < 0x40; ++i) {
        if ((code[i] == 0x48 || code[i] == 0x4c) && code[i + 1] == 0x8b && (code[i + 2] & 0xc7) == 0x05)
            return reinterpret_cast<void**>(mem::RipTarget(code + i + 3, code + i + 7));
    }
    return nullptr;
}

Table* FindTable(HMODULE exe, Api api) {
    std::string prefix = std::string("NVSDK_NGX_") + ApiName(api) + "_";
    void** create = SlotOf(exe, (prefix + "CreateFeature").c_str());
    if (!create) return nullptr;
    auto* table = reinterpret_cast<Table*>(create - 4);
    struct Check {
        const char* name;
        void** expected;
    } checks[] = {
        {"Shutdown", &table->shutdown},
        {"GetScratchBufferSize", &table->scratch},
        {"GetParameters", &table->getParameters},
        {"EvaluateFeature_C", &table->evaluate},
        {"ReleaseFeature", &table->release},
    };
    for (const auto& c : checks) {
        void** slot = SlotOf(exe, (prefix + c.name).c_str());
        if (slot != c.expected) {
            LOGE("NGX {} table layout mismatch at {} ({} vs {})", ApiName(api), c.name, static_cast<void*>(slot),
                 static_cast<void*>(c.expected));
            return nullptr;
        }
    }
    return table;
}

// (Re)applies the shim entries. The game's NGX init rewrites the table, so this also runs per frame.
void PatchTable(ApiState& s, Api api, bool initial) {
    if (!s.table) return;
    Table current = *s.table;
    if (current.create == s.shim.create) return;
    // Remember whatever the legacy core installed, ignoring empty slots from a failed init.
    if (current.create) s.legacy = current;
    void** slots = reinterpret_cast<void**>(s.table);
    const void* const* values = reinterpret_cast<const void* const*>(&s.shim);
    for (int i = 0; i < 7; ++i) mem::WritePointer(slots + i, const_cast<void*>(values[i]));
    if (initial)
        LOGD("NGX {} table hooked (legacy core {})", ApiName(api), s.legacy.create ? "loaded" : "not loaded");
    else
        LOGW("NGX {} table was rewritten by the game, hooked again", ApiName(api));
}

// Whether DXGI lists an NVIDIA adapter. `others` gets the names of the other hardware adapters.
bool IsNvidiaGpu(std::string* others) {
    {
        std::lock_guard lock(g_mutex);
        if (g_config.debugNoNvidia) {
            if (others) *others = "DebugNoNvidia";
            return false;
        }
    }
    ComPtr<IDXGIFactory1> factory = DxgiFactory();
    if (!factory) return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.VendorId == 0x10de) return true;
        if (!others || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        char name[256];
        if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr)) continue;
        if (!others->empty()) *others += ", ";
        *others += name;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Loaders that load plugins early (SharpPluginLoader preloads Stracker's Loader) put the plugin in
// place before the game's renderer and its NGX 1.x init. That init clears the function table,
// loads the driver's NGX 1.x core and fills the table through the exe's GetProcAddress import.
// Left alone, the game then sets DLSS up with the NGX 1.x core: DLSS 1's upscaler alone (DLISP at
// 0.75) with no DLSS pass the plugin could feed, so nothing wrote the frame (a grey screen), and
// switching the game to a DLSS pass afterwards crashed its render thread. So when the renderer does
// not exist yet, those lookups get the plugin's entries, and the game sets DLSS up through the
// plugin from the start, as when the plugin is loaded after that init.

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_realGetProcAddress = nullptr;

FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name) {
    FARPROC real = g_realGetProcAddress(module, name);
    if (!real || IS_INTRESOURCE(name) || std::strncmp(name, "NVSDK_NGX_D3D1", 14) != 0) return real;
    // Same order as Table.
    static constexpr const char* kEntries[] = {"Init",          "Shutdown",        "GetScratchBufferSize",
                                               "GetParameters", "CreateFeature",   "EvaluateFeature",
                                               "ReleaseFeature"};
    for (Api api : {Api::D3D11, Api::D3D12}) {
        std::string prefix = std::string("NVSDK_NGX_") + ApiName(api) + "_";
        if (std::strncmp(name, prefix.c_str(), prefix.size()) != 0) continue;
        const void* const* shim = reinterpret_cast<const void* const*>(&State(api).shim);
        for (int i = 0; i < 7; ++i) {
            if (std::strcmp(name + prefix.size(), kEntries[i]) != 0) continue;
            if (i == 0) LOGD("game initialises NGX {} through the plugin's entries", ApiName(api));
            return reinterpret_cast<FARPROC>(const_cast<void*>(shim[i]));
        }
    }
    return real;
}

// The exe's import slot for GetProcAddress, taken from the calls in the NGX init exports.
void** FindGetProcAddressSlot(HMODULE exe) {
    void* real[3] = {reinterpret_cast<void*>(&GetProcAddress), nullptr, nullptr};
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) real[1] = reinterpret_cast<void*>(GetProcAddress(k32, "GetProcAddress"));
    if (HMODULE kb = GetModuleHandleW(L"kernelbase.dll")) real[2] = reinterpret_cast<void*>(GetProcAddress(kb, "GetProcAddress"));
    for (const char* exportName : {"NVSDK_NGX_D3D12_Init", "NVSDK_NGX_D3D11_Init"}) {
        auto* code = reinterpret_cast<uint8_t*>(GetProcAddress(exe, exportName));
        if (!code) continue;
        for (int i = 0; i < 0x200; ++i) {
            if (code[i] != 0xff || code[i + 1] != 0x15) continue;  // call [rip+disp32]
            auto** slot = reinterpret_cast<void**>(mem::RipTarget(code + i + 2, code + i + 6));
            void* value = mem::SafeRead<void*>(slot).value_or(nullptr);
            if (value && (value == real[0] || value == real[1] || value == real[2])) return slot;
        }
    }
    return nullptr;
}

void HookNgxLookups(HMODULE exe) {
    void** slot = FindGetProcAddressSlot(exe);
    if (!slot) {
        LOGW("loaded before the game's renderer, but the GetProcAddress import of its NGX init was not found; "
             "DLSS may need to be turned off and on in the graphics options");
        return;
    }
    if (!mem::HookSlot(slot, reinterpret_cast<void*>(&HookGetProcAddress), reinterpret_cast<void**>(&g_realGetProcAddress))) {
        LOGW("loaded before the game's renderer, but the GetProcAddress import of its NGX init could not be hooked; "
             "DLSS may need to be turned off and on in the graphics options");
        return;
    }
    LOGD("loaded before the game's renderer: its NGX init will get the plugin's entries (import slot +{:#x})",
         reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(exe));
}

// Makes the game's DLSS context use our parameter object, adopting features the legacy core
// created before the plugin was loaded. Builds the context when the game skipped it because the
// legacy core refused DLSS (e.g. a current nvngx_dlss.dll in the game folder). `mayUseDxgi` is
// false under the loader lock; the GPU vendor check then waits for the first frame.
void AdoptContext(bool mayUseDxgi) {
    game::DlssContext* ctx = game::Context();
    if (ctx) {
        if (ctx->params == &g_params) return;
        auto* lock = reinterpret_cast<CRITICAL_SECTION*>(ctx->lock);
        EnterCriticalSection(lock);
        g_legacyParams = static_cast<NVSDK_NGX_Parameter*>(ctx->params);
        ctx->params = &g_params;
        Api api = game::IsDx12().value_or(true) ? Api::D3D12 : Api::D3D11;
        if (ctx->dlssHandle && !FindFeature(static_cast<NVSDK_NGX_Handle*>(ctx->dlssHandle)))
            AddFeature(api, NVSDK_NGX_Feature_SuperSampling, static_cast<NVSDK_NGX_Handle*>(ctx->dlssHandle));
        if (ctx->dlispHandle && !FindFeature(static_cast<NVSDK_NGX_Handle*>(ctx->dlispHandle)))
            AddFeature(api, NVSDK_NGX_Feature_ImageSignalProcessing, static_cast<NVSDK_NGX_Handle*>(ctx->dlispHandle));
        LeaveCriticalSection(lock);
        int legacyDlss = -1;
        if (g_legacyParams) g_legacyParams->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &legacyDlss);
        LOGD("DLSS context adopted (game's NGX 1.x reported DLSS available {}; existing DLSS {}, DLISP {})",
             legacyDlss, ctx->dlssHandle, ctx->dlispHandle);
        g_adoptedLate = mayUseDxgi;  // after Install, which refreshes the render scale itself
        return;
    }
    if (g_contextCreated || g_fallbackChecked) return;
    if (!game::Device()) return;  // renderer not up yet
    bool legacyLoaded = g_api[0].legacy.create || g_api[1].legacy.create;
    if (!legacyLoaded && !mayUseDxgi) return;
    g_fallbackChecked = true;

    auto dx12 = game::IsDx12();
    if (!dx12) {
        LOGW("no DLSS context and the renderer API is unknown; DLSS stays unavailable");
        return;
    }
    std::string others;
    if (!legacyLoaded && !IsNvidiaGpu(&others)) {
        // DLSS and Reflex both need one; the hooks call straight through to the game from now on.
        LOGW("no NVIDIA graphics card found ({}): DLSS and Reflex need one, the plugin stays inactive",
             others.empty() ? "none listed" : others);
        if (const char* wine = WineVersion())
            LOGW("running under Wine {}: an NVIDIA card shows up as another vendor while NVAPI is off (Proton 8 and "
                 "older: PROTON_ENABLE_NVAPI=1 %command% in the Steam launch options; newer Proton turns it off "
                 "only with PROTON_DISABLE_NVAPI=1)",
                 wine);
        game::Deactivate();
        return;
    }
    auto* mem = static_cast<game::DlssContext*>(game::GameAlloc(sizeof(game::DlssContext)));
    if (!mem) {
        LOGE("cannot allocate the DLSS context");
        return;
    }
    std::memset(mem, 0, sizeof(*mem));
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(mem->lock));
    mem->params = &g_params;
    if (!*dx12) mem->d3d11Context = game::ImmediateContext11();
    game::SetContext(mem);
    g_contextCreated = true;
    LOGI("game had no DLSS context ({} the NGX 1.x core); created one, DLSS can now be enabled in the "
         "graphics options",
         legacyLoaded ? "DLSS was refused by" : "no");
}

// At every TAA pass build: worker threads, possibly several per frame and at the same time.
void OnFrame() {
    std::lock_guard lock(g_mutex);
    PatchTable(g_api[0], Api::D3D11, false);
    PatchTable(g_api[1], Api::D3D12, false);
    AdoptContext(true);

    // The game creates its DLISP feature only when DLSS is (re)created. After switching from DLAA
    // to an upscaling mode at run time, give it one so the DLISP pass has a handle to use.
    if (game::DlispActive()) {
        game::DlssContext* ctx = game::Context();
        if (ctx && !ctx->dlispHandle) {
            Api api = game::IsDx12().value_or(true) ? Api::D3D12 : Api::D3D11;
            ctx->dlispHandle = AddFeature(api, NVSDK_NGX_Feature_ImageSignalProcessing, nullptr)->Handle();
            LOGD("attached a DLISP feature for the upscaling mode");
        }
    }

    static game::RendererState lastState;
    game::RendererState state = game::ReadState();
    if (!(state == lastState)) {
        LOGD("renderer: {}", game::DescribeState(state));
        lastState = state;
    }
}

void ReloadSettings(bool& recompute);

// Once per frame on the game's main thread, where the game applies graphics options itself.
void OnUpdate() {
    bool recompute = false;
    {
        std::lock_guard lock(g_mutex);
        // Also here and not only at TAA pass builds: those stop while the game's TAA is off.
        PatchTable(g_api[0], Api::D3D11, false);
        PatchTable(g_api[1], Api::D3D12, false);
        AdoptContext(true);
        d3d::RetireTick(game::Frame());
        if (g_dlssOffPending) {
            g_dlssOffPending = false;
            if (game::DlssEnabledInGame()) {
                LOGW("DLSS could not be started (see above): the game renders without it and shows its NVIDIA DLSS "
                     "option as unavailable until the next start");
                recompute = true;
            }
        }
        // The game set DLSS up through the NGX 1.x core before the context was adopted (normally
        // prevented by HookNgxLookups). With a DLSS pass, asking again is a Mode switch. Without one
        // (DLSS 1's upscaler alone) switching at run time crashed the game's render thread, so
        // that is left to the game's own options.
        if (g_adoptedLate) {
            g_adoptedLate = false;
            if (game::DlssEnabledInGame()) {
                LOGD("the game set DLSS up before the plugin took over; asking it for the render scale again");
                recompute = true;
            } else if (game::DlssOptionOn()) {
                LOGW("the game set DLSS up with DLSS 1's upscaler before the plugin took over; turn NVIDIA DLSS "
                     "off and on in the graphics options");
            }
        }
        static ULONGLONG nextReload = 0;
        ULONGLONG now = GetTickCount64();
        if (now >= nextReload) {
            nextReload = now + 500;
            ReloadSettings(recompute);
        }
    }
    // Outside the lock: the game's recompute releases and creates its DLSS features through the
    // shim and may wait for the render thread, which takes the lock to evaluate.
    if (recompute && game::RecomputeRenderScale()) LOGD("render scale refreshed");
}

// Twice a second: the ini and graphics_option.ini (a timestamp check each). Sets `recompute` when
// the render scale may have changed.
void ReloadSettings(bool& recompute) {
    if (ReloadConfigIfChanged()) {
        game::ApplyConfig(g_config);
        reflex::Configure(g_config);
        LogSetLevel(g_config.logLevel);
        LOGI("ini reloaded: mode {}, preset {}, depth inverted {}, MV scale {},{}, jitter scale {},{}, auto "
             "exposure {}, reflection fix {}, bypass {}, Reflex {}, frame limit {}",
             QualityModeName(g_config.mode), g_config.preset, g_config.depthInverted, g_config.mvScaleX,
             g_config.mvScaleY, g_config.jitterScaleX, g_config.jitterScaleY, g_config.autoExposure,
             g_config.reflectionFix, g_config.bypass, ReflexModeName(g_config.reflex), g_config.frameLimit);
        for (auto& f : g_features) {
            f->dlss.failed = false;  // allow a retry with the new settings
            f->evaluations = 0;      // log the next evaluations in detail again
            f->resetNext = true;     // start from an empty history
        }
        g_probeRequested = g_config.debugProbe;
        g_dumpOutputRequested = g_config.debugDumpOutput;
        if (g_config.debugDumpFrames > 0)
            probe::RequestFrameDump(g_dataDir.c_str(), g_config.debugDumpFrames,
                                    static_cast<unsigned>(g_config.debugDumpDelay * 1000.0f));
        if (game::DlssOptionOn()) recompute = true;
    }
    ReloadGameOptionsIfChanged();
    static float lastSharpness = -1.0f;
    static Sharpener lastSharpener = Sharpener::NVSharpen;
    if (g_config.Sharpness() != lastSharpness || g_config.sharpener != lastSharpener) {
        lastSharpness = g_config.Sharpness();
        lastSharpener = g_config.sharpener;
        LOGI("sharpening {:.2f} with {} (from {})", lastSharpness, SharpenerName(lastSharpener),
             g_config.sharpness < 0.0f ? "the game's DLSS Sharpness option" : "the ini");
    }
}

bool InstallTables(const std::wstring& dataDir, const std::wstring& gameDir) {
    std::lock_guard lock(g_mutex);
    g_dataDir = dataDir;
    g_gameDir = gameDir;

    g_params.Set(NVSDK_NGX_Parameter_SuperSampling_Available, 1);
    g_params.Set("ImageSignalProcessing.Available", 1);  // DLISP, NGX 1.x only
    g_params.Set(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, reinterpret_cast<void*>(&OptimalSettingsCallback));

    HMODULE exe = GetModuleHandleW(nullptr);
    g_api[0].table = FindTable(exe, Api::D3D11);
    g_api[1].table = FindTable(exe, Api::D3D12);
    if (!g_api[0].table && !g_api[1].table) {
        LOGE("NGX function tables not found");
        return false;
    }
    g_api[0].shim = Entry<Api::D3D11>::Make();
    g_api[1].shim = Entry<Api::D3D12>::Make();
    PatchTable(g_api[0], Api::D3D11, true);
    PatchTable(g_api[1], Api::D3D12, true);

    AdoptContext(false);
    if (!game::Device()) HookNgxLookups(exe);
    game::SetFrameCallback(&OnFrame);
    game::SetUpdateCallback(&OnUpdate);
    LOGD("renderer at install: {}", game::DescribeState());
    return true;
}

}  // namespace

const char* WineVersion() {
    using WineGetVersionFn = const char*(__cdecl*)();
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto get = ntdll ? reinterpret_cast<WineGetVersionFn>(GetProcAddress(ntdll, "wine_get_version")) : nullptr;
    return get ? get() : nullptr;
}

bool Install(const std::wstring& dataDir, const std::wstring& gameDir) {
    if (!InstallTables(dataDir, gameDir)) return false;
    // Outside the lock (see OnUpdate). The game picked its render scale with the old callback
    // before we were loaded.
    if (game::Context() && game::DlssEnabledInGame() && game::RecomputeRenderScale())
        LOGD("DLSS was already on, render scale refreshed");
    return true;
}

}  // namespace mhwdlss::ngx
