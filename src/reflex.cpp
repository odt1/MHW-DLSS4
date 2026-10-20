#include "reflex.h"

#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include <nvapi_reflex.h>

#include "config.h"
#include "game.h"
#include "log.h"
#include "memory.h"

namespace mhwdlss::reflex {

namespace {

using Microsoft::WRL::ComPtr;

// NVAPI is used through nvapi_QueryInterface from the driver's nvapi64.dll, nothing is linked.
using QueryInterfaceFn = void*(__cdecl*)(NvU32 id);
using InitializeFn = NvAPI_Status(__cdecl*)();
using GetSleepStatusFn = NvAPI_Status(__cdecl*)(IUnknown* device, NV_GET_SLEEP_STATUS_PARAMS_V1* params);
using SetSleepModeFn = NvAPI_Status(__cdecl*)(IUnknown* device, NV_SET_SLEEP_MODE_PARAMS_V1* params);
using SleepFn = NvAPI_Status(__cdecl*)(IUnknown* device);
using GetLatencyFn = NvAPI_Status(__cdecl*)(IUnknown* device, NV_LATENCY_RESULT_PARAMS_V1* params);
using SetLatencyMarkerFn = NvAPI_Status(__cdecl*)(IUnknown* device, NV_LATENCY_MARKER_PARAMS_V1* params);

struct NvApi {
    GetSleepStatusFn getSleepStatus = nullptr;
    SetSleepModeFn setSleepMode = nullptr;
    SleepFn sleep = nullptr;
    GetLatencyFn getLatency = nullptr;
    SetLatencyMarkerFn setLatencyMarker = nullptr;
};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
using SetEventFn = BOOL(WINAPI*)(HANDLE event);
using WaitFn = DWORD(WINAPI*)(HANDLE handle, DWORD milliseconds);

constexpr int kPresentSlot = 8;  // IDXGISwapChain::Present
constexpr ULONGLONG kReportIntervalMs = 5000;

struct Settings {
    ReflexMode mode = ReflexMode::On;
    unsigned intervalUs = 0;  // frame rate cap, 0 = none
    bool stats = false;
    bool sleep = true;
    bool operator==(const Settings&) const = default;
};

// NVAPI is needed for any of these; with none the plugin leaves Reflex alone.
bool InUse(const Settings& s) { return s.mode != ReflexMode::Off || s.intervalUs || s.stats; }

NvApi g_nv;
bool g_nvTried = false;
bool g_nvReady = false;

// From Configure, which runs where the ini is reloaded: in the TAA update, which the game runs on
// the main thread or on a job thread.
std::mutex g_settingsMutex;
Settings g_wanted;
bool g_applyPending = true;

// Main thread only.
Settings g_applied;
bool g_sleepModeSet = false;  // the driver has settings of ours other than all off
DWORD g_mainThread = 0;

// The game's swap chain and the D3D device NVAPI gets (the game keeps both alive).
IDXGISwapChain* g_swapChain = nullptr;
std::atomic<IUnknown*> g_device{nullptr};
// Markers and Sleep are on (NVAPI loaded, device known, settings in use).
std::atomic<bool> g_active{false};

PresentFn g_origPresent = nullptr;
void* const* g_presentVtable = nullptr;
SetEventFn g_origSetEvent = nullptr;
WaitFn g_origWait = nullptr;
std::atomic<HANDLE> g_startEvent{nullptr};
std::atomic<HANDLE> g_doneEvent{nullptr};

// Frame ids. A frame is simulated in one main thread update and rendered and presented during the
// next one, so the ids of the render and present markers trail the simulation's by one. Set on the
// main thread; the render thread reads g_renderFrame.
uint64_t g_simFrame = 0;                 // frame the main thread simulates
uint64_t g_queuedFrame = 0;              // newest simulated frame the renderer has taken over
std::atomic<uint64_t> g_renderFrame{0};  // frame being rendered, shown by the next Present
uint64_t g_renderStartFrame = 0;         // last frame RENDERSUBMIT_START was sent for
std::atomic<uint64_t> g_renderEndFrame{0};
uint64_t g_presentedFrame = 0;
bool g_renderStarted = false;  // the render thread was started since the last Present
bool g_simEnded = false;
bool g_inputSampled = false;

void Marker(NV_LATENCY_MARKER_TYPE type, uint64_t frame) {
    IUnknown* device = g_device.load(std::memory_order_acquire);
    if (!frame || !device || !g_active.load(std::memory_order_relaxed)) return;
    NV_LATENCY_MARKER_PARAMS_V1 p{};
    p.version = NV_LATENCY_MARKER_PARAMS_VER1;
    p.frameID = frame;
    p.markerType = type;
    g_nv.setLatencyMarker(device, &p);
}

bool OnMainThread() { return GetCurrentThreadId() == g_mainThread; }

// ---------------------------------------------------------------------------------------------
// Hooks: the swap chain's Present (vtable), and the exe's SetEvent and WaitForSingleObject imports,
// through which the main thread starts the render thread, waits for it, and the render thread
// reports that it has submitted the frame.

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    bool tracked = swapChain == g_swapChain && !(flags & DXGI_PRESENT_TEST) && g_active.load() && OnMainThread();
    uint64_t frame = 0;
    if (tracked) {
        // Without the render thread the game renders on the main thread, right before this.
        if (!g_renderStarted) g_renderFrame = g_queuedFrame;
        frame = g_renderFrame;
        if (frame <= g_presentedFrame) frame = 0;  // presented already
        if (!g_simEnded) {
            g_simEnded = true;
            Marker(SIMULATION_END, g_simFrame);
        }
        Marker(PRESENT_START, frame);
    }
    HRESULT hr = g_origPresent(swapChain, syncInterval, flags);
    if (tracked) {
        Marker(PRESENT_END, frame);
        if (frame) g_presentedFrame = frame;
        // The game hands the frame it simulated to the renderer right before presenting.
        g_queuedFrame = g_simFrame;
        g_renderStarted = false;
    }
    return hr;
}

BOOL WINAPI HookSetEvent(HANDLE event) {
    if (event && g_active.load(std::memory_order_relaxed)) {
        if (event == g_startEvent.load(std::memory_order_relaxed) && OnMainThread()) {
            if (g_queuedFrame > g_renderStartFrame) {
                g_renderStartFrame = g_queuedFrame;
                g_renderFrame = g_queuedFrame;
                g_renderStarted = true;
                Marker(RENDERSUBMIT_START, g_queuedFrame);
            }
        } else if (event == g_doneEvent.load(std::memory_order_relaxed)) {
            uint64_t frame = g_renderFrame.load();
            if (frame > g_renderEndFrame.load()) {
                g_renderEndFrame = frame;
                Marker(RENDERSUBMIT_END, frame);
            }
        }
    }
    return g_origSetEvent(event);
}

DWORD WINAPI HookWait(HANDLE handle, DWORD milliseconds) {
    // The main thread waits for the render thread once its simulation is done.
    if (handle && handle == g_doneEvent.load(std::memory_order_relaxed) && g_active.load(std::memory_order_relaxed) &&
        OnMainThread() && !g_simEnded) {
        g_simEnded = true;
        Marker(SIMULATION_END, g_simFrame);
    }
    return g_origWait(handle, milliseconds);
}

bool LoadNvApi() {
    if (g_nvTried) return g_nvReady;
    g_nvTried = true;
    HMODULE nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nvapi) {
        LOGI("NVIDIA Reflex: no nvapi64.dll (no NVIDIA driver), Reflex stays off");
        return false;
    }
    auto query = reinterpret_cast<QueryInterfaceFn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    auto initialize = query ? reinterpret_cast<InitializeFn>(query(kNvApiId_Initialize)) : nullptr;
    NvAPI_Status status = initialize ? initialize() : -1;
    if (status != NVAPI_OK) {
        LOGW("NVIDIA Reflex: NVAPI initialisation failed ({}), Reflex stays off", status);
        return false;
    }
    g_nv.getSleepStatus = reinterpret_cast<GetSleepStatusFn>(query(kNvApiId_D3D_GetSleepStatus));
    g_nv.setSleepMode = reinterpret_cast<SetSleepModeFn>(query(kNvApiId_D3D_SetSleepMode));
    g_nv.sleep = reinterpret_cast<SleepFn>(query(kNvApiId_D3D_Sleep));
    g_nv.getLatency = reinterpret_cast<GetLatencyFn>(query(kNvApiId_D3D_GetLatency));
    g_nv.setLatencyMarker = reinterpret_cast<SetLatencyMarkerFn>(query(kNvApiId_D3D_SetLatencyMarker));
    if (!g_nv.getSleepStatus || !g_nv.setSleepMode || !g_nv.sleep || !g_nv.getLatency || !g_nv.setLatencyMarker) {
        LOGW("NVIDIA Reflex: the driver's NVAPI has no Reflex functions (driver too old?), Reflex stays off");
        return false;
    }
    g_nvReady = true;
    return true;
}

// The D3D device behind the swap chain. For D3D12 the swap chain's device is its command queue,
// except under ReShade, whose swap chain hands out the device itself.
IUnknown* DeviceOf(IDXGISwapChain* swapChain, bool dx12) {
    HRESULT hr;
    if (dx12) {
        ComPtr<ID3D12Device> device;
        hr = swapChain->GetDevice(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) return device.Get();
        ComPtr<ID3D12CommandQueue> queue;
        hr = swapChain->GetDevice(IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr)) hr = queue->GetDevice(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) return device.Get();
    } else {
        ComPtr<ID3D11Device> device;
        hr = swapChain->GetDevice(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) return device.Get();
    }
    LOGD("NVIDIA Reflex: the swap chain did not give its device ({:#x})", static_cast<unsigned>(hr));
    return nullptr;
}

// The pointer read from the game is a COM object whose vtable lies in a loaded module.
bool LooksLikeObject(void* object) {
    auto vtable = mem::SafeRead<void*>(object);
    HMODULE module = nullptr;
    return vtable && *vtable &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(*vtable), &module);
}

bool HookSwapChain(IDXGISwapChain* swapChain) {
    auto* const* vtable = *reinterpret_cast<void* const* const*>(swapChain);
    if (g_presentVtable) {
        if (vtable == g_presentVtable) return true;
        LOGW("NVIDIA Reflex: the game's new swap chain is of another kind; its Present is not tracked");
        return false;
    }
    auto** slot = const_cast<void**>(vtable + kPresentSlot);
    if (!mem::HookSlot(slot, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_origPresent))) {
        LOGW("NVIDIA Reflex: the swap chain's Present could not be hooked");
        return false;
    }
    g_presentVtable = vtable;
    return true;
}

void HookImports() {
    if (void** slot = game::SetEventImport(); slot && !g_origSetEvent) {
        if (!mem::HookSlot(slot, reinterpret_cast<void*>(&HookSetEvent), reinterpret_cast<void**>(&g_origSetEvent)))
            g_origSetEvent = nullptr;
    }
    if (void** slot = game::WaitForSingleObjectImport(); slot && !g_origWait) {
        if (!mem::HookSlot(slot, reinterpret_cast<void*>(&HookWait), reinterpret_cast<void**>(&g_origWait)))
            g_origWait = nullptr;
    }
}

// Once the game has its swap chain (and again if it makes a new one).
void Attach(const Settings& wanted) {
    if (!InUse(wanted) && !g_swapChain) return;  // hooked once Reflex is wanted
    auto* swapChain = static_cast<IDXGISwapChain*>(game::SwapChain());
    if (!swapChain || swapChain == g_swapChain) return;
    if (!LoadNvApi()) return;
    auto dx12 = game::IsDx12();
    if (!dx12) return;
    static void* rejected = nullptr;
    if (swapChain == rejected) return;
    if (!LooksLikeObject(swapChain)) {
        LOGW("NVIDIA Reflex: the game's swap chain {} does not look like a COM object", static_cast<void*>(swapChain));
        rejected = swapChain;
        return;
    }
    IUnknown* device = DeviceOf(swapChain, *dx12);
    if (!device) {
        LOGW("NVIDIA Reflex: no D3D device behind the game's swap chain {}", static_cast<void*>(swapChain));
        rejected = swapChain;
        return;
    }
    if (!HookSwapChain(swapChain)) return;
    HookImports();
    game::RenderEvents events = game::RenderThreadEvents();
    g_startEvent = events.start;
    g_doneEvent = events.done;
    g_swapChain = swapChain;
    g_device = device;
    {
        std::lock_guard lock(g_settingsMutex);
        g_applyPending = true;
    }
    LOGD("NVIDIA Reflex: {} swap chain {}, device {}, render thread events {} / {}", *dx12 ? "D3D12" : "D3D11",
         static_cast<void*>(swapChain), static_cast<void*>(device), events.start, events.done);
}

void LogSleepStatus(IUnknown* device, const char* what) {
    NV_GET_SLEEP_STATUS_PARAMS_V1 status{};
    status.version = NV_GET_SLEEP_STATUS_PARAMS_VER1;
    NvAPI_Status s = g_nv.getSleepStatus(device, &status);
    if (s != NVAPI_OK) {
        LOGW("NVIDIA Reflex: {}; sleep status not available ({})", what, s);
        return;
    }
    LOGI("NVIDIA Reflex: {}; the driver reports low latency mode {}, game sleep {}, fullscreen VRR {}, control "
         "panel V-Sync {}",
         what, status.bLowLatencyMode ? "on" : "off", status.bUseGameSleep ? "used" : "not used", status.bFsVrr != 0,
         status.bCplVsyncOn != 0);
}

void Apply(IUnknown* device, const Settings& wanted) {
    bool use = InUse(wanted);
    if (use || g_sleepModeSet) {
        NV_SET_SLEEP_MODE_PARAMS_V1 p{};
        p.version = NV_SET_SLEEP_MODE_PARAMS_VER1;
        p.bLowLatencyMode = wanted.mode != ReflexMode::Off;
        p.bLowLatencyBoost = wanted.mode == ReflexMode::Boost;
        p.minimumIntervalUs = wanted.intervalUs;
        p.bUseMarkersToOptimize = 1;
        NvAPI_Status s = g_nv.setSleepMode(device, &p);
        if (s != NVAPI_OK) {
            LOGW("NVIDIA Reflex: setting low latency mode {} failed ({})", ReflexModeName(wanted.mode), s);
            use = false;
        } else {
            g_sleepModeSet = use;
            std::string what = std::format("low latency mode {}", ReflexModeName(wanted.mode));
            if (wanted.intervalUs) what += std::format(", frame limit {:.1f} fps", 1e6 / wanted.intervalUs);
            if (wanted.intervalUs && GameFrameRate())
                what += std::format(" (the game's Frame Rate option caps at {} too; set it to No Limit)", GameFrameRate());
            LogSleepStatus(device, what.c_str());
        }
    }
    g_applied = wanted;
    g_active = use;
}

// Averages of NVIDIA's latency report over its last 64 frames, every few seconds (LatencyStats=1).
// The times are in microseconds.
void Report(IUnknown* device) {
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + kReportIntervalMs;
    static NV_LATENCY_RESULT_PARAMS_V1 r;
    std::memset(&r, 0, sizeof(r));
    r.version = NV_LATENCY_RESULT_PARAMS_VER1;
    NvAPI_Status s = g_nv.getLatency(device, &r);
    if (s != NVAPI_OK) {
        LOGI("latency report not available ({})", s);
        return;
    }
    struct Mean {
        double sum = 0;
        int n = 0;
        void Add(uint64_t from, uint64_t to) {
            if (!from || !to || to < from) return;
            sum += static_cast<double>(to - from);
            ++n;
        }
        double Ms() const { return n ? sum / n / 1000.0 : 0.0; }
    };
    // Spread of the intervals between consecutive Presents and between consecutive frames done on the
    // graphics card: how evenly frames reach the screen.
    struct Spread {
        double sum = 0, squares = 0;
        int n = 0;
        void Add(uint64_t from, uint64_t to) {
            if (!from || !to || to < from) return;
            double v = static_cast<double>(to - from) / 1000.0;
            sum += v;
            squares += v * v;
            ++n;
        }
        double Ms() const { return n > 1 ? std::sqrt(std::max(0.0, squares / n - (sum / n) * (sum / n))) : 0.0; }
    };
    Mean inputToGpu, startToGpu, interval, toInput, sim, render, toPresent, present, presentToGpu, gpu;
    Spread presentPacing, gpuPacing;
    int frames = 0;
    uint64_t previousStart = 0, previousPresent = 0, previousGpuEnd = 0;
    for (const auto& f : r.frameReport) {
        if (!f.frameID || !f.simStartTime || !f.gpuRenderEndTime) {
            previousStart = previousPresent = previousGpuEnd = 0;
            continue;
        }
        ++frames;
        presentPacing.Add(previousPresent, f.presentStartTime);
        gpuPacing.Add(previousGpuEnd, f.gpuRenderEndTime);
        previousPresent = f.presentStartTime;
        previousGpuEnd = f.gpuRenderEndTime;
        inputToGpu.Add(f.inputSampleTime, f.gpuRenderEndTime);
        startToGpu.Add(f.simStartTime, f.gpuRenderEndTime);
        interval.Add(previousStart, f.simStartTime);
        toInput.Add(f.simStartTime, f.inputSampleTime);
        sim.Add(f.simStartTime, f.simEndTime);
        render.Add(f.renderSubmitStartTime, f.renderSubmitEndTime);
        toPresent.Add(f.simStartTime, f.presentStartTime);
        present.Add(f.presentStartTime, f.presentEndTime);
        presentToGpu.Add(f.presentEndTime, f.gpuRenderEndTime);
        gpu.sum += f.gpuActiveRenderTimeUs;
        ++gpu.n;
        previousStart = f.simStartTime;
    }
    if (!frames) {
        LOGI("latency report: no complete frames yet");
        return;
    }
    NV_GET_SLEEP_STATUS_PARAMS_V1 status{};
    status.version = NV_GET_SLEEP_STATUS_PARAMS_VER1;
    g_nv.getSleepStatus(device, &status);
    LOGI("latency over {} frames: input to GPU done {:.1f} ms, frame start to GPU done {:.1f}, frame interval {:.2f} "
         "(spread: Present {:.2f}, GPU done {:.2f}); input read {:.1f} ms into the frame, simulation {:.1f}, render "
         "thread {:.1f}, frame start to Present {:.1f}, Present {:.1f}, Present to GPU done {:.1f}, GPU busy {:.1f}; "
         "driver low latency mode {}, game Frame Rate {}",
         frames, inputToGpu.Ms(), startToGpu.Ms(), interval.Ms(), presentPacing.Ms(), gpuPacing.Ms(), toInput.Ms(),
         sim.Ms(), render.Ms(), toPresent.Ms(), present.Ms(), presentToGpu.Ms(), gpu.Ms(),
         status.bLowLatencyMode ? "on" : "off", GameFrameRate() ? std::to_string(GameFrameRate()) : std::string("No Limit"));
}

// The start of the game's frame on the main thread, before its frame rate limiter and input: Reflex
// sleeps, then the frame's simulation starts.
//
// The update starts the render thread on the previous frame after the game's limiter, and that
// frame's command lists go to the graphics card right before its Present at the end of the update,
// so any wait on the main thread holds some frame back: a frame rate cap costs about one frame
// interval of input lag wherever it sleeps. Sleeping after the render thread start instead was
// measured to be no better, and with Low Latency Boost it halved the frame rate.
void OnFrameStart() {
    g_mainThread = GetCurrentThreadId();
    Settings wanted;
    bool pending;
    {
        std::lock_guard lock(g_settingsMutex);
        wanted = g_wanted;
        pending = g_applyPending;
    }
    Attach(wanted);
    IUnknown* device = g_device.load();
    if (!device) return;
    if (pending) {
        {
            std::lock_guard lock(g_settingsMutex);
            g_applyPending = false;
            wanted = g_wanted;
        }
        Apply(device, wanted);
    }
    if (!g_active) return;
    if (g_applied.stats) Report(device);
    if (g_applied.sleep) g_nv.sleep(device);
    ++g_simFrame;
    g_simEnded = g_inputSampled = false;
    Marker(SIMULATION_START, g_simFrame);
}

void OnInput() {
    if (!g_active || !OnMainThread() || g_inputSampled) return;
    g_inputSampled = true;
    Marker(INPUT_SAMPLE, g_simFrame);
}

}  // namespace

void Install() {
    game::SetFrameStartCallback(&OnFrameStart);
    game::SetInputCallback(&OnInput);
}

void Configure(const Config& config) {
    Settings s;
    s.mode = config.reflex;
    s.intervalUs = config.frameLimit > 0.0f ? static_cast<unsigned>(std::lround(1e6 / config.frameLimit)) : 0;
    s.stats = config.latencyStats;
    s.sleep = config.reflexSleep;
    std::lock_guard lock(g_settingsMutex);
    if (s == g_wanted && !g_applyPending) return;
    g_wanted = s;
    g_applyPending = true;
}

}  // namespace mhwdlss::reflex
