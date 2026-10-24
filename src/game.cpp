#include "game.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "config.h"
#include "log.h"
#include "memory.h"

namespace mhwdlss::game {

namespace {

// Offsets inside game objects (MHW:I 15.2x, DX11 and DX12 builds share them).
constexpr ptrdiff_t kRendererDevice = 0x78;
constexpr ptrdiff_t kRendererDlssOn = 0xa5;
constexpr ptrdiff_t kRendererDlssBits = 0xb4;     // 1 = DLSS, 2 = DLISP
constexpr ptrdiff_t kRendererDlispOn = 0xa6;
constexpr ptrdiff_t kRendererScaleApplied = 0x204;
constexpr ptrdiff_t kRendererRenderFrame = 0x1f208;
constexpr ptrdiff_t kRendererFrame = 0x1f210;
constexpr ptrdiff_t kRendererView = 0x1f450;
constexpr ptrdiff_t kViewFlags = 0x58;
// View flag bits for which the TAA update and draw do nothing.
constexpr uint32_t kViewSkipsTaa = (1u << 0x12) | (1u << 0x17);
constexpr ptrdiff_t kDeviceDlssContext = 0x42700;
constexpr ptrdiff_t kCameraJitter = 0x38;
constexpr ptrdiff_t kCameraJitterFrame = 0x40;  // render frame the jitter was set in
// The camera's viewports (8 at +0x50, 0x1a0 bytes each). Its draw (+0x228e770) runs the camera
// setup for each active viewport and then builds that viewport's scene pass, the TAA pass with it.
constexpr ptrdiff_t kCameraViewports = 0x50;
constexpr ptrdiff_t kViewportStride = 0x1a0;
constexpr int kViewportCount = 8;
constexpr ptrdiff_t kViewportTarget = 0x18;  // own render target; null for the screen
constexpr ptrdiff_t kViewportActive = 0x20;
constexpr ptrdiff_t kViewportPass = 0x25;    // 0 = main
constexpr ptrdiff_t kViewportView = 0xa0;    // view matrix (row vectors), unjittered
constexpr ptrdiff_t kViewportProj = 0xe0;    // projection, unjittered
constexpr ptrdiff_t kTaaHistory = 0x168;   // three texture objects, indexed by frame % 3
constexpr ptrdiff_t kTaaSharpen = 0x187;   // mSharpen: TAA sharpen pass; the history slot shifts by one
constexpr ptrdiff_t kTextureNative = 0x60;
constexpr ptrdiff_t kTextureDepthNative = 0xf0;
// Natives are reference counted; the game releases them at +0x2554f90 (lock xadd -1, destroyed
// when the count was 1).
constexpr ptrdiff_t kNativeRefCount = 0x8;
constexpr ptrdiff_t kNativeResource11 = 0x18;
constexpr ptrdiff_t kNativeResource12 = 0x20;
constexpr ptrdiff_t kDrawContextView = 0x1d0;
constexpr ptrdiff_t kViewTargets = 0x628;
constexpr ptrdiff_t kTargetsDepthCopy = 0xa0;

// uTemporalAAFilter2 per-frame update: picks the Halton jitter for the frame and hands it to
// the camera. It references the renderer, scene and camera singletons.
constexpr std::string_view kTaaUpdatePattern =
    "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 8B 90 50 F4 01 00 8B 42 58 0F BA E0 12 72 ?? "
    "0F BA E0 17 72 ?? 0F 57 C0 0F 2E 81 80 01 00 00 74 ?? 83 79 0C 08 75 ?? 8B 41 14 "
    "83 E0 12 3C 12 75 ?? 80 B9 84 01 00 00 00 74 ?? 48 8B 0D ?? ?? ?? ?? 48 89 5C 24 20 "
    "48 8B 1D ?? ?? ?? ?? E8";
// uTemporalAAFilter2 draw: builds the TAA pass and, with DLSS on, the NGX evaluate command.
constexpr std::string_view kTaaDrawPrologue = "48 89 54 24 10 55 53 48 8D 6C 24 88 48 81 EC 78 01 00 00";
// Renderer method that re-queries DLSS optimal settings and updates the render scale.
constexpr std::string_view kRecomputePattern =
    "40 53 48 83 EC 40 48 8B 41 78 48 8B D9 48 85 C0 0F 84 ?? ?? ?? ?? 4C 8B 90 00 27 04 00 "
    "4D 85 D2 0F 84 ?? ?? ?? ?? 48 8B 81 48 F4 01 00 8B 89 40 B4 07 00";
// Slot distance in the vtable between update (+0x48) and draw (+0x60).
constexpr ptrdiff_t kDrawSlotFromUpdate = 3;
// Scene setter for the anti-aliasing switches (scene, mode 0..3): writes a word, TAA in its low
// byte and FXAA in its high byte. Only searched for the word's offset.
constexpr std::string_view kSceneAaSetterPattern =
    "44 0F B6 C2 84 D2 74 ?? 41 83 E8 01 74 ?? 41 83 E8 01 74 ?? 41 83 F8 01 75 ?? 66 C7 81 ?? ?? ?? ?? 01 01 C3";
constexpr ptrdiff_t kSceneAaSetterDisp = 29;
// Picking the screen-space reflection resolve technique (in +0x2286260): the scene's
// mGlobalEnvMapEachWaterAttn, mSSLRDitherResolve and mSSLRGBufferJitter bytes go to the selector
// (+0x227ec90). Only searched for the offset of mSSLRGBufferJitter.
constexpr std::string_view kSslrResolveSelectPattern =
    "44 0F B6 8E ?? ?? ?? ?? 85 C0 44 0F B6 86 ?? ?? ?? ?? 0F B6 96 ?? ?? ?? ?? 0F 94 C1 88 5C 24 20 E8";
constexpr ptrdiff_t kSslrGBufferJitterDisp = 21;
// Material sampler resolve (in +0x2383400): reads the scene's mSamplerLodBias (a bank number) and
// takes the sampler from that bank. Only searched for the offset of mSamplerLodBias.
constexpr std::string_view kSamplerBankPattern =
    "48 8B 05 ?? ?? ?? ?? 8B D2 8B 88 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B 8C C8 88 24 00 00";
constexpr ptrdiff_t kSamplerBankDisp = 11;
// Paired getters "is DX11" / "is DX12" reading the same global byte.
constexpr std::string_view kApiFlagPattern =
    "80 3D ?? ?? ?? ?? 00 0F 94 C0 C3 CC CC CC CC CC 0F B6 05 ?? ?? ?? ?? C3";
// operator new: forwards to the MT Framework allocator (vtable +0x48, 16-byte alignment).
constexpr std::string_view kOperatorNewPattern =
    "4C 8B 0D ?? ?? ?? ?? 4D 85 C9 74 ?? 49 8B 01 48 8B D1 41 B8 10 00 00 00 49 8B C9 48 FF 60 48";
constexpr ptrdiff_t kDeviceImmediateContext11 = 0x1400;
// Swap chains of the device (count at +0x14a0, 0x48 bytes each): the D3D12 present calls Present on
// the interface at +0x1470 (exe +0x23aa1d4), the D3D11 present on the one at +0x1488 (+0x23aa330).
constexpr ptrdiff_t kDeviceSwapChain12 = 0x1470;
constexpr ptrdiff_t kDeviceSwapChain11 = 0x1488;
constexpr ptrdiff_t kDeviceSwapChainCount = 0x14a0;
// Render thread (config.ini RenderingThread=ON): started each frame through the event at +0x1b8,
// signals +0x1c0 when it has submitted the frame.
constexpr ptrdiff_t kRendererStartEvent = 0x1b8;
constexpr ptrdiff_t kRendererDoneEvent = 0x1c0;

// sMhMain's per-frame update (its vtable +0x30), which the main thread loop runs once per frame:
// the frame timer with the limiter of the Frame Rate option (Sleep, then a QueryPerformanceCounter
// spin), the start of the render thread on the previous frame, input polling and the simulation,
// then the wait for the render thread and the Present of the previous frame.
constexpr std::string_view kMainUpdatePattern =
    "40 53 48 81 EC 80 00 00 00 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? "
    "48 8B 8B 88 F8 0A 00 48 8B 01 FF 50 48 83 BB CC 00 00 00 01";
// sMhMouse's per-frame update (its vtable +0x30): reads the mouse through DirectInput.
constexpr std::string_view kMouseUpdatePattern =
    "40 53 41 57 48 83 EC 28 48 8B D9 E8 ?? ?? ?? ?? 45 33 FF 44 38 BB B2 00 00 00";
// The render thread's loop end: SetEvent(renderer->done), then WaitForSingleObject(renderer->start).
constexpr std::string_view kRenderThreadPattern =
    "48 8B 0D ?? ?? ?? ?? 48 8B 89 C0 01 00 00 FF 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
    "BA FF FF FF FF 48 8B 89 B8 01 00 00 FF 15";

uint8_t** g_renderer = nullptr;
uint8_t** g_camera = nullptr;
uint8_t** g_scene = nullptr;
const uint8_t* g_apiFlag = nullptr;
// Halton(2,3) table the TAA update picks the jitter from (20 entries, float2; the count is a
// constant in the code, +0x227db60).
constexpr ptrdiff_t kSceneHalton = 0xe6a0;
constexpr uint64_t kHaltonCount = 20;

using OperatorNewFn = void*(__fastcall*)(size_t size);
OperatorNewFn g_operatorNew = nullptr;

using TaaDrawFn = void*(__fastcall*)(void* self, void* drawContext);
TaaDrawFn g_origTaaDraw = nullptr;
using TaaUpdateFn = void*(__fastcall*)(void* self);
TaaUpdateFn g_origTaaUpdate = nullptr;
constexpr ptrdiff_t kTaaJitterEnable = 0x184;

using RecomputeFn = void(__fastcall*)(void* renderer);
RecomputeFn g_recompute = nullptr;

using FrameUpdateFn = void*(__fastcall*)(void* self);
FrameUpdateFn g_origMainUpdate = nullptr;
FrameUpdateFn g_origMouseUpdate = nullptr;
void (*g_frameStartCallback)() = nullptr;
void (*g_inputCallback)() = nullptr;
void** g_setEventImport = nullptr;
void** g_waitImport = nullptr;

// Renderer frame of the last DLSS evaluation, plus one (0 = none yet).
std::atomic<uint64_t> g_lastDlssFrame{0};
// Since the last JitterStats() call.
std::atomic<uint64_t> g_jitterFrames{0};
std::atomic<uint64_t> g_jitterSetByPlugin{0};
std::atomic<uint64_t> g_sharpenSuppressed{0};

// The hooks run on several game threads at once (the TAA pass is built on worker threads, the
// update on the main thread), so they read their settings from here, published by ApplyConfig,
// instead of from g_config while an ini reload replaces it.
struct HookSettings {
    std::atomic<bool> bypass{false};
    std::atomic<bool> forceJitter{true};
    std::atomic<bool> fullPixelJitter{true};
    std::atomic<bool> verboseFrames{false};
    std::atomic<bool> debugFixedJitter{false};
    std::atomic<float> debugJitterX{0.0f};
    std::atomic<float> debugJitterY{0.0f};
    std::atomic<bool> reflectionFix{true};
    std::atomic<int> jitterPhases{-1};
    std::atomic<float> mipBias{std::numeric_limits<float>::quiet_NaN()};
};
HookSettings g_settings;
// Until the NGX side is installed the hooks only call the game's functions.
std::atomic<bool> g_active{false};
std::atomic<bool> g_dlssUnavailable{false};
void (*g_updateCallback)() = nullptr;

struct JitterRecord {
    const void* resource11 = nullptr;
    const void* resource12 = nullptr;
    Jitter jitter;
};
std::mutex g_jitterMutex;
std::array<JitterRecord, 8> g_jitterRecords;
size_t g_jitterNext = 0;

// Camera cuts. The game only asks DLSS for a reset when it rebuilds the TAA history (render size
// changes, loading); a cut in a cutscene keeps the history, and DLSS then blends the previous shot
// into the first frames of the new one. A cut is a jump of the main viewport's camera since the last
// frame that built the TAA pass: position, view direction or projection. Frames without a TAA pass
// in between don't hide a cut; DLSS had no new input then, so a jump across them needs a reset too.
constexpr float kCutDistance = 500.0f;     // world units (centimetres)
constexpr float kCutTurnCos = 0.7071f;     // 45 degrees
constexpr float kCutProjection = 0.05f;    // relative change of the projection's x/y scale

struct CameraShot {
    std::array<float, 3> position{};
    std::array<float, 3> forward{};
    std::array<float, 2> scale{};
    int viewport = 0;
};
uint64_t g_cutFrame = 0;  // renderer frame of the last camera read
bool g_cutFrameIsCut = false;
std::optional<CameraShot> g_lastShot;
int g_shotViewport = -2;  // viewport of the last read, -1 none, -2 not read yet
std::atomic<uint64_t> g_cuts{0};
int g_viewportLogs = 0;

// The main viewport (active, pass 0, drawn to the screen) of the camera, as position, view direction
// and projection scale. View matrices are row-vector (world * V), so V = [R 0; t 1] and the camera
// position is -t * R^T.
std::optional<CameraShot> ReadShot(uint8_t* camera) {
    for (int i = 0; i < kViewportCount; ++i) {
        uint8_t* vp = camera + kCameraViewports + i * kViewportStride;
        auto active = mem::SafeRead<uint8_t>(vp + kViewportActive);
        auto pass = mem::SafeRead<uint8_t>(vp + kViewportPass);
        auto target = mem::SafeRead<void*>(vp + kViewportTarget);
        if (!active || !*active || !pass || *pass != 0 || !target || *target) continue;
        auto view = mem::SafeRead<std::array<float, 16>>(vp + kViewportView);
        auto proj = mem::SafeRead<std::array<float, 16>>(vp + kViewportProj);
        if (!view || !proj) return std::nullopt;
        const auto& v = *view;
        CameraShot shot;
        shot.viewport = i;
        for (int j = 0; j < 3; ++j) {
            shot.position[j] = -(v[12] * v[j * 4 + 0] + v[13] * v[j * 4 + 1] + v[14] * v[j * 4 + 2]);
            shot.forward[j] = v[j * 4 + 2];
        }
        shot.scale = {(*proj)[0], (*proj)[5]};
        bool finite = std::ranges::all_of(v, [](float x) { return std::isfinite(x); }) &&
                      std::isfinite(shot.scale[0]) && std::isfinite(shot.scale[1]);
        if (!finite) return std::nullopt;
        return shot;
    }
    return std::nullopt;
}

// Whether the camera of renderer frame `frame` cut away from the previous frame's. Called under
// g_jitterMutex from every TAA pass build; the camera is read once per frame.
bool CameraCut(uint8_t* camera, uint64_t frame) {
    if (frame == g_cutFrame) return g_cutFrameIsCut;
    std::optional<CameraShot> shot = ReadShot(camera);
    int viewport = shot ? shot->viewport : -1;
    if (viewport != g_shotViewport && g_viewportLogs < 20) {
        ++g_viewportLogs;
        if (shot)
            LOGD("camera: main viewport {} (cut detection on)", viewport);
        else
            LOGD("camera: no main viewport (active, pass 0, drawn to the screen); camera cuts are not "
                 "detected until one is back");
    }
    g_shotViewport = viewport;
    g_cutFrame = frame;
    g_cutFrameIsCut = false;
    if (shot && g_lastShot) {
        const CameraShot& a = *g_lastShot;
        const CameraShot& b = *shot;
        float d2 = 0.0f, dot = 0.0f, la = 0.0f, lb = 0.0f;
        for (int j = 0; j < 3; ++j) {
            float d = b.position[j] - a.position[j];
            d2 += d * d;
            dot += a.forward[j] * b.forward[j];
            la += a.forward[j] * a.forward[j];
            lb += b.forward[j] * b.forward[j];
        }
        float cosTurn = la > 0.0f && lb > 0.0f ? dot / std::sqrt(la * lb) : 1.0f;
        float zoom = 0.0f;
        for (int j = 0; j < 2; ++j)
            if (a.scale[j] != 0.0f) zoom = std::max(zoom, std::abs(b.scale[j] / a.scale[j] - 1.0f));
        g_cutFrameIsCut = d2 > kCutDistance * kCutDistance || cosTurn < kCutTurnCos || zoom > kCutProjection;
        if (g_cutFrameIsCut && g_cuts.fetch_add(1) < 20)
            LOGD("camera cut at frame {}: moved {:.0f}, turned {:.0f} degrees, zoom {:.1f}%; DLSS history reset",
                 frame, std::sqrt(d2), std::acos(std::clamp(cosTurn, -1.0f, 1.0f)) * 57.29578f, zoom * 100.0f);
    }
    g_lastShot = shot;
    return g_cutFrameIsCut;
}

void CaptureJitter(uint8_t* taa) {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    uint8_t* camera = g_camera ? *g_camera : nullptr;
    if (!renderer || !camera) return;

    auto frame = mem::SafeRead<uint64_t>(renderer + kRendererFrame);
    auto jitter = mem::SafeRead<std::array<float, 2>>(camera + kCameraJitter);
    auto sharpen = mem::SafeRead<uint8_t>(taa + kTaaSharpen);
    if (!frame || !jitter || !sharpen) return;

    // Same slot choice as the game's DLSS evaluate: history[frame % 3], shifted by one
    // when the TAA sharpen pass is active.
    uint64_t slot = (*sharpen ? *frame + 1 : *frame) % 3;
    auto texture = mem::SafeRead<uint8_t*>(taa + kTaaHistory + slot * sizeof(void*));
    if (!texture || !*texture) return;
    auto native = mem::SafeRead<uint8_t*>(*texture + kTextureNative);
    if (!native || !*native) return;

    JitterRecord rec;
    rec.resource11 = mem::SafeRead<void*>(*native + kNativeResource11).value_or(nullptr);
    rec.resource12 = mem::SafeRead<void*>(*native + kNativeResource12).value_or(nullptr);
    rec.jitter = {(*jitter)[0], (*jitter)[1], *frame};

    static std::atomic<uint64_t> logged{0};
    uint64_t n = logged.load();
    if (n < 4 || (g_settings.verboseFrames && n < 40)) {
        ++logged;
        // Inputs of the TAA update's jitter decision, to explain a zero jitter.
        LOGD("jitter capture: frame {}, slot {}, jitter {:.4f},{:.4f}, camera frame {}, render frame {}, blend {:.3f}, "
             "type {}, flags {:#x}, enabled {}, view flags {:#x}",
             *frame, slot, (*jitter)[0], (*jitter)[1],
             mem::SafeRead<uint64_t>(camera + kCameraJitter + 8).value_or(0),
             mem::SafeRead<uint64_t>(renderer + 0x1f208).value_or(0),
             mem::SafeRead<float>(taa + 0x180).value_or(-1.0f), mem::SafeRead<uint32_t>(taa + 0xc).value_or(0),
             mem::SafeRead<uint32_t>(taa + 0x14).value_or(0), mem::SafeRead<uint8_t>(taa + 0x184).value_or(0),
             mem::SafeRead<uint32_t>(mem::SafeRead<uint8_t*>(renderer + 0x1f450).value_or(nullptr) + 0x58)
                 .value_or(0));
    }

    std::lock_guard lock(g_jitterMutex);
    rec.jitter.cut = CameraCut(camera, *frame);
    g_jitterRecords[g_jitterNext] = rec;
    g_jitterNext = (g_jitterNext + 1) % g_jitterRecords.size();
}

void (*g_frameCallback)() = nullptr;

// The DLSS evaluate command takes the view's depth from its R32F depth copy, a plain texture.
// On DX12 the command's barrier setup reads the texture's depth-view native (+0xf0), which only
// depth formats have, and crashes on null. The game's own copy command falls back to the colour
// native (+0x60) in that case; give the depth copy the same fallback before the pass is built.
//
// When the texture is rebuilt (ini reloads, graphics option changes, area changes) the game
// releases +0x60 and +0xf0 separately, so the alias takes a reference of its own. Without it the
// first release destroyed the native and the second decremented freed memory, or the count of
// whatever object had been allocated there since, destroying that one while still in use (seen
// as exploded geometry until the game restarted).
void PatchViewDepth(uint8_t* drawContext) {
    auto view = mem::SafeRead<uint8_t*>(drawContext + kDrawContextView);
    if (!view || !*view) return;
    auto targets = mem::SafeRead<uint8_t*>(*view + kViewTargets);
    if (!targets || !*targets) return;
    auto depth = mem::SafeRead<uint8_t*>(*targets + kTargetsDepthCopy);
    if (!depth || !*depth) return;
    auto** depthNative = reinterpret_cast<void**>(*depth + kTextureDepthNative);
    auto colorNative = mem::SafeRead<void*>(*depth + kTextureNative);
    auto current = mem::SafeRead<void*>(depthNative);
    if (!colorNative || !*colorNative || !current || *current) return;
    auto* native = static_cast<uint8_t*>(*colorNative);
    auto count = mem::SafeRead<LONG>(native + kNativeRefCount);
    if (!count || *count <= 0) return;  // not a live native
    // The pass can be built on two threads at once; only the thread that installs the alias keeps
    // the reference it took.
    auto* refs = reinterpret_cast<volatile LONG*>(native + kNativeRefCount);
    LONG references = InterlockedIncrement(refs);
    if (InterlockedCompareExchangePointer(depthNative, *colorNative, nullptr) != nullptr) {
        InterlockedDecrement(refs);
        return;
    }
    LOGD("game: depth copy texture {} given its native {} as depth view (references {})", static_cast<void*>(*depth),
         *colorNative, references);
}

// The Halton entry the TAA update gives the camera this frame (scene + 0xe6a0, frame % 20).
const float* HaltonForFrame(uint8_t* renderer, uint8_t* scene) {
    uint64_t frame = *reinterpret_cast<uint64_t*>(renderer + kRendererFrame);
    return reinterpret_cast<const float*>(scene + kSceneHalton + (frame % kHaltonCount) * 8);
}

// Besides the enable switch, the TAA update only jitters when the scene's TAA settings say so
// (blend rate, filter type and flags). DLSS needs the jitter whenever it runs, so when the
// update left the camera alone and DLSS ran in the last frames, the jitter is set here the same
// way the update does it. Returns false when the camera keeps its value.
bool EnsureJitter(uint8_t* taa) {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    uint8_t* camera = g_camera ? *g_camera : nullptr;
    uint8_t* scene = g_scene ? *g_scene : nullptr;
    if (!renderer || !camera || !scene) return false;
    const float* halton = HaltonForFrame(renderer, scene);
    auto* jitter = reinterpret_cast<float*>(camera + kCameraJitter);
    if (jitter[0] == halton[0] && jitter[1] == halton[1]) return true;  // set by the update

    uint64_t frame = *reinterpret_cast<uint64_t*>(renderer + kRendererFrame);
    uint64_t last = g_lastDlssFrame.load();
    if (!last || frame + 1 > last + 4) return false;  // DLSS is not running
    auto view = mem::SafeRead<uint8_t*>(renderer + kRendererView);
    if (!view || !*view) return false;
    auto flags = mem::SafeRead<uint32_t>(*view + kViewFlags);
    if (!flags || (*flags & kViewSkipsTaa)) return false;

    jitter[0] = halton[0];
    jitter[1] = halton[1];
    *reinterpret_cast<uint32_t*>(camera + kCameraJitterFrame) =
        *reinterpret_cast<uint32_t*>(renderer + kRendererRenderFrame);
    ++g_jitterSetByPlugin;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOGD("jitter: the TAA update did not jitter the camera (blend {:.3f}, type {}, flags {:#x}, enabled {}); "
             "the plugin does while DLSS runs",
             mem::SafeRead<float>(taa + 0x180).value_or(-1.0f), mem::SafeRead<uint32_t>(taa + 0xc).value_or(0),
             mem::SafeRead<uint32_t>(taa + 0x14).value_or(0), mem::SafeRead<uint8_t>(taa + kTaaJitterEnable).value_or(0));
    }
    return true;
}

// The game turns the Halton value h into a clip-space offset of h / output size, which moves the
// image by h / 2 * render scale render pixels: its jitter covers less than half a pixel. With
// DLSS the camera value is replaced right after the TAA update sets it, by a sample that moves the
// image by up to half a render pixel each way, so the samples spread over a whole render pixel.
//
// The game's sequence has 20 phases (Halton(2,3) indices 20..39). DLSS needs 8 per output pixel
// covered by a render pixel (DLSS guide, 3.7.1): 18 in Quality, 24 in Balanced, 32 in Performance.
// With 20 phases (DLAA, Quality, or JitterPhases=20) the plugin keeps the game's table; otherwise it
// writes Halton(2,3) from index 1 with max(20, 8 / scale^2) phases. Only with FullPixelJitter.
float Halton(uint32_t index, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

uint32_t JitterPhases(float scale) {
    int setting = g_settings.jitterPhases;
    if (setting > 0) return static_cast<uint32_t>(setting);
    if (setting == 0) return static_cast<uint32_t>(kHaltonCount);
    auto wanted = static_cast<uint32_t>(std::ceil(8.0f / (scale * scale) - 0.01f));
    return std::max(static_cast<uint32_t>(kHaltonCount), wanted);
}

void WidenJitter() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    uint8_t* camera = g_camera ? *g_camera : nullptr;
    uint8_t* scene = g_scene ? *g_scene : nullptr;
    if (!renderer || !camera || !scene) return;
    const float* halton = HaltonForFrame(renderer, scene);
    auto* jitter = reinterpret_cast<float*>(camera + kCameraJitter);
    // Only a value the update has just written; a second filter instance may not rewrite it.
    if (jitter[0] != halton[0] || jitter[1] != halton[1]) return;
    float scale = RenderScale();
    float factor = 2.0f / scale;
    uint32_t phases = JitterPhases(scale);
    if (phases == kHaltonCount) {
        jitter[0] *= factor;
        jitter[1] *= factor;
        return;
    }
    uint64_t frame = *reinterpret_cast<uint64_t*>(renderer + kRendererFrame);
    auto index = static_cast<uint32_t>(frame % phases) + 1;
    jitter[0] = (Halton(index, 2) - 0.5f) * factor;
    jitter[1] = (Halton(index, 3) - 0.5f) * factor;
}

// The scene's anti-aliasing switches, TAA in the low byte and FXAA in the high byte of a word at
// scene + g_sceneAa. The game sets them from the Anti-Aliasing option when it applies graphics
// options (+0x43dcb0), and the TAA and FXAA filters follow them every frame. With DLSS on, that
// apply sets TAA only for render scales below 1, but at 1 (DLAA, which the stock game never had)
// it keeps the option, which the game greys out while DLSS is on: FXAA then ran over the DLSS
// output, and Off or FXAA turned the TAA filter off, and with it DLSS, which runs inside it. So
// while DLSS runs the switches are held at TAA only.
constexpr uint16_t kAaTaaOnly = 0x0001;
ptrdiff_t g_sceneAa = 0;
std::mutex g_aaMutex;
uint8_t* g_aaScene = nullptr;
bool g_aaHeld = false;
uint16_t g_aaGame = 0;  // the game's own setting, last seen
bool g_aaOurs = false;  // the word still holds what the plugin wrote (the game has not set it since)

// Bypass shows the game's TAA instead of DLSS, in DLAA only: the upscaling modes need the TAA
// pass and the jitter for DLSS at the DLISP position either way.
bool Bypassed() { return g_settings.bypass && !DlispActive(); }

std::string AaName(uint16_t value) {
    switch (value) {
        case 0x0000: return "Off";
        case 0x0001: return "TAA";
        case 0x0100: return "FXAA";
        case 0x0101: return "TAA+FXAA";
        default: return std::format("{:#06x}", value);
    }
}

void HoldTaaOnly() {
    uint8_t* scene = g_scene ? *g_scene : nullptr;
    if (!scene || !g_sceneAa) return;
    std::lock_guard lock(g_aaMutex);
    if (scene != g_aaScene) {
        g_aaScene = scene;
        g_aaHeld = false;
        g_aaOurs = false;
    }
    auto* word = reinterpret_cast<volatile uint16_t*>(scene + g_sceneAa);
    uint16_t current = *word;
    if (current != kAaTaaOnly) g_aaOurs = false;  // the game has set it since
    bool dlss = DlssEnabledInGame();
    bool bypassed = Bypassed();
    bool unavailable = g_dlssUnavailable;
    if (dlss && !bypassed && !unavailable) {
        // Any other value was set by the game (at start, or when it applied the options again).
        if (current != kAaTaaOnly) {
            if (!g_aaHeld || current != g_aaGame)
                LOGD("anti-aliasing: the game set {}; TAA only while DLSS runs", AaName(current));
            g_aaGame = current;
            *word = kAaTaaOnly;
            g_aaOurs = true;
        } else if (!g_aaHeld && !g_aaOurs) {
            g_aaGame = current;  // TAA only is the game's own setting
        }
        g_aaHeld = true;
    } else if (g_aaHeld) {
        g_aaHeld = false;
        // Turning DLSS off makes the game apply the option again (after clearing the DLSS state
        // checked above), so only a hold the plugin ends itself has a setting to give back. When
        // NGX failed, the plugin has turned DLSS off already (render scale recompute) by now.
        if ((dlss || unavailable) && g_aaOurs && g_aaGame != kAaTaaOnly) {
            *word = g_aaGame;
            g_aaOurs = false;
            LOGD("anti-aliasing: back to the game's {} ({})", AaName(g_aaGame),
                 unavailable ? "DLSS is not available" : "Bypass");
        }
    }
}

// The scene's mSSLRGBufferJitter switch (scene + g_sceneSslrJitter, on in the game) picks the
// "_GJitter" versions of the screen-space reflection passes (classify, trace, resolve). Their
// resolve fetches each pixel's reflection history from a neighbour inside its aligned 2x2 block,
// a different one every frame in a four-frame cycle (from the frame counter), and the next pass
// keeps 99.99% of that history (clamped to the neighbours). That spreads the history over each
// 2x2 block of render pixels: the reflections get nearly constant per block and change every
// frame. Measured: inside a block, neighbouring pixels of the history differ by a third of what
// they differ by across a block border. The game's TAA at native resolution hid it; DLSS scales
// the blocks up instead (4x4 screen pixels in Performance) and its input changes twice as much
// between frames where reflections dominate. The resolve without the switch is the same shader
// without that fetch offset, so the switch is held off while DLSS runs (developer key
// ReflectionFix, on by default).
ptrdiff_t g_sceneSslrJitter = 0;
std::mutex g_sslrMutex;
uint8_t* g_sslrScene = nullptr;
bool g_sslrHeld = false;
uint8_t g_sslrGame = 1;  // the game's own value, last seen
bool g_sslrOurs = false; // the byte still holds the plugin's 0 (the game has not set it since)
bool g_sslrLogged = false;

void HoldReflectionJitterOff() {
    uint8_t* scene = g_scene ? *g_scene : nullptr;
    if (!scene || !g_sceneSslrJitter) return;
    std::lock_guard lock(g_sslrMutex);
    if (scene != g_sslrScene) {
        g_sslrScene = scene;
        g_sslrHeld = false;
        g_sslrOurs = false;
        g_sslrLogged = false;
    }
    auto* flag = reinterpret_cast<volatile uint8_t*>(scene + g_sceneSslrJitter);
    uint8_t current = *flag;
    if (current != 0) g_sslrOurs = false;  // the game has set it since
    if (g_settings.reflectionFix && DlssEnabledInGame() && !Bypassed() && !g_dlssUnavailable) {
        if (current != 0) {
            // Once per hold: the game sets the switch when the scene starts, maybe after the hold began.
            if (!g_sslrLogged) LOGD("reflections: the game's 2x2 reflection filter is off while DLSS runs");
            g_sslrLogged = true;
            g_sslrGame = current;
            *flag = 0;
            g_sslrOurs = true;
        } else if (!g_sslrHeld && !g_sslrOurs) {
            g_sslrGame = 0;  // off is the game's own setting
        }
        g_sslrHeld = true;
    } else if (g_sslrHeld) {
        g_sslrHeld = false;
        g_sslrLogged = false;
        if (g_sslrOurs && g_sslrGame != 0) {
            *flag = g_sslrGame;
            LOGD("reflections: back to the game's filter");
        }
        g_sslrOurs = false;
    }
}

// Texture mip bias. The game builds 15 sampler banks when it loads its shaders (+0x23d6690): bank 0
// without bias, banks 1..7 at -log2(2, 2.5, 3, 3.5, 4, 4.5, 5) mip levels, banks 8..14 the same
// positive. A material takes its samplers from the bank the scene's mSamplerLodBias names
// (scene + g_sceneSamplerBank) when it is loaded (+0x2383566), so a bank set here applies to areas
// loaded afterwards. With DLSS the textures should be sampled for the output resolution: NVIDIA
// recommends log2(render scale) - 1 (the game's own dynamic resolution uses the same), which the
// banks match closely: DLAA -1, Quality -1.58, Balanced -1.81, Performance -2. The bank is held from
// the first frame on: materials loaded before the title's first TAA pass (about 18 s later) stay
// resident into the hub, and made up about a fifth of its material samplers.
ptrdiff_t g_sceneSamplerBank = 0;
std::mutex g_bankMutex;
uint8_t* g_bankScene = nullptr;
bool g_bankHeld = false;
uint32_t g_bankGame = 0;   // the game's own bank, last seen
uint32_t g_bankOurs = 0;   // the bank the plugin wrote (0 = none)
uint32_t g_banksUsed = 0;  // banks the plugin has set this session, one bit each
bool g_bankRoomLogged = false;

// The D3D12 device's sampler table cache (device +0x1370, made once with the device by +0x23a48e0):
// +0xc the next free slot, +0x10 the capacity (2048, the size of the shader-visible sampler heap
// behind it). Each new combination of samplers a draw binds takes slots for the rest of the session:
// the lookup (+0x2624870) appends without a bound check and nothing frees them. A bank the materials
// use adds its own combinations (Astera with one bank: 260..360 slots in all), so a bank not set yet
// this session is only set while this many slots are left.
constexpr ptrdiff_t kDeviceSamplerCache = 0x1370;
constexpr ptrdiff_t kSamplerCacheNext = 0xc;
constexpr ptrdiff_t kSamplerCacheCapacity = 0x10;
constexpr uint32_t kSamplerCacheReserve = 512;

struct SamplerCacheUse {
    uint32_t next = 0;
    uint32_t capacity = 0;
};

// D3D12 only: D3D11 binds samplers without such a cache.
std::optional<SamplerCacheUse> SamplerCache() {
    uint8_t* device = Device();
    if (!device || IsDx12() != true) return std::nullopt;
    auto cache = mem::SafeRead<uint8_t*>(device + kDeviceSamplerCache).value_or(nullptr);
    if (!cache) return std::nullopt;
    auto next = mem::SafeRead<uint32_t>(cache + kSamplerCacheNext);
    auto capacity = mem::SafeRead<uint32_t>(cache + kSamplerCacheCapacity);
    if (!next || !capacity) return std::nullopt;
    return SamplerCacheUse{*next, *capacity};
}

// The renderer's DLSS type and applied render scale agree (DLISP exactly below 1.0). At start-up the
// game sets the type a frame before the scale (measured: 0.34 s), and a bank picked in between would
// be DLAA's.
bool RenderScaleSettled() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer) return false;
    auto bits = mem::SafeRead<uint32_t>(renderer + kRendererDlssBits).value_or(0);
    auto scale = mem::SafeRead<float>(renderer + kRendererScaleApplied).value_or(1.0f);
    return ((bits & 2) != 0) == (scale < 1.0f);
}

uint32_t BankFor(float bias) {
    static const std::array<float, 8> kBias = {0.0f,           -1.0f,          -std::log2(2.5f), -std::log2(3.0f),
                                               -std::log2(3.5f), -std::log2(4.0f), -std::log2(4.5f), -std::log2(5.0f)};
    uint32_t best = 0;
    for (uint32_t i = 1; i < kBias.size(); ++i)
        if (std::abs(kBias[i] - bias) < std::abs(kBias[best] - bias)) best = i;
    return best;
}

void HoldMipBias() {
    uint8_t* scene = g_scene ? *g_scene : nullptr;
    if (!scene || !g_sceneSamplerBank) return;
    std::lock_guard lock(g_bankMutex);
    if (scene != g_bankScene) {
        g_bankScene = scene;
        g_bankHeld = false;
        g_bankOurs = 0;
    }
    auto* bank = reinterpret_cast<volatile uint32_t*>(scene + g_sceneSamplerBank);
    uint32_t current = *bank;
    if (g_bankOurs && current != g_bankOurs) g_bankOurs = 0;  // the game has set it since
    float bias = g_settings.mipBias;
    float scale = RenderScale();
    if (std::isnan(bias)) bias = std::log2(scale) - 1.0f;
    uint32_t wanted = BankFor(bias);
    if (DlssEnabledInGame() && !Bypassed() && !g_dlssUnavailable && wanted != 0) {
        if (current != wanted && RenderScaleSettled()) {
            auto cache = SamplerCache();
            bool used = (g_banksUsed & (1u << wanted)) != 0;
            if (used || !cache || cache->next + kSamplerCacheReserve <= cache->capacity) {
                if (!g_bankOurs) g_bankGame = current;
                *bank = wanted;
                g_bankOurs = wanted;
                g_banksUsed |= 1u << wanted;
                std::string use = cache ? std::format("; sampler cache {}/{}", cache->next, cache->capacity) : "";
                LOGD("textures: mip bias bank {} ({:.2f} levels, render scale {:.3f}) for areas loaded from now on{}",
                     wanted, -std::log2(1.5f + 0.5f * static_cast<float>(wanted)), scale, use);
            } else if (!g_bankRoomLogged) {
                g_bankRoomLogged = true;
                LOGW("textures: the game's sampler cache has {} of {} slots left; areas loaded from now on keep "
                     "mip bias bank {}",
                     cache->capacity > cache->next ? cache->capacity - cache->next : 0, cache->capacity, current);
            }
        }
        g_bankHeld = true;
    } else if (g_bankHeld) {
        g_bankHeld = false;
        if (g_bankOurs) {
            *bank = g_bankGame;
            g_bankOurs = 0;
            LOGD("textures: the game's mip bias bank {} for areas loaded from now on", g_bankGame);
        }
    }
}

// The pass can be built on two threads at once for the same filter, so the TAA sharpen flag is
// cleared by the first build that starts and restored by the last one that ends.
struct SharpenHold {
    int builds = 0;
    uint8_t saved = 0;
    bool cleared = false;
};
std::mutex g_sharpenMutex;
std::unordered_map<uint8_t*, SharpenHold> g_sharpenHolds;

class SharpenGuard {
public:
    SharpenGuard(uint8_t* taa, bool suppress) : taa_(taa) {
        std::lock_guard lock(g_sharpenMutex);
        SharpenHold& h = g_sharpenHolds[taa];
        if (h.builds++ > 0) return;
        h.saved = taa[kTaaSharpen];
        h.cleared = suppress && h.saved;
        if (!h.cleared) return;
        taa[kTaaSharpen] = 0;
        ++g_sharpenSuppressed;
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGD("game: TAA sharpen pass (amount {:.3f}) turned off while DLSS runs",
                 mem::SafeRead<float>(taa + 0x198).value_or(-1.0f));
        }
    }
    ~SharpenGuard() {
        std::lock_guard lock(g_sharpenMutex);
        auto it = g_sharpenHolds.find(taa_);
        if (it == g_sharpenHolds.end() || --it->second.builds > 0) return;
        if (it->second.cleared) taa_[kTaaSharpen] = it->second.saved;
        g_sharpenHolds.erase(it);
    }
    SharpenGuard(const SharpenGuard&) = delete;
    SharpenGuard& operator=(const SharpenGuard&) = delete;

private:
    uint8_t* taa_;
};

// Scene parameters may turn the TAA jitter off (the title screen does). DLSS needs the
// sub-pixel jitter, so it is switched on for the duration of the update that applies it.
// The update runs every frame, also while the TAA filter is switched off.
void* __fastcall HookTaaUpdate(void* self) {
    if (!g_active) return g_origTaaUpdate(self);
    if (g_updateCallback) g_updateCallback();
    HoldTaaOnly();
    HoldReflectionJitterOff();
    HoldMipBias();
    if (!DlssEnabledInGame() || Bypassed() || g_dlssUnavailable) return g_origTaaUpdate(self);
    auto* taa = static_cast<uint8_t*>(self);
    auto* enable = taa + kTaaJitterEnable;
    uint8_t saved = *enable;
    bool forceJitter = g_settings.forceJitter;
    if (forceJitter) *enable = 1;
    void* result = g_origTaaUpdate(self);
    *enable = saved;
    ++g_jitterFrames;
    if (g_settings.debugFixedJitter) {
        // Constant jitter for checking the sign convention (DLSS guide, section 8.4.2).
        if (uint8_t* camera = g_camera ? *g_camera : nullptr) {
            auto* jitter = reinterpret_cast<float*>(camera + kCameraJitter);
            jitter[0] = g_settings.debugJitterX;
            jitter[1] = g_settings.debugJitterY;
        }
        return result;
    }
    if (forceJitter) EnsureJitter(taa);
    if (g_settings.fullPixelJitter) WidenJitter();
    return result;
}

void* __fastcall HookTaaDraw(void* self, void* drawContext) {
    if (!g_active) return g_origTaaDraw(self, drawContext);
    static std::atomic<uint64_t> calls{0};
    if (calls.fetch_add(1) == 0) LOGD("game: first TAA draw (filter {})", self);
    if (g_frameCallback) g_frameCallback();
    auto* taa = static_cast<uint8_t*>(self);
    bool dlss = DlssEnabledInGame();
    // With the TAA sharpen pass on, the game points DLSS at the next history slot and then has the
    // sharpen pass write the sharpened TAA result over it. The pass stays off while DLSS runs.
    SharpenGuard sharpen(taa, dlss && !Bypassed() && !g_dlssUnavailable);
    if (dlss) PatchViewDepth(static_cast<uint8_t*>(drawContext));
    CaptureJitter(taa);
    return g_origTaaDraw(self, drawContext);
}

uint8_t* ExeBase() { return reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)); }

uintptr_t Rva(const void* p) { return static_cast<uintptr_t>(static_cast<const uint8_t*>(p) - ExeBase()); }

void* __fastcall HookMainUpdate(void* self) {
    if (g_active) {
        // Runs from the first frame, long before the first TAA update.
        HoldMipBias();
        if (g_frameStartCallback) g_frameStartCallback();
    }
    return g_origMainUpdate(self);
}

void* __fastcall HookMouseUpdate(void* self) {
    if (g_active && g_inputCallback) g_inputCallback();
    return g_origMouseUpdate(self);
}

// The only .rdata slot pointing at `fn` (a vtable entry), or null when there is none or several.
void** UniqueSlotOf(const mem::Section& rdata, const void* fn) {
    void** found = nullptr;
    for (auto* p = reinterpret_cast<void**>(rdata.begin); p + 1 <= reinterpret_cast<void**>(rdata.end); ++p) {
        if (*p != fn) continue;
        if (found) return nullptr;
        found = p;
    }
    return found;
}

// Hooks a per-frame update through its vtable slot. Returns false when the function or its slot is
// not found; the plugin then runs without it.
bool HookFrameUpdate(const mem::Section& text, const mem::Section& rdata, std::string_view pattern,
                     const char* name, void* hook, FrameUpdateFn& original) {
    auto matches = mem::FindPattern(text, pattern);
    void** slot = matches.size() == 1 ? UniqueSlotOf(rdata, matches[0]) : nullptr;
    if (!slot) {
        LOGW("game: {} not found ({} matches); NVIDIA Reflex works without it", name, matches.size());
        return false;
    }
    if (!mem::HookSlot(slot, hook, reinterpret_cast<void**>(&original))) {
        LOGW("game: {} vtable slot +{:#x} could not be hooked; NVIDIA Reflex works without it", name, Rva(slot));
        return false;
    }
    LOGD("game: {} +{:#x} hooked through vtable slot +{:#x}", name, Rva(matches[0]), Rva(slot));
    return true;
}

}  // namespace

bool Init() {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto text = mem::FindSection(exe, ".text");
    auto rdata = mem::FindSection(exe, ".rdata");
    if (!text || !rdata) {
        LOGE("game: executable sections not found");
        return false;
    }

    auto updates = mem::FindPattern(*text, kTaaUpdatePattern);
    if (updates.size() != 1) {
        LOGE("game: TAA update function matched {} times (expected 1); unsupported game version?",
             updates.size());
        return false;
    }
    uint8_t* update = updates[0];
    g_renderer = reinterpret_cast<uint8_t**>(mem::RipTarget(update + 0x07, update + 0x0b));
    g_scene = reinterpret_cast<uint8_t**>(mem::RipTarget(update + 0x49, update + 0x4d));
    g_camera = reinterpret_cast<uint8_t**>(mem::RipTarget(update + 0x55, update + 0x59));
    LOGD("game: TAA update at +{:#x}, renderer global +{:#x}, scene global +{:#x}, camera global +{:#x}",
         Rva(update), Rva(g_renderer), Rva(g_scene), Rva(g_camera));

    // The TAA vtable is the only place in .rdata that points at the update method.
    void** updateSlot = nullptr;
    for (auto* p = reinterpret_cast<void**>(rdata->begin); p + 1 <= reinterpret_cast<void**>(rdata->end); ++p) {
        if (*p == update) {
            if (updateSlot) {
                LOGE("game: TAA update method referenced from more than one vtable");
                return false;
            }
            updateSlot = p;
        }
    }
    if (!updateSlot) {
        LOGE("game: TAA vtable not found");
        return false;
    }
    void** drawSlot = updateSlot + kDrawSlotFromUpdate;
    auto drawCheck = mem::FindPattern(
        mem::Section{static_cast<uint8_t*>(*drawSlot), static_cast<uint8_t*>(*drawSlot) + 0x20}, kTaaDrawPrologue);
    if (drawCheck.size() != 1 || drawCheck[0] != *drawSlot) {
        LOGE("game: TAA draw slot +{:#x} does not point at the expected function", Rva(drawSlot));
        return false;
    }
    if (!mem::HookSlot(drawSlot, reinterpret_cast<void*>(&HookTaaDraw), reinterpret_cast<void**>(&g_origTaaDraw)) ||
        !mem::HookSlot(updateSlot, reinterpret_cast<void*>(&HookTaaUpdate), reinterpret_cast<void**>(&g_origTaaUpdate))) {
        LOGE("game: the TAA vtable slots could not be hooked");
        return false;
    }
    LOGD("game: TAA draw +{:#x} hooked through vtable slot +{:#x}", Rva(g_origTaaDraw), Rva(drawSlot));

    auto recompute = mem::FindPattern(*text, kRecomputePattern);
    if (recompute.size() == 1) {
        g_recompute = reinterpret_cast<RecomputeFn>(recompute[0]);
        LOGD("game: render scale update at +{:#x}", Rva(recompute[0]));
    } else {
        LOGW("game: render scale update matched {} times; scale changes need a settings toggle",
             recompute.size());
    }

    auto aaSetter = mem::FindPattern(*text, kSceneAaSetterPattern);
    if (aaSetter.size() == 1) {
        int32_t offset;
        std::memcpy(&offset, aaSetter[0] + kSceneAaSetterDisp, sizeof(offset));
        g_sceneAa = offset;
        LOGD("game: anti-aliasing switches at scene +{:#x} (setter +{:#x})", offset, Rva(aaSetter[0]));
    } else {
        LOGW("game: anti-aliasing setter matched {} times; the Anti-Aliasing option stays in effect with DLSS",
             aaSetter.size());
    }

    auto sslrSelect = mem::FindPattern(*text, kSslrResolveSelectPattern);
    if (sslrSelect.size() == 1) {
        int32_t offset;
        std::memcpy(&offset, sslrSelect[0] + kSslrGBufferJitterDisp, sizeof(offset));
        g_sceneSslrJitter = offset;
        LOGD("game: reflection filter switch at scene +{:#x} (+{:#x})", offset, Rva(sslrSelect[0]));
    } else {
        LOGW("game: reflection technique choice matched {} times; ReflectionFix has no effect", sslrSelect.size());
    }

    auto samplerBank = mem::FindPattern(*text, kSamplerBankPattern);
    if (samplerBank.size() == 1 && mem::RipTarget(samplerBank[0] + 3, samplerBank[0] + 7) == reinterpret_cast<uint8_t*>(g_scene)) {
        int32_t offset;
        std::memcpy(&offset, samplerBank[0] + kSamplerBankDisp, sizeof(offset));
        g_sceneSamplerBank = offset;
        LOGD("game: sampler bank at scene +{:#x} (+{:#x})", offset, Rva(samplerBank[0]));
    } else {
        LOGW("game: material sampler resolve matched {} times; the texture mip bias stays the game's",
             samplerBank.size());
    }

    auto apiFlag = mem::FindPattern(*text, kApiFlagPattern);
    if (apiFlag.size() == 1) {
        g_apiFlag = mem::RipTarget(apiFlag[0] + 0x13, apiFlag[0] + 0x17);
        // Set when the renderer starts, which may be after the plugin is loaded.
        if (Device())
            LOGD("game: renderer API flag at +{:#x} (DX12 = {})", Rva(g_apiFlag), *g_apiFlag != 0);
        else
            LOGD("game: renderer API flag at +{:#x} (renderer not started yet)", Rva(g_apiFlag));
    } else {
        LOGW("game: renderer API flag matched {} times", apiFlag.size());
    }

    auto opNew = mem::FindPattern(*text, kOperatorNewPattern);
    if (opNew.size() == 1) {
        g_operatorNew = reinterpret_cast<OperatorNewFn>(opNew[0]);
    } else {
        LOGW("game: operator new matched {} times", opNew.size());
    }

    // For NVIDIA Reflex: the start of each frame, the input poll and the render thread's events.
    HookFrameUpdate(*text, *rdata, kMainUpdatePattern, "frame update", reinterpret_cast<void*>(&HookMainUpdate),
                    g_origMainUpdate);
    HookFrameUpdate(*text, *rdata, kMouseUpdatePattern, "mouse update", reinterpret_cast<void*>(&HookMouseUpdate),
                    g_origMouseUpdate);
    auto renderThread = mem::FindPattern(*text, kRenderThreadPattern);
    if (renderThread.size() == 1 &&
        mem::RipTarget(renderThread[0] + 3, renderThread[0] + 7) == reinterpret_cast<uint8_t*>(g_renderer)) {
        g_setEventImport = reinterpret_cast<void**>(mem::RipTarget(renderThread[0] + 16, renderThread[0] + 20));
        g_waitImport = reinterpret_cast<void**>(mem::RipTarget(renderThread[0] + 41, renderThread[0] + 45));
        LOGD("game: render thread loop at +{:#x}, SetEvent import +{:#x}, WaitForSingleObject import +{:#x}",
             Rva(renderThread[0]), Rva(g_setEventImport), Rva(g_waitImport));
    } else {
        LOGW("game: render thread loop matched {} times", renderThread.size());
    }
    return true;
}

std::optional<bool> IsDx12() {
    if (!g_apiFlag) return std::nullopt;
    return *g_apiFlag != 0;
}

void* ImmediateContext11() {
    uint8_t* device = Device();
    if (!device) return nullptr;
    return mem::SafeRead<void*>(device + kDeviceImmediateContext11).value_or(nullptr);
}

void* GameAlloc(size_t size) { return g_operatorNew ? g_operatorNew(size) : nullptr; }

void SetFrameCallback(void (*callback)()) { g_frameCallback = callback; }

void SetUpdateCallback(void (*callback)()) { g_updateCallback = callback; }

void SetFrameStartCallback(void (*callback)()) { g_frameStartCallback = callback; }

void SetInputCallback(void (*callback)()) { g_inputCallback = callback; }

void* SwapChain() {
    uint8_t* device = Device();
    auto dx12 = IsDx12();
    if (!device || !dx12) return nullptr;
    if (mem::SafeRead<uint32_t>(device + kDeviceSwapChainCount).value_or(0) == 0) return nullptr;
    return mem::SafeRead<void*>(device + (*dx12 ? kDeviceSwapChain12 : kDeviceSwapChain11)).value_or(nullptr);
}

RenderEvents RenderThreadEvents() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer) return {};
    return {mem::SafeRead<void*>(renderer + kRendererStartEvent).value_or(nullptr),
            mem::SafeRead<void*>(renderer + kRendererDoneEvent).value_or(nullptr)};
}

void** SetEventImport() { return g_setEventImport; }

void** WaitForSingleObjectImport() { return g_waitImport; }

void ApplyConfig(const Config& config) {
    g_settings.bypass = config.bypass;
    g_settings.forceJitter = config.forceJitter;
    g_settings.fullPixelJitter = config.fullPixelJitter;
    g_settings.verboseFrames = config.verboseFrames;
    g_settings.debugFixedJitter = config.debugFixedJitter;
    g_settings.debugJitterX = config.debugJitterX;
    g_settings.debugJitterY = config.debugJitterY;
    g_settings.reflectionFix = config.reflectionFix;
    g_settings.jitterPhases = config.jitterPhases;
    g_settings.mipBias = config.mipBias;
}

void Activate(const Config& config) {
    ApplyConfig(config);
    g_active = true;
}

void Deactivate() { g_active = false; }

uint64_t Frame() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    return renderer ? mem::SafeRead<uint64_t>(renderer + kRendererFrame).value_or(0) : 0;
}

void SetDlssUnavailable(bool unavailable) { g_dlssUnavailable = unavailable; }

uint8_t* Device() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer) return nullptr;
    return mem::SafeRead<uint8_t*>(renderer + kRendererDevice).value_or(nullptr);
}

DlssContext* Context() {
    uint8_t* device = Device();
    if (!device) return nullptr;
    return mem::SafeRead<DlssContext*>(device + kDeviceDlssContext).value_or(nullptr);
}

void SetContext(DlssContext* ctx) {
    if (uint8_t* device = Device()) *reinterpret_cast<DlssContext**>(device + kDeviceDlssContext) = ctx;
}

bool RecomputeRenderScale() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer || !g_recompute) return false;
    g_recompute(renderer);
    return true;
}

RendererState ReadState() {
    RendererState s;
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer) return s;
    auto u8 = [&](ptrdiff_t off) { return mem::SafeRead<uint8_t>(renderer + off).value_or(0); };
    auto u32 = [&](ptrdiff_t off) { return mem::SafeRead<uint32_t>(renderer + off).value_or(0); };
    auto f32 = [&](ptrdiff_t off) { return mem::SafeRead<float>(renderer + off).value_or(0.0f); };
    s.exists = true;
    s.dlssOn = u8(kRendererDlssOn);
    s.dlispOn = u8(kRendererDlispOn);
    s.mode = u32(0xa8);
    s.dlssScale = f32(0xb0);
    s.featureBits = u32(kRendererDlssBits);
    s.scaleRequested = f32(0x200);
    s.scaleApplied = f32(kRendererScaleApplied);
    s.context = Context();
    return s;
}

std::string DescribeState(const RendererState& s) {
    if (!s.exists) return "renderer not created";
    return std::format(
        "DLSS on {}, DLISP on {}, mode {}, DLSS scale {:.3f}, feature bits {}, render scale {:.3f}/{:.3f}, "
        "context {}",
        s.dlssOn, s.dlispOn, s.mode, s.dlssScale, s.featureBits, s.scaleRequested, s.scaleApplied, s.context);
}

float RenderScale() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    float scale = renderer ? mem::SafeRead<float>(renderer + kRendererScaleApplied).value_or(1.0f) : 1.0f;
    return std::clamp(scale, 0.25f, 1.0f);
}

bool DlispActive() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer || !DlssEnabledInGame()) return false;
    auto on = mem::SafeRead<uint8_t>(renderer + kRendererDlispOn).value_or(0);
    auto bits = mem::SafeRead<uint32_t>(renderer + kRendererDlssBits).value_or(0);
    auto scale = mem::SafeRead<float>(renderer + kRendererScaleApplied).value_or(1.0f);
    // Same test as the game's DLISP pass.
    return on && (bits & 2) && scale != 1.0f;
}

bool DlssOptionOn() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    return renderer && mem::SafeRead<uint8_t>(renderer + kRendererDlssOn).value_or(0) != 0;
}

bool DlssEnabledInGame() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer) return false;
    auto on = mem::SafeRead<uint8_t>(renderer + kRendererDlssOn).value_or(0);
    auto bits = mem::SafeRead<uint32_t>(renderer + kRendererDlssBits).value_or(0);
    return on && (bits & 1);
}

void NoteDlssEvaluated() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer) return;
    g_lastDlssFrame = mem::SafeRead<uint64_t>(renderer + kRendererFrame).value_or(0) + 1;
}

std::string JitterStats() {
    uint64_t frames = g_jitterFrames.exchange(0);
    uint64_t byPlugin = g_jitterSetByPlugin.exchange(0);
    uint64_t sharpen = g_sharpenSuppressed.exchange(0);
    return std::format("camera jittered by the plugin in {} of {} frames, TAA sharpen suppressed in {}", byPlugin,
                       frames, sharpen);
}

std::optional<Jitter> JitterForOutput(const void* outputResource) {
    std::lock_guard lock(g_jitterMutex);
    const JitterRecord* best = nullptr;
    for (const auto& rec : g_jitterRecords) {
        if (rec.resource11 != outputResource && rec.resource12 != outputResource) continue;
        if (!best || rec.jitter.frame > best->jitter.frame) best = &rec;
    }
    if (!best) return std::nullopt;
    return best->jitter;
}

}  // namespace mhwdlss::game
