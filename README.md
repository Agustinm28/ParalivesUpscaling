# Paralives Upscaling (FSR / XeSS / DLSS)

Native AMD FSR 3.1 / FSR 4, Intel XeSS and NVIDIA DLSS upscaling and anti-aliasing for **Paralives**, as a BepInEx mod.

No OptiScaler, no DLL proxies: the mod drives the official AMD FidelityFX, Intel XeSS and NVIDIA DLSS SDKs directly on the
game's own DirectX 12 device, and integrates with the game's render pipeline (Unity 6, URP) so the upscaler gets proper
depth and motion vectors.

## Features

- **FSR 4** (machine-learning upscaler) and **FSR 3.1**, picked automatically or forced from the options menu.
- **Intel XeSS** (XMX path on Intel Arc, DP4a path on most other DirectX 12 GPUs), with automatic fallback to FSR.
- **NVIDIA DLSS** on NVIDIA RTX cards (greyed out in the menu on other GPUs), with automatic fallback to FSR.
- **Native AA** mode: FSR as anti-aliasing at full resolution, a sharper and more stable replacement for MSAA/SMAA.
- Upscaling modes from Quality (1.5x) to Ultra Performance (3.0x).
- Settings live in the game's own **Options > Video** menu and apply instantly, without restarting.
- The UI is always drawn at full resolution.
- The game's post-processing (bloom, vignette) runs *after* FSR, at full resolution, as AMD recommends.
- Fixes for this game's quirks: characters are missing from Unity's depth/motion data, so the mod rebuilds their
  motion vectors from the real depth buffer (no ghosting on characters when the camera moves); grass, ambient
  occlusion and save-game thumbnails keep working.

## Requirements

| | |
|---|---|
| Game | Paralives (Steam), Unity 6 build |
| Mod loader | [BepInEx 5](https://github.com/BepInEx/BepInEx/releases) x64 (tested with 5.4.23.5) |
| Graphics API | **DirectX 12**: add `-force-d3d12` to the game's Steam launch options |
| FSR 3.1 | Any DirectX 12 GPU (AMD, NVIDIA, Intel) |
| FSR 4 | AMD Radeon RX 9000 series; RX 7000 series with a recent driver |
| Intel XeSS | Best on Intel Arc; works on most DirectX 12 GPUs with Shader Model 6.4 (DP4a) |
| NVIDIA DLSS | NVIDIA GeForce RTX (20 series or newer) with a recent driver |

The mod only works on DirectX 12, not on DirectX 11 (the game's default). On tested hardware the game runs as fast or
faster on DirectX 12.

## Installation

The release zip contains:

| | |
|---|---|
| `Install.bat`, `setup.ps1` | The installer |
| `files\` | The mod, laid out like the game folder |
| `licenses\` | AMD FidelityFX, Intel XeSS and NVIDIA DLSS SDK licenses |
| `README.md` | This file |

### Easy install (recommended)

1. **Extract the zip** (right-click > Extract All). Do not run `Install.bat` from inside the zip: Windows would only copy
   the installer, without the mod files.
2. Close Paralives if it is running, then run **`Install.bat`** from the extracted folder.
3. The installer looks for the Paralives folder (next to the installer, then in your Steam libraries) and **asks you to
   confirm it**:

   ```
   Paralives folder: D:\SteamLibrary\steamapps\common\Paralives
   Is this the right folder? [Y]es / [C]hange / [Q]uit
   ```

   **Change** opens a folder picker (the folder must contain `Paralives.exe`); **Quit** exits without changing anything.
   Nothing is installed or removed until you confirm the folder.
4. If **BepInEx** is not installed, the installer downloads **BepInEx 5.4.23.5 x64** from BepInEx's official GitHub
   release and checks it against the official SHA-256 before extracting it; if the file does not match, nothing is
   installed. Then it copies the mod.
5. In Steam: right-click Paralives > Properties > General > Launch Options, and add `-force-d3d12`. The installer copies
   it to the clipboard, so you only need to paste it.
6. Start the game and pick a technology and a quality in **Options > Video**.

**Updating or removing:** run `Install.bat` again. If the mod is already installed it offers **[U]pdate / reinstall**,
**[R]emove** or **[C]ancel**. Remove deletes only the mod's files (and, if you say so, its settings); BepInEx stays,
since other mods may use it. If you no longer need it, you can also remove `-force-d3d12` from the launch options.

### Manual installation

1. Install BepInEx 5 x64 into the Paralives folder (the one with `Paralives.exe`) and start the game once.
2. Copy the contents of the zip's `files` folder into the Paralives folder, which puts these files in place:

   | File | Destination |
   |---|---|
   | `ParalivesUpscaling.dll` (managed) | `BepInEx\plugins\ParalivesUpscaling\` |
   | `ParalivesUpscaling.dll` (native) | `Paralives_Data\Plugins\x86_64\` |
   | `amd_fidelityfx_loader_dx12.dll` | `Paralives_Data\Plugins\x86_64\` |
   | `amd_fidelityfx_upscaler_dx12.dll` | `Paralives_Data\Plugins\x86_64\` |
   | `libxess.dll` | `Paralives_Data\Plugins\x86_64\` |
   | `nvngx_dlss.dll` | `Paralives_Data\Plugins\x86_64\` |

3. In Steam: Paralives > Properties > General > Launch Options: `-force-d3d12`.
4. Start the game and pick a technology and a quality in **Options > Video**.

To uninstall manually, delete those six files, the `BepInEx\plugins\ParalivesUpscaling` folder and, to reset the
settings, `BepInEx\config\paralives.upscaling.cfg`.

## Usage

In **Options > Video**:

| Option | Values |
|---|---|
| **Upscaling Technology** | Off, FSR 3, FSR 4, Intel XeSS, NVIDIA DLSS. The entries show the exact versions your driver offers (e.g. FSR 3.1.5, FSR 4.1.1, XeSS 2.0.2, DLSS 310.9.1). DLSS is greyed out (and cannot be picked) without an NVIDIA RTX card. FSR 4 falls back to FSR 3, XeSS and DLSS to FSR, if they cannot start. |
| **Upscaling Quality** | Native AA, Quality, Balanced, Performance, Ultra Performance |
| **FSR Sharpness** | 0 (off) to 1 (FSR only) |
| **Show upscaler info on screen** | Shows at the top of the screen what is actually running, e.g. `XeSS 2.0.2 · Quality · 1707x960 → 2560x1440` (fallbacks included). |

The same modes and render resolutions apply to XeSS and DLSS (DLAA for Native AA). Switching to XeSS the first time takes a moment while it compiles its shaders.

The options follow the game's language (English, Spanish, Brazilian Portuguese, French, German, Italian, Polish,
Simplified Chinese). Without DirectX 12 they are shown greyed out, with a note explaining how to enable it.

| Mode | Scale | Render resolution at 2560x1440 |
|---|---|---|
| NativeAA | 1.0x | 2560x1440 |
| Quality | 1.5x | 1707x960 |
| Balanced | 1.7x | 1506x847 |
| Performance | 2.0x | 1280x720 |
| UltraPerformance | 3.0x | 853x480 |

Changes apply immediately. The selection is saved in `BepInEx\config\paralives.upscaling.cfg`.

## Will it raise my FPS?

**Only if your GPU is what limits your frame rate.** Upscaling makes the GPU render fewer pixels; it does nothing for
the CPU, and Paralives is a simulation game with a lot of CPU work per frame (characters, AI, objects, the game's own
rendering setup).

Measured with a Radeon RX 9070 XT at 2560x1440, game paused, same scene:

| Mode | Frame time | FPS | GPU usage |
|---|---|---|---|
| Off | 6.9 ms | ~145 | 77 % |
| NativeAA (FSR 4) | 7.25 ms | ~137 | 82 % |
| Quality (FSR 4) | 7.0 ms | ~143 | 74 % |
| Performance (FSR 4) | 6.9 ms | ~145 | 68 % |

The frame time stays at ~6.9 ms whether the game renders 2560x1440 or 1280x720, and the GPU never reaches 100 %: that
machine is **CPU-bound** at about 145 FPS, so rendering fewer pixels cannot make it faster. In a heavier, unpaused scene
on the same machine (more GPU load), Performance went from ~85 to ~115 FPS.

In practice:

- **GPU-bound** (4K, older or mid-range GPUs, busy lots): the upscaling modes give real FPS gains.
- **CPU-bound** (fast GPU at 1080p/1440p): FPS stays about the same. Use **NativeAA** for the best image; it costs a
  few percent at most and looks better than the game's MSAA/SMAA.
- A quick check: if your GPU usage is well below ~95 % with the upscaler off, you are CPU-bound.

The mod itself adds about 0.3 ms of CPU time per frame (it renders the world with a second camera).

## Known limitations

- **DLSS has not been tested by me**: the mod was developed on an AMD card, so DLSS has not been tried on NVIDIA
  hardware yet. It uses the official DLSS SDK and the same inputs as FSR and XeSS; if something looks wrong with it,
  please report it with both logs.

- **Walking characters with FSR 3 / XeSS**: the game's character shaders produce no per-object motion vectors, so FSR 3
  and XeSS can show slight flicker on characters while they walk. For FSR 3 the mod softens it with a reactive mask;
  FSR 4 handles it well.
- **Optimisation mod (6ix modpack)**: keep its `GameResolution = 1`; the mod pauses itself while another mod renders the
  game camera to a texture.
- **ParalivesDLSS**: needs DirectX 11, this mod needs DirectX 12, so they never run together.
- **AMD Adrenalin "FSR 4 upgrade"**: the driver can silently replace FSR 3.1 with FSR 4. Turn it off if you want to
  compare the two versions.

## Advanced settings

`BepInEx\config\paralives.upscaling.cfg`, section `[Advanced]` (restart the game after editing):

| Setting | Default | Purpose |
|---|---|---|
| `FullResolutionAO` | `true` | While upscaling, compute the game's ambient occlusion at full render resolution (sharper contact shadows, a bit more GPU). |
| `CharacterReactive` | `0.6` | FSR 3 only: how much FSR trusts the current frame on characters. Higher = less flicker on walking characters, less smoothing. |
| `JitterSignX/Y`, `MotionVectorSignX/Y` | `-1` | Calibration. Change only if the image shimmers while still or smears while the camera moves. |
| `JitterCancellation` | `true` | Unity's motion vectors include the camera jitter; FSR removes it. |
| `PresentFlipY` | `false` | Only if the 3D world shows upside down. |
| `DebugView` | `false` | Shows FSR's debug view (motion vectors, masks) instead of the image, and logs depth/motion probes. |
| `PerfLog` | `false` | Every 2 s, logs FPS, frame time, URP render time, render resolution and every camera rendered. |

## Troubleshooting

Logs:

- `BepInEx\LogOutput.log`: managed side ("Upscaler running: 4.1.1, 1707x960 -> 2560x1440." when it works).
- `Paralives_Data\Plugins\x86_64\ParalivesUpscaling.log`: native side (available FSR versions, context creation, errors).

| Symptom | Cause / fix |
|---|---|
| FSR options greyed out | The game is on DirectX 11: add `-force-d3d12`. |
| Upscaling switched itself off after a crash | Crash guard: the game closed while the upscaler was starting, so the next start turned it off. Turn it back on in the menu; if it happens again, send both logs. |
| "ParalivesUpscaling.dll could not be loaded" | The native DLL is missing from `Paralives_Data\Plugins\x86_64`. |
| FSR4 picked but log says 3.1.x | Your GPU/driver does not offer FSR 4; update the driver. |
| DLSS greyed out | No NVIDIA RTX card (or the driver is too old). `ParalivesUpscaling.log` says why ("DLSS: not available ..."). |
| XeSS picked but log says FSR | XeSS could not start ("XeSS unavailable; falling back to FSR" in `ParalivesUpscaling.log`, with the reason). |

## How it works

1. A hidden camera copies the game camera every frame and renders the world, without post-processing, into a
   low-resolution HDR texture through a sub-pixel jittered projection. URP renders depth and motion vectors for it.
2. A render-graph pass copies URP's depth texture, the real depth buffer and URP's motion vectors.
3. On the render thread, the native plugin records a small compute pass that rebuilds the motion vectors where URP's
   depth prepass missed geometry (the characters) and writes a reactive mask there, then runs FSR (FidelityFX API) or
   XeSS or DLSS into a full-resolution texture. The command list goes to Unity with
   `IUnityGraphicsD3D12v6::ExecuteCommandList` plus the resource states FSR expects, so Unity handles the barriers.
4. The game camera draws no world; a pass copies the FSR output into it, the game's post-processing runs on top at
   full resolution, and the UI camera draws the UI.

Harmony patches keep grass (drawn for one specific camera), ambient occlusion (HBAO only runs on cameras with
post-processing) and save thumbnails working.

## Building from source

Requirements: Visual Studio 2022 or newer with the C++ desktop workload and a Windows 10/11 SDK, the .NET SDK, and the
game installed (the C# project references the game's Unity and Paralives assemblies; BepInEx's reference assemblies are
in `managed/lib`).

```powershell
.\install.ps1                                   # builds both parts and copies them into the game
.\install.ps1 -Game "E:\Games\Paralives"        # other install location
.\package.ps1                                   # builds the release zip: dist\paralives_upscaling-<version>.zip
```

The release zip contains `Install.bat` + `setup.ps1` (from `installer/`), the mod under `files\` laid out like the game
folder, this README and the SDK licenses under `licenses\`.

`native\build.bat` and `dotnet build managed\ParalivesUpscaling.csproj -c Release` build each part on its own. If the game is
not in `D:\SteamLibrary\steamapps\common\Paralives`, also change `GamePath` in `managed\ParalivesUpscaling.csproj`.

| Folder | Contents |
|---|---|
| `managed/` | BepInEx plugin (C#): cameras, render-graph passes, menu, Harmony patches |
| `native/` | Native D3D12 plugin (C++): FidelityFX calls, motion-vector repair pass |
| `native/ffx/` | AMD FidelityFX SDK v2.3.0 headers and signed runtime DLLs |
| `native/xess/` | Intel XeSS SDK 3.0.2 headers and signed runtime DLL (its super-resolution library reports version 2.0.2) |
| `native/dlss/` | NVIDIA DLSS SDK 310.9.1 headers, static NGX library and signed runtime DLL |
| `native/Unity/` | Unity native plugin API headers |
| `installer/` | End-user installer: `Install.bat` + `setup.ps1` (installs BepInEx if needed, installs / updates / removes the mod) |
| `PLProbe/` | Test harness used to verify Unity's D3D12 plugin interface in this game |

## Credits and licenses

- AMD FidelityFX SDK (FSR 3.1 / FSR 4): © Advanced Micro Devices, MIT license. The `amd_fidelityfx_*.dll` files are
  AMD's signed binaries from SDK v2.3.0.
- Intel XeSS SDK: © Intel Corporation, Intel Simplified Software License (`native/xess/LICENSE.txt`). `libxess.dll` is
  Intel's signed binary from SDK 3.0.2.
- NVIDIA DLSS SDK: © NVIDIA Corporation, NVIDIA DLSS SDK license (`native/dlss/LICENSE.txt`). `nvngx_dlss.dll` is
  NVIDIA's signed binary from SDK 310.9.1.
- Unity native plugin API headers: © Unity Technologies, Unity Companion License.
- BepInEx 5.4.23.5 (`managed/lib`, reference only; the installer downloads the official release): LGPL-2.1.
- Camera scheme inspired by the ParalivesDLSS mod (Nexus Mods #400).
- AMD, Radeon and FidelityFX are trademarks of Advanced Micro Devices; Intel, Arc and XeSS are trademarks of Intel
  Corporation; NVIDIA, GeForce, RTX and DLSS are trademarks of NVIDIA Corporation. This is an unofficial fan mod, not
  made or endorsed by AMD, Intel, NVIDIA or Paralives Studio.
