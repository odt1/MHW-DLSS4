# Changelog

Changes in each version of MHW-DLSS4, for players. Developer detail is in the git history and `docs/technical.md`.

Unless a version says otherwise, no ini changes are needed and old ini files keep working. No version changes game files or game code.

## 0.9.9 (2026-10-25)

There is no 0.9.8. Coming from 0.9.5 or older: the changes of 0.9.7 below are part of this version too.

- Checked: DLSS doesn't start over during fights. A port of this plugin to FSR reported split-second blur in combat and suspected this. We played a full expedition with two monsters and cutscenes and logged every restart. DLSS started fresh only at loading screens, cutscene cuts and a few one-frame camera jumps, never from hits, roars or camera shake.
- Camera cuts are now also caught when the game draws a frame without its main camera, for example at some scene changes.
- With `LogLevel=Debug`, the log names each time DLSS starts fresh and why: loading, camera cut (with how far the camera moved, turned and zoomed), or a settings change. This helps with reports of blur or ghosting.
- The log shows the render preset the same way everywhere (`preset K`).


0.9.6 was only released as a test build for Linux (0.9.6 test 1, 2026-10-24); its changes are part of 0.9.7.

- Sharper textures from the start. Some textures are loaded while the game starts up and stay in use for the whole session, for example about a fifth of the hub's surfaces. Up to 0.9.5 these missed the sharper texture detail of Quality, Balanced and Performance. The detail is now set from the first frame, so they get it too.
- Safer Mode changes while playing in DX12. Each new Mode used in a session takes up room in a table the game fills once and never clears. The plugin now stops changing texture detail well before that table is full. It writes a warning in the log, and the textures keep their current detail until you restart.
- DLSS on Linux (Steam Proton) with an NVIDIA RTX card, confirmed by a player with the test build. For Monster Hunter: World, Proton's NVIDIA layer (dxvk-nvapi) reports RTX cards as older GTX 10 cards, to keep the game's own old DLSS 1 from crashing, and NVIDIA's DLSS runtime then refused to start ("unsupported hw detected" in the log). The plugin now reports the card's real generation only for its own DLSS 4 start, after the game's DLSS 1 check has already run.
- No launch option is needed. If you added `DXVK_NVAPI_GPU_ARCH` to the launch options, remove it: set there, it also reaches the game's DLSS 1, and the game can quit right after starting.
- Under Proton the log now names the Linux system, checks that NVIDIA's NGX files are in the game's prefix, and says which card generation NVIDIA's DLSS runtime sees. Should that ever be wrong, for example with a future Proton version or graphics card, the log says so, and the new `GpuArch` setting in an `[Advanced]` section of the ini sets it by hand: `TU100` (RTX 20), `GA100` (RTX 30), `AD100` (RTX 40) or `GB200` (RTX 50 and newer). `GpuArch=Off` leaves it alone. It only does anything under Linux.
- Corrected Proton hints in the log: Proton 9 and newer turn NVAPI on by default; `PROTON_ENABLE_NVAPI=1` is only needed with Proton 8 and older.

## 0.9.5 (2026-10-20)

- No more black screen when DLSS can't start. If NVIDIA's DLSS runtime refuses to start (seen on Linux with Proton), the 3D view used to stay black while menus still worked. Now the game simply runs without DLSS at full resolution, and `MHW-DLSS4.log` says why DLSS could not start.
- Without an NVIDIA graphics card the plugin now switches itself off completely and names the graphics card it found in the log. DLSS and Reflex both need an NVIDIA card, so on Intel Arc or AMD cards the plugin can't do anything. If you set `NVIDIA DLSS=On` in `graphics_option.ini` by hand on such a card, the game itself shows a "DLSS not available" message at start and turns the option off again.
- On Linux the log says when the game runs under Wine or Proton, with a hint when DLSS or the NVIDIA card is not found.
- Works more safely next to other plugins that change the same parts of the game.

## 0.9.4 (2026-10-15)

- Cleaner log file. `MHW-DLSS4.log` now only holds what you need: which DLSS version runs, your settings, and any warnings or errors. If you report a problem and are asked for a detailed log, set `LogLevel=Debug` in `MHW-DLSS4.ini`.

## 0.9.3 (2026-10-15)

- Camera cuts caught more reliably. 0.9.2 started DLSS fresh when the camera jumps, but it could miss a jump when the game skipped a frame right next to it, which happens during scene changes. Those jumps now count too, so there is less leftover ghosting after cuts.
- The log now says whether camera cut detection is working (`camera: main viewport ...`), which makes it easy to check in cutscenes.

## 0.9.2 (2026-10-14)

0.9.1 was not released; its changes are part of 0.9.2.

- NVIDIA Reflex added. The new LowLatency setting (Off, On, Boost; default On) lowers input lag a little. It works with DLSS on or off. Boost also keeps the graphics card at full clock speed for a bit less lag, at the cost of more power use.
- Shiny reflections fixed. Reflections on weapons, armour and metal no longer look blocky or shimmer in Balanced and Performance. They are also cleaner in Quality and DLAA. It's on automatically, with no setting to change.
- Frame rate caps explained. The README now has a section on frame rate caps and input lag, with measurements. Short version: the game's own Frame Rate option and the NVIDIA driver's Max Frame Rate add the same input lag, as long as LowLatency is on. DLSS itself adds none.
- Sharper textures in Quality, Balanced and Performance. Ground, armour and other surfaces now get detail sized for your screen resolution, not the lower internal one. Performance mode gains the most and now looks close to DLAA. If you change Mode while playing, textures catch up at the next area change or after a restart.
- No more ghosting after camera cuts. When the camera jumps, for example between shots in a cutscene, the plugin now tells DLSS to start fresh. Previously a faint image of the previous shot could linger for a few frames.
- Smoother edges in Balanced and Performance. DLSS now gets more varied samples to build the image from (24 in Balanced, 32 in Performance, up from the game's 20). This gives it more to work with on fine edges.

Texture sharpness, camera cuts and sample variety were inspired by the documentation of Gargauth's mhworld-dlss, not copied from its code.

## 0.8.2 (2026-10-10)

- The plugin DLL is now built with compiler optimisation (0.7.3 and 0.8.1 were not).
- `MHW-DLSS4.ini` shows only the player settings. `Bypass` moved to the `[DLSS]` section. The `[Advanced]` section of older ini files can be deleted; it still works if you keep it.
- The ini is checked for changes twice a second, whatever the frame rate.

## 0.8.1 (2026-10-10)

- Works with SharpPluginLoader 2.0. With it installed, the plugin is loaded earlier than with Stracker's Loader alone, and the game used to set up its old DLSS 1 instead: grey screen. The plugin now takes over the game's DLSS setup from the start.

## 0.8.0 (2026-10-09)

- Renamed from MHWDLSS to MHW-DLSS4: `MHW-DLSS4.dll`, `MHW-DLSS4.ini`, `MHW-DLSS4.log` and the `MHW-DLSS4` folder in `nativePC\plugins`. Delete the old `MHWDLSS` files when you upgrade; if both are installed, only one runs and the log says so.

## 0.7.3 (2026-10-09)

- Fixes from a code review: DX12 resources freed at the right time after Mode changes, `Bypass` only affects DLAA, the anti-aliasing setting is handed back when DLSS can't start, and safer behaviour when the game builds frames on several threads at once.

## 0.7.2 (2026-10-09)

- No more graphics driver crash when changing Mode while playing in DX12.
- The game's Anti-Aliasing option can no longer switch DLSS off or add FXAA blur on top of DLSS: while DLSS runs, the game's anti-aliasing is held at TAA, which DLSS replaces.

## Earlier versions (MHWDLSS)

- 0.7.1: fixed "exploded geometry" in DX12 that could appear after the game rebuilt its textures and stayed until a restart.
- 0.6 and 0.7: Sharpener setting with NVSharpen, Vanilla and RCAS, Sharpness from 0 to 1 per sharpener, default 0.33.
- 0.5: uses the newest DLSS that the NVIDIA driver has downloaded; `DLLPath` setting for your own `nvngx_dlss.dll`.
- 0.4: NVSharpen sharpening.
- 0.3: sharper edges in motion (motion vectors scaled correctly).
- 0.2: RCAS sharpening; Ultra Performance removed (black screen in this game).
- 0.1: first version: the game's DLSS 1 replaced by DLSS 4.
