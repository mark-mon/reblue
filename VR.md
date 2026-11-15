# re:Blue VR

A VR build of [re:Blue](https://github.com/zolaware/reblue) (Blue Dragon, static recompilation) for PC headsets through
OpenXR. The field, battles and cutscenes are drawn in stereo 3D around you. The HUD, dialogue and menus sit on a panel
in front of you, and 2D-only screens (title, videos, menus over a still picture) go on a flat screen.

This is an unofficial fork, based on re:Blue **v1.3.1**. It contains no game files. You need your own Blue Dragon discs
and a working re:Blue v1.3.1 install, exactly as for re:Blue itself.

## Install (players)

1. Install re:Blue v1.3.1 the normal way and check that the game runs and loads your saves.
2. Download the zip from this fork's Releases page and copy `rebluevr.exe` and `openxr_loader.dll` into the re:Blue
   folder, next to `reblue.exe`.
3. Start SteamVR (or another OpenXR runtime with Direct3D 12 support), then run `rebluevr.exe`.

`rebluevr.exe` uses the install's own `rexruntime.dll`, `D3D12` folder and DLLs. `reblue.exe` is not changed, and
settings and saves are shared with it. Without a headset, `rebluevr.exe` plays flat.

Keep re:Blue's auto-updater off: an update that replaces `rexruntime.dll` can stop this build from starting until a
matching VR build is released.

## Settings

In the game: Settings -> Display -> the "VR" rows (shown only while VR runs). They are saved to `vr_config.txt` next
to `rebluevr.exe`, which is written with the defaults at the first start and can be edited by hand:

| Setting | Default | Meaning |
|---|---|---|
| `mode` | `stereo` | `stereo`, `mono`, `theater` or `off` |
| `resolution` | `100` | percent of the headset's recommended eye size (applies at the next start) |
| `supersampling` | `1` | scene supersampling on top (re:Blue's own Supersampling setting is not used in VR) |
| `world_scale` | `10` | game units per metre (bigger = the world looks smaller) |
| `keep_pitch` | `0` | `1` keeps the game camera's up/down tilt |
| `cull_widen` | `1.6` | how far beyond the headset's view the game draws objects |
| `screen_width`, `screen_distance` | `3.2`, `2.5` | the flat screen, metres |
| `hud` | `1` | HUD and menus on their own panel (`0` = drawn into both eyes) |
| `hud_distance`, `hud_width`, `hud_height` | `1.4`, `1.4`, `-0.1` | the panel, metres |
| `hud_follow` | `0` | `1` = the panel follows your head, `0` = fixed in the room |

## Keys

- **F8**: stereo -> mono -> theater
- **F9**: recenter (the HUD panel and the flat screen move in front of you)
- **F11**: snapshot for bug reports. It saves the next headset images as `bdvr_dump_*.ppm` next to `rebluevr.exe`
  and logs that frame's structure to `bdvr.log`. Please attach both (zipped) to bug reports.

## How it works

- The game renders one image per frame. Every vertex shader that uses the scene camera (c24..c35) ends with a
  per-eye re-aim of its clip position: `C = P_eye * View(eye pose) * P_game^-1`. The shader change is in the
  `XenosRecomp` submodule (`vr/xenosrecomp-vr.patch`). C is chosen per draw on the CPU by matching the draw's camera
  constants against the cameras the game built that frame (`src/vr/bdvr_camera.cpp`).
- Stereo: the frame's command list is recorded once and replayed for each eye with a different eye index
  (`src/vr/bdvr_plume.cpp`).
- HUD panel: the 2D overlay lists are drawn over black and over white into their own texture, so the panel has proper
  alpha (`src/gpu/vr_hud.cpp`). Talk portraits from the scene list go to the panel too.
- Frames with no world camera (title, videos, menus over a still picture) go on a flat screen.
- The game is paced to the headset's refresh rate and the desktop vsync is off while VR runs.
- OpenXR session, swapchains, the copy to the headset and `vr_config.txt`: `src/vr/bdvr_xr.cpp`.

## Building

Build re:Blue as its README describes (ReXGlue SDK nightly-20261002, the one v1.3.1 was released with), with these
extras:

1. Apply the shader change to the submodule:
   `git -C thirdparty/XenosRecomp apply ../../vr/xenosrecomp-vr.patch`
2. Put the game's `default.xex` and its 152 shaders (`.vso` / `.pso`) in `assets/`. `vr/tools/ipk_extract.py`
   extracts the shaders from `game\pack\!necessity.ipk` of an installed copy.
3. Configure and build the D3D12 target. The output `reblue.exe` is shipped renamed to `rebluevr.exe`.

`vr/tools/bd_build.ps1` is the Windows script used for the releases (LLVM 20, CMake, Ninja, vcpkg; the paths at the
top are for the build machine).

## Credits

re:Blue by zolaware and contributors, on the ReXGlue SDK (BSD-3-Clause, see `LICENSE`). OpenXR loader and headers by
The Khronos Group (Apache-2.0 OR MIT).
