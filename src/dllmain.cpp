#include <windows.h>

#include <filesystem>

#include "config.h"
#include "crash.h"
#include "game.h"
#include "log.h"
#include "ngx_shim.h"
#include "reflex.h"

namespace fs = std::filesystem;
using namespace mhwdlss;

namespace {

std::string Narrow(const fs::path& p) { return p.string(); }

// Under Wine: the host system, and where the NVIDIA driver's NGX core is registered. Proton copies
// nvngx.dll and _nvngx.dll there from the driver; NGX can't start without them.
void LogWine(const char* version) {
    using HostVersionFn = void(__cdecl*)(const char**, const char**);
    auto host = reinterpret_cast<HostVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_host_version"));
    const char* system = nullptr;
    const char* release = nullptr;
    if (host) host(&system, &release);
    LOGI("running under Wine {} on {} {}", version, system ? system : "?", release ? release : "?");

    wchar_t folder[MAX_PATH]{};
    DWORD bytes = sizeof(folder);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ,
                     nullptr, folder, &bytes) != ERROR_SUCCESS) {
        LOGW("Wine: no NVIDIA NGX core registered in the prefix (Proton copies it from the NVIDIA driver): DLSS can't "
             "start");
        return;
    }
    fs::path core(folder);
    std::error_code ec;
    bool nvngx = fs::exists(core / L"nvngx.dll", ec);
    bool core2 = fs::exists(core / L"_nvngx.dll", ec);
    if (nvngx && core2)
        LOGI("Wine: NVIDIA NGX core in {}", Narrow(core));
    else
        LOGW("Wine: NVIDIA NGX core registered in {}, but nvngx.dll {} and _nvngx.dll {}: DLSS can't start",
             Narrow(core), nvngx ? "is there" : "is missing", core2 ? "is there" : "is missing");
}

void Start(HMODULE module) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(module, buf, MAX_PATH);
    fs::path dll(buf);
    fs::path dir = dll.parent_path();
    std::wstring stem = dll.stem().wstring();

    LoadConfig((dir / (stem + L".ini")).wstring());
    LogInit((dir / (stem + L".log")).wstring(), g_config.logLevel);

    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path exe(buf);
    LoadGameOptions((exe.parent_path() / L"graphics_option.ini").wstring());
    LOGI("MHW-DLSS4 {} loaded from {}", MHW_DLSS4_VERSION, Narrow(dll));
    LOGI("game: {}", Narrow(exe));
    if (const char* wine = ngx::WineVersion()) LogWine(wine);
    LOGI("config: mode {} (render scale {:.3f}), preset {}, sharpness {:.2f}{} with {}, depth inverted {}, MV scale {},{}, "
         "jitter scale {},{}, reflection fix {}, Reflex {}, frame limit {}",
         QualityModeName(g_config.mode), g_config.RenderScale(),
         g_config.PresetLetter(), g_config.Sharpness(),
         g_config.sharpness < 0.0f ? " (game option)" : "", SharpenerName(g_config.sharpener), g_config.depthInverted, g_config.mvScaleX,
         g_config.mvScaleY, g_config.jitterScaleX, g_config.jitterScaleY, g_config.reflectionFix,
         ReflexModeName(g_config.reflex),
         g_config.frameLimit);

    std::error_code ec;
    // Up to 0.7.3 the plugin was MHWDLSS.dll. With both in the folder only the one the loader loads
    // first hooks the game: the other no longer finds the TAA filter's vtable entry.
    if (_wcsicmp(dll.filename().c_str(), L"MHWDLSS.dll") != 0 && fs::exists(dir / L"MHWDLSS.dll", ec))
        LOGW("an older version (MHWDLSS.dll) is also installed: delete MHWDLSS.dll, MHWDLSS.ini, MHWDLSS.log and "
             "the MHWDLSS folder from {} (move a nvngx_dlss.dll of your own from there to {} first)",
             Narrow(dir), Narrow(dir / stem));

    if (!g_config.enabled) {
        LOGI("disabled in the ini");
        return;
    }
    InstallCrashLogger();

    // Folder for NGX's files and the DLSS DLLs the plugin uses (see dlss_dll.h). It sits beside the
    // plugin instead of in nativePC\plugins itself because the loader would load every DLL found there.
    fs::path dataDir = dir / stem;
    fs::create_directories(dataDir, ec);

    if (!game::Init()) {
        LOGE("game hooks failed, plugin inactive");
        return;
    }
    if (!ngx::Install(dataDir.wstring(), exe.parent_path().wstring())) {
        LOGE("NGX hooks failed, plugin inactive");
        return;
    }
    reflex::Configure(g_config);
    reflex::Install();
    // Only now do the game hooks act; until here they call straight through to the game.
    game::Activate(g_config);
    LOGI("ready; turn on NVIDIA DLSS in the game's graphics options");
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        try {
            Start(module);
        } catch (const std::exception& e) {
            LOGE("startup failed: {}", e.what());
        } catch (...) {
            LOGE("startup failed");
        }
    }
    return TRUE;
}
