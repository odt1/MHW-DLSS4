# MHW-DLSS4: technical notes

For developers. Players only need the [README](../README.md).

## How it works

The game links a static NGX 1.3 library and, at start-up, initialises the driver's NGX core with its application id. The plugin is loaded later (with the input system) and:

1. Replaces the entries of the game's internal NGX function table (in `.data`) and the game's NGX parameter object, so every DLSS 1 call ends up in the plugin. No game code is patched; the TAA filter is hooked through two vtable entries. When a loader loads the plugin before the game's renderer exists (SharpPluginLoader 2.0 preloads Stracker's Loader), the plugin also answers the game's own NGX init, which looks the NGX functions up through the exe's GetProcAddress import, so the game sets DLSS up through the plugin from the start.
2. Shuts the game's NGX 1.x context down and initialises a current NGX with a project id and the folder of the chosen `nvngx_dlss.dll` as its only search path. With the game's application id, or with the old context still alive, the driver serves NVIDIA's per-game DLSS 1 model over the air instead.
3. At the game's DLSS 1 call inside the TAA pass, runs DLSS with the game's colour, dense motion vectors and depth copy, plus the camera jitter recorded when the pass was built. The game's jitter only covers a fraction of a pixel and scene parameters sometimes switch it off, so the plugin keeps it on and widens it to a full render pixel while DLSS runs, with more phases in the upscaling modes (see [Jitter, history reset and texture sharpness](#jitter-history-reset-and-texture-sharpness)).
4. In the upscaling modes the TAA-position call passes the jittered frame through and DLSS runs where the game calls DLISP, its old upscaler, after post-processing. That is the only place the game's pipeline changes resolution.
5. With sharpening on, DLSS writes into a texture of the plugin and a compute pass (NVSharpen, Vanilla or RCAS) writes the sharpened result into the game's texture.
6. Keeps the TAA filter's own sharpen pass off while DLSS runs. With it on, the game would write the sharpened TAA result over the DLSS output.
7. Works around a crash in the game's never-used DX12 DLSS path: the barrier setup for the DLSS command expects a depth view the depth copy texture does not have. The plugin lends it the texture's colour view and takes a reference of its own, because the game releases both views separately when the texture is rebuilt.
8. Holds the game's anti-aliasing at TAA only while DLSS runs. The game decides its anti-aliasing when graphics options are applied: with DLSS below native resolution it switches to TAA only, but at native resolution (DLAA, which the original game never had) it kept the greyed-out Anti-Aliasing option. FXAA then ran over the DLSS image, and Off or FXAA switched off the TAA pass and with it DLSS.
9. Keeps D3D12 textures and DLSS features it replaces alive for 16 game frames before freeing them. D3D12 frees resources immediately, even while queued GPU work still uses them, which crashed the GPU when the render resolution changed while the game was running.
10. Adds NVIDIA Reflex (see below), independent of DLSS.
11. Switches the game's screen-space reflections to the version of their temporal filter that does not work in 2x2 pixel blocks while DLSS runs (see [Reflections](#reflections)).
12. Resets the DLSS history on camera cuts, which the game does not, and picks the game's sampler bank with the texture mip bias NVIDIA recommends for the render scale (see [Jitter, history reset and texture sharpness](#jitter-history-reset-and-texture-sharpness)).

The game builds the TAA pass on worker threads, twice per frame and at the same time, and runs the TAA update once per frame on the main thread. The plugin reloads its ini and asks the game to recompute the render scale from that update (where the game applies graphics options itself), outside the lock the render thread takes for DLSS. The hooks only act once the NGX side is installed.

## NVIDIA Reflex and input lag

The game's main thread runs one frame per update of its main object (`sMhMain`, exe `+0x1af2b60`, called through its vtable):

1. the frame timer with the limiter of the Frame Rate option (`+0x225f9f0`: `Sleep`, then a `QueryPerformanceCounter` spin);
2. the start of the render thread on the previous frame (`SetEvent` on the renderer's start event);
3. input polling (the mouse through DirectInput in `sMhMouse`'s update) and the simulation;
4. the wait for the render thread; then the previous frame's command lists go to the graphics card and that frame is presented.

A frame is therefore presented one update after its input was read, and anything that waits on the main thread (a frame rate limiter, or Reflex's sleep) waits inside some frame's time from input to screen: a frame rate cap costs about one frame interval of input lag wherever it sleeps.

The plugin uses NVAPI from the driver's `nvapi64.dll` through `nvapi_QueryInterface` (nothing is linked; the declarations are in `third_party/nvapi/nvapi_reflex.h`):

- `NvAPI_D3D_SetSleepMode` with the ini settings: low latency mode, boost, `minimumIntervalUs` for `FrameLimit`, markers used for optimisation.
- `NvAPI_D3D_Sleep` at the start of the update, then `SIMULATION_START`. The update is hooked through `sMhMain`'s vtable slot (`+0x33fa9e8`).
- `INPUT_SAMPLE` when `sMhMouse`'s update runs (its vtable slot `+0x2f86e78`).
- `RENDERSUBMIT_START` and `_END` when the main thread starts the render thread and when the render thread reports that it is done, through the exe's `SetEvent` import; `SIMULATION_END` when the main thread starts waiting for the render thread (`WaitForSingleObject` import). Both imports are found from the render thread's loop (`+0x229ac88`).
- `PRESENT_START` and `_END` around the swap chain's Present (slot 8 of its vtable). The game keeps the swap chain in its device object at `+0x1470` (D3D12) or `+0x1488` (D3D11). The render and present markers carry the id of the frame simulated one update earlier.

NVAPI gets the D3D device from the swap chain. Under ReShade, the D3D12 swap chain hands out the device itself instead of its command queue, so both are tried.

### Measurements

With `LatencyStats=1`, the plugin logs NVIDIA's latency report every 5 seconds (averages over the last 64 frames); `tools/live/latencytest.ps1` switches ini settings in the running game and collects the reports. Title screen, D3D12, 2560x1440, DLAA, RTX 4080 SUPER, time from the input poll until the graphics card has finished the frame:

| Setting | Frame interval | Input to GPU done |
| --- | --- | --- |
| No cap, `LowLatency=Off` (no Sleep calls) | 6.1 ms | 14.6 to 14.9 ms |
| No cap, `LowLatency=On` | 5.8 ms | 13.9 to 14.4 ms |
| No cap, `LowLatency=Boost` | 5.8 ms | 12.5 to 12.6 ms |
| The game's Frame Rate option at 60 | 16.67 ms | 23.9 to 25.9 ms |
| `FrameLimit=60` | 16.67 ms | 23.5 to 23.8 ms |
| Reflex frame limit 60 without Sleep calls (the driver then waits inside Present) | 16.67 ms | 33.2 ms |
| Performance mode, the game's Frame Rate at 60 | 16.67 ms | 20.7 ms |
| NVIDIA DLSS off in the game (no anti-aliasing), no cap | 5.5 ms | 15.2 ms |

In the game world (Astera), `tools/live/latencymatrix.ps1` with `tools/live/mhwharness.py` (key press by `SendInput`, then the player position read until it moves; 15 to 25 tries each). The game reads the key at the start of the next frame and moves the player one frame later; "read to GPU done" is the plugin's report for that frame. Medians, DLAA, RTX 4080 SUPER:

| Setup | Cap | Frame interval | Key to read | Key to player moved | Read to GPU done |
| --- | --- | --- | --- | --- | --- |
| user's mods, D3D12 | none | 7.2 ms | 4.2 to 5.8 ms | 11.7 to 13.3 ms | 17.5 to 19.1 ms |
| user's mods, D3D12 | game 60 | 16.7 ms | 8.7 to 12.2 | 25.5 to 29.0 | 26.8 to 26.9 |
| user's mods, D3D12 | `FrameLimit=60` | 16.7 ms | 8.0 | 24.8 | 26.9 |
| user's mods, D3D12 | driver 60, no Reflex sleep | 16.7 ms | 9.8 to 12.7 | 26.7 to 29.6 | 33.2 |
| user's mods, D3D12 | driver 60, Reflex sleep | 16.7 ms | 10.3 to 12.3 | 27.1 to 29.2 | 26.8 to 26.9 |
| only Stracker's Loader, D3D12 | none | 6.6 ms | 5.8 to 6.3 | 12.5 to 13.3 | 17.4 to 17.7 |
| only Stracker's Loader, D3D12 | game 60 | 16.7 ms | 8.2 to 13.1 | 25.0 to 29.9 | 26.4 |
| only Stracker's Loader, D3D12 | driver 60, no Reflex sleep | 16.7 ms | 6.7 to 13.1 | 23.5 to 29.9 | 33.2 |
| only Stracker's Loader, D3D12 | driver 60, Reflex sleep | 16.7 ms | 12.6 | 29.5 | 26.3 |
| only Stracker's Loader, D3D12 | game 144, driver 144 or `FrameLimit=144` | 6.9 ms | 5.6 to 6.6 | 12.7 to 13.7 | 16.5 to 16.7 |
| only Stracker's Loader, D3D11 | game 60 or driver 60 | 16.7 ms | 8.2 to 11.3 | 25.2 to 28.2 | 24.0 to 24.3 |
| only Stracker's Loader, D3D11 | none | 7.5 ms | 6.3 to 7.2 | 14.2 to 14.9 | 17.7 |

"Only Stracker's Loader" means MHW-DLSS4 and MHW-SteamOffline (for the harness) were the only other mods; ReShade, SharpPluginLoader, Fix-orld, BetterInputs and DPSTickFix were removed. The key-to-read time is the wait for the next frame (half a frame interval on average), so its spread between runs is sampling noise; the read-to-GPU-done time is not. The game's own cap was never worse than the others; the driver's cap without Reflex's sleep was 6.8 ms worse in D3D12 and the same in D3D11. Close to the GPU limit (cap 144 at about 152 fps uncapped) all caps measured the same.

D3D11 at the title: 16.0 ms without Sleep calls, 15.7 On, 15.2 Boost, 23.6 with `FrameLimit=60`. With and without a mod that sets the swap chain's maximum frame latency to 1 the numbers were the same: the game's swap chain already has a maximum frame latency of 1, so frames hardly queue up.

- Low latency mode gains little in this game; Boost about 2 ms through higher GPU clocks.
- The game's limiter sleeps before the input poll (the input is read 14 to 15 ms into the frame), where Reflex sleeps too, and both caps cost the same. The spread of the intervals between Presents was 0.1 to 0.2 ms for every cap.
- Without the game's Sleep call the driver's frame limit waits inside Present, 9.7 ms more at 60 fps. NVIDIA documents that the driver's Max Frame Rate setting sleeps in `NvAPI_D3D_Sleep` when the game calls it; with `LowLatency=On` the Control Panel's 240 fps cap of the test machine moved its wait out of Present (Present took 0.3 ms instead of 1.5 ms without Sleep calls). The Control Panel cap at 60 fps itself was not measured.
- Sleeping right after the render thread start instead of at the start of the update was no better, and with Boost it halved the frame rate to 30 fps.
- DLSS adds no measurable input lag: with DLSS off the game was as fast as with DLAA, and Performance mode lowers the lag along with the GPU time (3 ms instead of 6).
- The driver reported low latency mode on even with `LowLatency=Off` once Sleep was called; the test machine's Control Panel has Low Latency Mode on (maximum pre-rendered frames 1).

## Reflections

Found with RenderDoc captures of the hub (see [Graphics debugging](#graphics-debugging)); the game's shaders carry their resource and constant names. With ScreenSpaceReflection on, the environment reflections (cube map and screen-space reflections together) go through these passes, all at render resolution unless noted:

1. Classify and trace (`SSLR_Classify`, `SSLR_Trace_Accurate`): rays traced at half the render resolution (`fSSLRScale` 0.5; the reflection object's constructor at `+0x25924d0` sets level 1 and scale 0.5, so the `_FULL_RES` techniques never run).
2. Resolve (`SSLR_Resolve*`, compute): four random samples of the trace per pixel, a new pattern every frame (seeded with `iRenderFrame`), plus the cube map; writes the new reflection and the previous result fetched through the motion vectors.
3. Blend (`CBGlobalIllumination`): clamps that previous result to the range of the four neighbours of the new one and keeps 99.99% of it (`fSpecularTemporalBlendRate` 0.9999); the result is added to the scene and kept for the next frame.

The scene's `mSSLRGBufferJitter` switch (scene `+0xb440`, on in the game) picks the `_GJitter` techniques. Their resolve differs from the plain one only by an offset of the history fetch: each pixel reads the history of a neighbour inside its aligned 2x2 block, a different one every frame (none, diagonal, horizontal, vertical, from `iRenderFrame`). With the 99.99% blend that spreads the history over each 2x2 block of render pixels. The game's TAA at native resolution hid it. Measured:

- In the history (native capture), neighbouring pixels inside a 2x2 block differ by a third of what they differ by across a block border; the new reflection of the same frame has no such pattern.
- DLSS Performance output (1440p, hub): in places where reflections are most of the light, pixel differences across 4x4-pixel borders are 6-12% above the average and inside the blocks below it. Turning the switch off at run time removed that, turning it on brought it back; with ScreenSpaceReflection off it is not there either.
- DLSS input with a fixed jitter and a still camera (Performance): where reflections dominate, the change from one frame to the next is about twice as large with the switch on (0.030-0.033 against 0.014-0.019 of the pixel value). In DLAA the measured area held fewer reflections (0.042 against 0.037-0.041).

So while DLSS runs, the plugin holds the switch at 0 from the TAA update (like the anti-aliasing hold) and gives the game's value back when it stops, with Bypass in DLAA and when NGX is not available. The developer key `ReflectionFix=0` turns the hold off for comparisons. The user confirmed the fix in the hub and saw no performance cost. The offset of the switch is read from the technique choice in `+0x2286260` (pattern `kSslrResolveSelectPattern`). `tools/live/sslrpeek.py` reads and sets it in the running game.

## Jitter, history reset and texture sharpness

Static analysis in Binary Ninja of the 15.23 exe (`re\MonsterHunterWorld.exe.unpacked.exe`), checked in the running game where noted. Prompted by the RE notes of another DLSS mod for this game (Gargauth/mhworld-dlss, GPL-3.0: only its findings were used, re-checked here, no code).

### Camera cuts

The game asks DLSS for a reset only in its DLSS 1 call at the TAA position: `Reset` = the TAA filter's history-invalid byte (`+0x185`) or a blend rate of 0. The byte is set when the TAA history is rebuilt (`+0x2391f30`: render size changes, loading) and when the render scale changed this frame. No write of it was found for camera cuts (searched: every write of `+0x185`; whether the game has another path is unknown). So after a cut in a cutscene DLSS would keep blending the previous shot into the new one for a few frames.

The plugin compares the camera with the last frame that built the TAA pass itself; frames without a TAA pass in between don't hide a cut (DLSS had no new input then, so a jump across them needs a reset too). `sCamera` (`0x1451c4400`) holds 8 viewports at `+0x50`, `0x1a0` bytes each (`+0x18` own render target, `+0x20` active, `+0x25` pass, `+0xa0` view matrix, `+0xe0` projection, `+0x120`/`+0x160` last frame's copies). Its draw (`+0x228e770`) runs the camera setup for each active viewport and then builds that viewport's scene pass, so the TAA pass build reads the matrices of its own frame. The plugin reads the main viewport (active, pass 0, no own target) once per renderer frame when the TAA pass is built and records a cut with the jitter: position moved more than 500 units (cm), view turned more than 45 degrees, or the projection scale changed more than 5%. The cut is ORed into the game's `Reset` at the TAA position and carried to the DLISP call. Observed in the hub (Performance, 240 fps): the title-to-hub switch was a cut (5359 units, 101 degrees); walking and normal camera moves were none; very fast mouse sweeps from the harness (70-145 degrees per frame) were cuts, which leave almost nothing of the previous frame on screen anyway. A frame without a readable main viewport keeps the last shot (0.9.9), so a cut across it still counts. With `LogLevel=Debug` the log names the viewport it reads (`camera: main viewport N`) and says so when there is none (`camera: no main viewport`), in which case cuts are not detected; a cutscene that draws through a viewport with its own target would show up there. Developer key `CutReset=0` turns it off.

Since 0.9.9 `LogLevel=Debug` logs every reset with its source (the first 500): `DLSS reset at frame N: game G (history invalid H, blend B), camera cut C (moved M, turned T degrees, zoom Z%), plugin P`. MHW-FSR, an FSR port of this plugin, reported split-second blur in combat and changed its cut test to position only (> 15 m). Observed in a 66-minute session (2026-10-25, Performance, 240 fps cap: hub, an expedition with two monsters, quest cutscenes; the player saw no blur): 179 resets, 170 from camera cuts alone, 8 from the game's history-invalid byte (loads and area changes, 4 of them together with a cut), 1 from the plugin (Mode change). The blend rate was 0.950 at every reset, so the game's blend-0 reset never fired. In 26 minutes of the hunt there were 7 resets, all of the same shape (moved about 470 units, turned 76-89 degrees and zoomed 50% within one frame: a scripted camera snap; which action triggers it is unknown). Dense groups with a cut every 0.5-4 s and 100-170 degree turns in one frame were cutscenes. The only false positives were fast continuous zooms just over 5% per frame (7 consecutive frames once, 2 once), with the whole picture changing anyway. A 15 m position-only test would have missed 124 of the 174 cuts, among them the cutscene cuts that turn far while moving 1-4 m. Thresholds kept.

### Jitter phases

The TAA update (`+0x23931e0`) gives the camera entry `frame % 20` of a table in the scene (`+0xe6a0`, Halton(2,3) indices 20..39 minus 0.5, read in the running game; the 20 is a constant in the code at `+0x227db60`). The DLSS guide (3.7.1) asks for 8 phases per output pixel a render pixel covers: 18 in Quality, 24 in Balanced, 32 in Performance. The plugin already replaces the camera value after the update (`FullPixelJitter`), so it now writes Halton(2,3) from index 1 with max(20, ceil(8 / scale^2)) phases: DLAA and Quality keep the game's table, Balanced gets 24 and Performance 32 (checked in the running game: Performance frame 9235 got index 20, times 2 / 0.5). Developer key `JitterPhases`: -1 automatic, 0 the game's 20, N fixed (20 also selects the game's table, any other N Halton(2,3) from index 1). It only applies with `FullPixelJitter=1`, which writes the camera value.

### Texture mip bias

The game builds 15 sampler banks when it loads its shader package (`+0x23d6690`): bank 0 without bias, banks 1-7 at -log2(2, 2.5, 3, 3.5, 4, 4.5, 5) mip levels, banks 8-14 the same positive. A material takes its samplers from the bank named by the scene's `mSamplerLodBias` (scene `+0x1a8`) when it is loaded (`+0x2383566`); the game's constructor sets 0. The DLSS guide (3.5) recommends a bias of log2(render / display) - 1 (the game's own dynamic resolution uses log2(level) + log2(0.5) the same way), and the banks match it closely: DLAA bank 1 (-1), Quality bank 3 (-1.58), Balanced bank 4 (-1.81 for -1.79), Performance bank 5 (-2). So while DLSS runs the plugin holds that bank number and gives the game's value back when DLSS stops; no code is changed. Only materials loaded afterwards use it, and a Mode change at run time reaches the textures at the next area change. The game did not change the value when the hub loaded.

Since 0.9.6 the bank is held from the first frame (the sMhMain update, about 5 s after the process starts with SharpPluginLoader), as soon as the renderer's DLSS type and applied render scale agree (the type comes one frame before the scale). Up to 0.9.5 it was set from the TAA update, which first runs at the title, about 22 s in. Materials loaded at start-up stay resident into the hub: counting the pointers to each bank's sampler states in the game's memory in Astera (D3D12, cold start through `mhwharness.py to-game`), 42,778 pointed at bank 0 and 71,793 at the DLAA bank with the old timing, 18,291 and 97,501 with the new one. So about a fifth of the hub's material samplers had no bias before. The 18,000 left on bank 0 stayed the same when the bank was written from outside at 4.6 s, as soon as the scene object existed, so they are other users of bank 0.

The D3D12 renderer keeps every combination of samplers a draw binds in a table cache (`[renderer+0x78]+0x1370`, made once with the device by `+0x23a48e0`: +8 tables, +0xc next free slot, +0x10 capacity 2048), backed by a shader-visible sampler heap of 2048 descriptors. The lookup (`+0x2624870`) appends missing tables at the next slot without a bound check, and nothing frees slots while the game runs (the cache is only created in the device constructor and freed at shutdown). Each bank the materials use adds its own combinations. Astera used 363 slots with the old timing (bank 0 and the DLAA bank mixed) and 261 with the new one; stepping Anisotropic Filtering through all levels costs about 225 slots per level (measured by the parallel project mhw-dlss-update). So a bank not yet set in this session is only set while 512 slots are left; otherwise areas loaded from then on keep the current bank and the log has a warning (tested by raising the next free slot to 1600 and switching Mode). `tools/live/samplerpeek.py` prints the cache use, the bank and the renderer's DLSS state, or with `--watch` every change from the game's start.

If NGX then fails at the first evaluate (the Proton case of 0.9.5), the hold ends at the title, but the materials loaded until then keep the bias at full resolution.

Measured in the hub (2560x1440, same spawn point, restart per variant): Performance, texture detail (mean Laplacian) +33% with the bias, close to DLAA without it; frame-to-frame change with a still camera +5-10% (idle animations included). DLAA: +2% detail. Developer key `MipBias`: `Auto`, `Off` (the game's bank), or a bias in levels (the nearest bank).

### Other checks

- `IsHDR` at the DLISP position: the input there is after tone mapping (maximum about 1.4), and the flag is set because the texture is a float format. Measured in one session (Performance, hub, same spot): with `HDR=0` brightness and detail were within 1-2% of the default (background mean 44.3 against 44.5, detail 14.53 against 14.71), so the flag stays.
- Render size in Performance at widths whose half is not a multiple of 16 (3440, 1366): the aligning ratio setter `+0x229be60` (clamp to 0.5, then round the width down to a multiple of 16) is only referenced from the renderer's property table. With DLSS on, the game applies the scale through `+0x229bed0`, which clamps to [0.5, 1] without rounding, so the size mismatch that setter can cause does not apply here (not tested on such a display).
- Pass order (filter sort keys, from the other mod's notes, not re-checked): bloom runs before the TAA pass; then depth of field, motion blur, tone mapping and colour grading, FXAA, and the DLISP point. So in the upscaling modes DLSS sees the frame after depth of field, motion blur and tone mapping; bloom is already in the TAA input.
- DX12 frame pacing: before each update of the main object the main thread waits up to 1 s for the swap chain's frame latency waitable object (`+0x23ade30`, called from `+0x229b020` in DX12 only; the swap chain's `SetMaximumFrameLatency` is called at `+0x23a6553`). A single timeout sets a latch (device `+0x10da`) that switches the wait off until the game exits. The plugin's Reflex sleep comes after it. Not measured separately; the latency measurements above include it.

## Linux (Proton)

DLSS under Proton needs Proton 9 or newer, which turns NVAPI on by default (Proton 8: `PROTON_ENABLE_NVAPI=1`), and the NVIDIA driver's `nvngx.dll`, which Proton copies into the prefix's system32 when the driver package ships it. The log names the Wine version and host and checks the registered NGX core folder.

Proton's NVAPI, dxvk-nvapi, reports Turing and newer GPUs as Pascal to `MonsterHunterWorld.exe` (`needsPascalSpoofing` in its `src/util/util_env.cpp`, since 2022), because the game's own DLSS 1 crashed under Proton. NGX then refuses DLSS with "unsupported hw detected" (`0xbad00001` from the NGX init). dxvk-nvapi reads `DXVK_NVAPI_GPU_ARCH` at every `NvAPI_GPU_GetArchInfo` call and lets it take precedence over the spoof. So since 0.9.6 the plugin sets it in its own process right before its NGX init, after the game's DLSS 1 check has run: `GB200` for PCI device ids from 0x2b00 (newer generations included), `AD100` for 0x2600..0x28ff, `GA100` for 0x2200..0x25ff, `TU100` for 0x1e00..0x1f7f (GTX 16 cards have no DLSS). Set as a Steam launch option, the variable also reaches the game's DLSS 1; with Stracker's Loader alone the game then quit before its logo. Confirmed working on Proton with 0.9.6 test 1 (a player's log, 2026-10-25).

Since 0.9.7 the log then says what NVAPI reports for the game's GPU (`Wine: NVAPI reports the GPU as AD100 (0x190)`), with a warning and the `GpuArch` values when it is still older than Turing. `GpuArch` in `[Advanced]` sets the value by hand or turns the plugin's setting off, for cards outside the mapping or a dxvk-nvapi that changes its spoofing. If dxvk-nvapi ever reads the variable only once, setting it in the process would come too late; the log would show Pascal, and the remaining way would be the launch option (inferred, not tested).

## Graphics debugging

The game exe restarts itself through Steam when it is started directly (Steam DRM), so launchers of graphics debuggers lose it. `tools/capture/` loads RenderDoc from inside the game instead:

- `MHW-Capture.dll` (`cmake -S tools/capture -B build-capture -G Ninja`, needs RenderDoc installed for `renderdoc_app.h`) goes into `nativePC\plugins`. With SharpPluginLoader's early load the plugin folder is loaded before the renderer, so RenderDoc hooks the device. `echo 1 > nativePC\plugins\MHW-Capture.trigger` captures the next frame into `nativePC\plugins\MHW-Capture\`. Under RenderDoc the game's NGX setup fails (the plugin logs "DLSS was refused by the NGX 1.x core"), so the game runs without DLSS and writes `NVIDIA DLSS=Off` into graphics_option.ini (put it back afterwards). The capture shows the game's own pipeline at native resolution, which is what the passes above were read from.
- `rdq.py --start CAPTURE.rdc` keeps a capture open in qrenderdoc's embedded Python (`rdserver.py`; RenderDoc's first-run analytics prompt has to be answered or switched off in `%APPDATA%\qrenderdoc\UI.config` first) and `rdq.py SCRIPT.py` runs scripts against it: actions, pipeline state with the shaders' resource names, disassembly, constant buffers, texture data (`texio.py` reads the dumps into numpy).
- Nsight Graphics 2026.3 is installed too. Its `ngfx-capture` can capture NGX and DLSS, but its captures need the GUI to inspect; the same Steam DRM restart applies to launching through it.

## Conventions

Measured with the scripts in `tools/`:

- The game's motion vectors are the clip-space difference to the previous position times the render size, so DLSS gets them scaled by `(0.5, -0.5)` (`MotionScaleX/Y`).
- Depth is reversed (sky = 0).
- The camera jitter `h` moves the image by `(h.x / 2, -h.y / 2) * render scale` render pixels.

`tools/motion_check.py` checks the motion vector scale and the jitter term against frames dumped while moving (`DebugDumpFrames=8` in the ini; the dump starts 5 seconds after saving, enough to get back into the game and move the camera).

Versions 0.1 and 0.2 passed the motion vectors unscaled (`MVScaleX/Y=1`). DLSS then put its history in the wrong place whenever the camera moved and dropped it, which showed as softer, slightly aliased edges in motion. Those keys are ignored now.

## Developer ini keys

The default ini only holds the player settings. These keys are read from an `[Advanced]` section (any section works) and default to the values below. Inis written by 0.8.1 and older have the first group in them.

| Key | Default | Meaning |
| --- | --- | --- |
| `DepthInverted` | 1 | Depth is reversed (sky = 0). |
| `MVJittered` | 0 | The motion vectors contain the camera jitter. They don't in this game. |
| `MotionScaleX/Y` | 0.5 / -0.5 | Scale from the game's velocity to DLSS motion vectors. |
| `JitterScaleX/Y` | 0.5 / -0.5 | Scale from the camera jitter to the DLSS jitter offset (times render scale). |
| `ForceJitter` | 1 | Keep the camera jitter on while DLSS runs (some scenes switch it off). |
| `FullPixelJitter` | 1 | Widen the game's half-pixel jitter to a full render pixel while DLSS runs. |
| `ReflectionFix` | 1 | Hold the game's 2x2 reflection history swap off while DLSS runs (see [Reflections](#reflections)); 0 = the game's reflections, for comparisons. |
| `CutReset` | 1 | Reset the DLSS history on camera cuts (see [Camera cuts](#camera-cuts)). |
| `JitterPhases` | -1 | Jitter phases while DLSS runs: -1 = max(20, 8 / scale^2), 0 or 20 = the game's table, N = N (see [Jitter phases](#jitter-phases)). Needs `FullPixelJitter=1`. |
| `MipBias` | Auto | Texture mip bias while DLSS runs: `Auto` = log2(render scale) - 1, `Off` = the game's, or levels (the nearest of the game's banks; see [Texture mip bias](#texture-mip-bias)). Reaches areas loaded afterwards; held from the first frame. |
| `HDR` | -1 | DLSS IsHDR flag: -1 from the game, 0 or 1 forced. |
| `ExposureScale`, `PreExposure` | 1 | Passed to DLSS evaluate as is. |
| `NGXLogging` | 0 | NGX's own log lines in `MHW-DLSS4.log`. |
| `VerboseFrames` | 0 | Log the first 40 TAA passes and evaluations after a reload instead of 4. |
| `DebugProbe` | 0 | D3D12: log statistics of the DLSS inputs after a reload. |
| `DebugDumpFrames`, `DebugDumpDelay` | 0, 5 | D3D12: dump N frames of colour, motion, depth and jitter to `MHW-DLSS4\`, starting the given seconds after the reload. |
| `DebugDumpOutput` | 0 | D3D11: write one DLSS output frame to `MHW-DLSS4\output.bin`. |
| `DebugFixedJitter`, `DebugJitterX/Y` | 0 | Replace the camera jitter with a constant (camera units). |
| `GpuArch` | Auto | Under Wine / Proton only: the `DXVK_NVAPI_GPU_ARCH` value set for the plugin's NGX init. `Auto` = from the card's PCI device id (a value from the launch options is kept), `Off` = leave the variable alone, or one of dxvk-nvapi's names (`TU100`, `GA100`, `AD100`, `GB200`; it ignores names it doesn't know), which also replaces a launch option value. Read at the first DLSS evaluate, so changes need a restart. See [Linux (Proton)](#linux-proton). |
| `DebugNgxFail` | 0 | Act as if NGX could not be initialised (the Proton report of 0.9.4): DLSS turns itself off and the game renders at full size. Read at the first DLSS evaluate. |
| `DebugNoNvidia` | 0 | Act as if the PC had no NVIDIA graphics card: the game's NGX init fails (the game then shows its own "DLSS not available" message once and writes `NVIDIA DLSS=Off` into graphics_option.ini) and the plugin switches itself off. |
| `LatencyStats` | 0 | Log NVIDIA's latency report every 5 seconds (Reflex markers; works with `LowLatency=Off` too). |
| `FrameLimit` | 0 | Frame rate cap through Reflex (`minimumIntervalUs`), fps; 0 = none. Measured to add the same input lag as the game's Frame Rate option and the driver's Max Frame Rate with `LowLatency` on, so it is a measurement tool, not a player setting (it was one in the 0.9.0 working copy). |
| `ReflexSleep` | 1 | 0 = set the markers and the sleep mode but never call `NvAPI_D3D_Sleep`, to compare where the driver waits. |

## Building

Visual Studio 2022 or newer (MSVC), CMake 3.24+, Ninja, and `fxc` from the Windows SDK.

- The NVIDIA DLSS SDK is not in the repository; see [third_party/dlss_sdk/README.md](../third_party/dlss_sdk/README.md) for which files to copy from https://github.com/NVIDIA/DLSS.
- `third_party/nis` holds `NIS_Config.h`, `NIS_Scaler.h` and `NIS_Main.hlsl` from the NVIDIA Image Scaling SDK v1.0.3 (https://github.com/NVIDIAGameWorks/NVIDIAImageScaling, MIT licence, see `third_party/nis/licence.txt`).
- `src/shaders/rcas.hlsl` is adapted from AMD FidelityFX Super Resolution 1.0 (MIT licence, `third_party/amd_fsr/licence.txt`).
- `third_party/nvapi/nvapi_reflex.h` holds the Reflex part of NVIDIA's NVAPI headers (https://github.com/NVIDIA/nvapi, MIT licence, `third_party/nvapi/licence.txt`).

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
tools\deploy.ps1          # copy into the game for testing
tools\package.ps1         # dist\MHW-DLSS4-<version>.zip
```

`tools/` also holds the scripts used to verify the conventions against the running game (window capture without stealing focus, ini-driven A/B runs, jitter and motion vector analysis); `tools/re` and `tools/live` hold reverse-engineering helpers and live-game test scripts.
