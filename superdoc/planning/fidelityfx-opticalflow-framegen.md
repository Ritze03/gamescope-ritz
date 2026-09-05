# FidelityFX Optical Flow + Frame Interpolation in gamescope — Feasibility Scout

**Status: PLANNING ONLY.** No code, no prototypes, nothing cloned into this tree, no
build changes. This document is the deliverable. It answers the user's question —
*"explore the ability of adding FSR FidelityFX SDK OpticalFlow for frame gen"* — for a
compositor that only ever sees a game's **final colour frame** (no motion vectors, no
depth). It builds on the two sibling scouts, [`lsfg-vk-integration.md`](lsfg-vk-integration.md)
and [`lsfg-in-tree-port.md`](lsfg-in-tree-port.md): gamescope's constraints (one
composite + one present per vblank, VRR conflict, overlay ghosting, licence gate) are
cited from there and **extended**, not re-derived. Where this scout's findings correct
a sibling's, it says so explicitly (§5.1).

## Verdict

**Feasible-but-costly as an engineering exercise; not sensible as a product path.**

The single biggest blocker: **FidelityFX Frame Interpolation is not an
optical-flow-only interpolator.** Its algorithm is built around game-provided **depth
and motion vectors** — the SDK documents the technique as taking "2 back buffers, and
several resources shared with `FSR3Upscaler` and `FfxOpticalFlow`", the public `ffx_api`
`Prepare` pass marks `depth` and `motionVectors` as its inputs, and five of the eleven
interpolation passes (reconstruct previous depth, game motion-vector field + its
inpainting pyramid, disocclusion mask) exist only to consume them. Optical flow is a
*secondary* candidate the shader blends in by colour similarity, not the primary
motion source. **There is no optical-flow-only mode in the SDK, no flag for it, no
shipped sample that does it, and no AMD product that does it with this code**: AFMF,
AMD's driver-level optical-flow-only frame generation, is closed, Windows-Adrenalin-only,
and the SDK says only that its Optical Flow component is "based on" it. Running FI
without depth/MVs means **modifying AMD's host code and shaders** to skip or fake those
passes — a research project whose output quality nobody has measured, with the SDK's
own fallback for "game vectors disagree with the image" being a crossfade.

Two further findings sharpen that:

- **The open-source line is frozen.** The last release with full Optical Flow / Frame
  Interpolation source and a Vulkan backend is **SDK 1.1.4** (FSR 3.1.4, 2025). SDK
  2.x ships FSR 4 as **prebuilt DLLs**, DX12-only ("Vulkan is currently not supported in
  SDK" per the README's known issues), and its 2.0.0 notes say "All SDK version 1
  effects are now deprecated". A gamescope port would vendor deprecated code AMD is not
  developing further.
- **The good news is real but secondary.** The SDK is MIT (compatible with this fork's
  BSD-2-Clause — the licence gate that blocks the LSFG port does *not* apply here), and
  its Vulkan shaders are **GLSL compiled with glslang**, the same compiler gamescope's
  `meson.build` already drives — no DXC, no HLSL, no prebuilt blobs needed. That makes
  option (a) the *best-licensed* and *best-build-fit* path; it does not make it the
  best-quality one.

**On pacing, this scout also corrects the sibling docs in gamescope's favour** (§5.1–5.2):
at a fixed refresh rate with the game limited to `refresh / 2`, 2× frame generation is
*one composite + one present per vblank, alternating real and generated* — exactly the
loop gamescope already runs. The "multiple presents per refresh interval" rearchitecture
that `lsfg-in-tree-port.md` §4 named as the hardest unsolved problem is only needed if
the target rate exceeds the display's refresh; pinning the target *to* the refresh
removes it. Added input latency is **one refresh interval (half a game frame) plus the
interpolation compute**, not "at least one game frame". This applies equally to options
(a) and (b); it lowers the pacing risk for both but does not touch (a)'s core
algorithmic blocker.

**Recommendation:** do not pursue (a). Keep (c) — external `lsfg-vk` under gamescope —
as the shipped answer, and if in-compositor frame generation is ever wanted, the
purpose-built optical-flow-only model in (b) is the higher-quality algorithm despite its
licence problem. The one thing worth doing with the FidelityFX SDK is the cheap offline
kill-test in §8, which would settle "what does FI produce with zeroed depth/MVs" with a
few days of work outside this tree — but only if someone wants the answer for its own
sake.

## 1. What the SDK actually is, today

| Fact | Finding | Source |
|---|---|---|
| Current release | **SDK 2.3.0** ("Redstone", 2026-06-24): FSR Upscaling (ML) 4.1.1, FSR Frame Generation (ML) 4.0.1, Ray Regeneration 1.2.0. FSR 3.1.5 / 2.3.4 are carried as legacy. | [README](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), [releases](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases) |
| Distribution model since 2.0.0 | "Starting with AMD FidelityFX SDK 2.0.0 the effects, previously combined in `amd_fidelityfx_dx12.dll`, are split into multiple DLLs based on effect type." PDBs are provided; the ML effects are DLL-only. "All SDK version 1 effects are now deprecated". | [version_2_0_0.md](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/whats-new/version_2_0_0.md) |
| Vulkan in 2.x | README known issues: "Vulkan is currently not supported in SDK". | [README](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) |
| Last full-source OF/FI + Vulkan | **v1.1.4** (2025-05-08, FSR 3.1.4): `sdk/src/components/{opticalflow,frameinterpolation}/`, `sdk/src/backends/vk/`, GLSL wrappers under `sdk/src/backends/vk/shaders/{opticalflow,frameinterpolation}/`. This is the only version a gamescope port could use. | [v1.1.4 tree](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4) (listed via `gh api git/trees`, 2026-09-05) |
| Licence | **MIT.** "Copyright (C) 2024 Advanced Micro Devices, Inc." … "The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software." The only obligation is retaining that notice. BSD-2-Clause + MIT in one binary is routine; **no relicensing question arises**, unlike `lsfg-in-tree-port.md`'s GPLv3 gate. | [LICENSE.txt @ v1.1.4](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/LICENSE.txt) |
| Official build platform | Windows only: "Visual Studio 2019", "Windows 10 SDK 10.0.18362.0", "CMake 3.17 - 3.30", "Vulkan SDK 1.3.239"; `GenerateSolution.bat`. No Linux mention anywhere in the docs. | [building-samples.md](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.3/docs/getting-started/building-samples.md) |
| Linux, upstream's position | Open since 2024 with no AMD response: [#28 "Can Support Linux?"](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/issues/28), [#85 "Any plans for Linux/Mac ports for FSR3?"](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/issues/85), [#105 "Linux integration: MSVC-only functions used"](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/issues/105) (the X4: Foundations team reports compiling `sdk/` on Linux after wrapping `_countof`, `wcscpy_s`, `sprintf_s`), [#194 "No Vulkan, Linux, or RHI compatibility"](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/issues/194) (2026-08). | GitHub issues, read 2026-09-05 |

**Why this matters for the question asked:** the phrase "FSR FidelityFX SDK OpticalFlow"
today points at a component that AMD has stopped developing in the open. FSR 4's frame
generation (the ML one) is DLL-only and RDNA4-only; the analytical fallback AMD ships
for older GPUs is inside those DLLs. What is portable is the 2024–2025 FSR 3.1 code.

## 2. The Optical Flow component (`ffx_opticalflow`, 1.1.2)

- **Input:** one colour buffer per frame (the "scene input"), plus a `reset` flag and a
  `backbufferTransferFunction` (`SRGB` / `PQ` / `SCRGB`) with `minMaxLuminance`. Nothing
  else — no MVs, no depth. This half of the pipeline is genuinely colour-only.
- **Output:** `opticalFlowVector`, `R16G16_SINT`, one vector per **8×8 block**
  ("(displaySize + 7) / 8" per axis), and `opticalFlowSCD`, a 3×1 `R32_UINT` scene-change
  flag. "The maximum tracking movement is 512 pixels." On a detected scene change it
  "returns zero motion estimates".
- **Algorithm:** 7-level luminance pyramid, histogram-based scene-change detection over 9
  image sections, then seven iterations of {block search against the previous frame,
  3×3 median filter, 2× upscale}. Seven compute passes in the VK backend
  (`ffx_opticalflow_{prepare_luma,compute_luminance_pyramid,generate_scd_histogram,compute_scd_divergence,compute_optical_flow_advanced_v5,filter_optical_flow_v5,scale_optical_flow_advanced_v5}_pass.glsl`).
- **Hardware:** "SM 6.2 is required. The effect uses wave operations, and also uses the
  HLSL `msad4` intrinsic extensively" — but the GLSL/Vulkan build has a **software
  `Sad()`/`QSad()` fallback** behind `FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION` (four
  byte-wise `abs()` differences), so it runs on any Vulkan 1.1+ GPU with subgroup ops,
  just slower where `msad4` isn't native. Memory at 4K: 26–28 MB on RX 7900 XTX / RX
  6600 / RTX 4080 / RTX 3060 (AMD's own table, which incidentally confirms NVIDIA was
  tested).
- **Cost:** AMD publishes **no timing** for OF alone. See §5.4 for what can be inferred.
- **Its stated role:** "based on AMD Fluid Motion Frames technology and optimized for
  game inputs … used in FSR3 combined with upscaled game motion vectors." Optical flow
  is designed as the *supplement* to game vectors (particles, shadows, reflections that
  MVs don't cover), not as the sole motion source.

Sources: [optical-flow.md @ v1.1.4](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/docs/techniques/optical-flow.md),
[gpuopen manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/),
[ffx_opticalflow.h](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.3/sdk/include/FidelityFX/host/ffx_opticalflow.h),
[ffx_opticalflow_common.h @ v1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/include/FidelityFX/gpu/opticalflow/ffx_opticalflow_common.h)
(`Sad()` lines 63–73, read via `gh api contents`, 2026-09-05).

## 3. The Frame Interpolation component (`ffx_frameinterpolation`, 1.1.3) — the crux

### 3.1 What it requires

The dispatch description (`FfxFrameInterpolationDispatchDescription`) carries, besides
the two colour buffers and the OF outputs:

- `dilatedDepth` — "The dilated depth buffer data"
- `dilatedMotionVectors` — "The dilated motion vector data"
- `reconstructedPrevDepth` — "The reconstructed depth buffer data"
- `cameraNear`, `cameraFar`, `cameraFovAngleVertical`, `viewSpaceToMetersFactor` —
  camera parameters used by the depth-based passes
- `currentBackBuffer_HUDLess` — "The current presentation color without HUD content"
  (the *only* input the docs call optional: "The `currentBackBuffer_HUDLess` is optional;
  when used it replaces the standard back buffer as interpolation source data")

In FSR 3.1 those dilated/reconstructed resources are produced by the **frame-generation
Prepare pass**, whose `ffx_api` description lists `depth` — "The depth buffer data" —
and `motionVectors` — "The motion vector data" — as its inputs. FSR 3.1's headline
feature, "the ability to decouple the frame interpolation process from that of
upscaling, so it can be used with any upscaler the user desires, or at native
resolution", decoupled FI from the *FSR upscaler*, **not from game depth and MVs** — the
game still hands them to Prepare. AMD's current FG landing page is blunter: frame
generation requires "render-resolution motion vectors and depth in supported formats"
and "camera position, up, right, and forward vectors … Failing to specify these fields
or providing incorrect/low-precision values may result in incorrect rendering."

### 3.2 What the passes do (VK backend, 11 GLSL wrappers)

In dispatch order (`ffx_frameinterpolation.cpp`): **setup** (clears atomics) →
**reconstruct previous depth** (`renderSize`) → **game motion-vector field**
(`renderSize`; packs game MVs with a priority of depth distance + colour similarity) →
**game vector-field inpainting pyramid** (SPD) → **optical-flow vector field**
(`displaySize / 8`) → **disocclusion mask** (`renderSize`; "two disocclusion masks: one
between interpolated and previous frame, one between interpolated and current", from
reprojected depth) → **interpolation** (`displaySize`) → **inpainting pyramid** →
**inpainting** ("fills holes and applies UI corrections") → optional **debug view**.

Everything from "reconstruct previous depth" through "disocclusion mask" is gated on
one condition only — `bExecutePreparationPasses = (false == Reset)` — **not** on whether
depth/MV resources were supplied. The host code registers `dilatedDepth`,
`dilatedMotionVectors` and `reconstructedPrevDepth` "only if not null", but the passes
that read them still dispatch; there is "no explicit validation" for their absence, so
passing null is not a supported "OF-only" mode, it is an unbound-descriptor hazard.

### 3.3 How the interpolation shader combines the two motion sources

From `gpu/frameinterpolation/ffx_frameinterpolation.h`:

- On `FrameIndexSinceLastReset() == 0` (first frame or `reset`): "copy the current back
  buffer and don't interpolate".
- Otherwise two candidates are produced — one warped by the **game** vector field, one
  by the **optical-flow** field — and blended by colour similarity:
  `fGameMvBias = pow(saturate(fGame_Sim / max(eps, fOF_Sim)), 1.0)`;
  `result = lerp(ofColor, fInterpolatedColor, saturate(fGameMvBias))`. For the first ~10
  frames after a reset it biases toward optical flow.
- The disocclusion factors shift the blend parameter `t` toward whichever real frame is
  not disoccluded (`t += 0.5 * (1 - fDisocclusionFactor.x); t -= 0.5 * (1 -
  fDisocclusionFactor.y)`), and when both are ~0 the pixel is marked for inpainting.
- HUD/UI handling and scene-change handling live outside this file (inpainting pass;
  OF's SCD output), and the UI path is keyed on `currentBackBuffer_HUDLess` being
  supplied.

**Consequence for a compositor:** with no depth there is no reprojected depth, hence no
disocclusion masks, hence no inpainting trigger and no `t`-shift; with no game MVs the
"game" candidate is whatever a zero vector field produces — a straight crossfade of
frames N and N+1 (a double image on anything that moved). The colour-similarity blend
would then pick the OF warp wherever OF found a good 8×8 block match and fall back to
the crossfade wherever it did not — i.e. exactly the regions (fast motion beyond 512 px,
occlusion boundaries, thin geometry, the edges of an in-game HUD) where frame
generation is hardest. That is an inference from the shader structure, not a
measurement — nobody has published one — and it is the open question the §8 kill-test
would answer. It is also why §5.6 expects AFMF-1-class artifacts at best.

### 3.4 Does anything ship optical-flow-only FI with this code? No.

- Every FSR3 integration (games, the SDK's own `FSR` sample, OptiScaler's "OptiFG"
  injection) feeds game depth + MVs through Prepare. OptiScaler's docs warn its FG mode
  "can produce crashes or HUD artifacts" even *with* those inputs.
- **AFMF** ("AMD Fluid Motion Frames") is the only AMD product that interpolates from
  colour alone. It lives in the Windows Adrenalin driver (release notes are filed under
  `RN-RAD-WIN-*`), supports RX 6000/7000 and 700M/800M iGPUs, "any OpenGL, Vulkan,
  DirectX 11, and 12 title", and is **not open source and not available on Linux** —
  which is the whole reason `lsfg-vk` exists. AFMF 1 handled hard cases by **fallback**:
  "AFMF frame generation is temporarily disabled in high-motion scenes to ensure the
  best interpolated image quality"; AFMF 2 added a "Search Mode" (Auto/Standard/High)
  that reduces fallback at high resolutions. The SDK's Optical Flow is "based on" AFMF;
  the SDK's *Frame Interpolation* is not AFMF's interpolator, and AFMF's fallback logic,
  UI detection and 2.x "AI optimisation" are not in the SDK.

Sources: [frame-interpolation.md @ v1.1.4](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/docs/techniques/frame-interpolation.md),
[gpuopen FI manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/),
[ffx_frameinterpolation.h (host)](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.3/sdk/include/FidelityFX/host/ffx_frameinterpolation.h),
[ffx_frameinterpolation.cpp](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.3/sdk/src/components/frameinterpolation/ffx_frameinterpolation.cpp),
[ffx_frameinterpolation.h (gpu) @ v1.1.4](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation.h),
[ffx_framegeneration.h (ffx_api) @ v1.1.4](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/ffx-api/include/ffx_api/ffx_framegeneration.h),
[version_1_1.md](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/docs/whats-new/version_1_1.md),
[gpuopen.com/amd-fsr-framegeneration](https://gpuopen.com/amd-fsr-framegeneration/),
[Notebookcheck on AFMF 2](https://www.notebookcheck.net/AMD-Fluid-Motion-Frames-2-technical-preview-launched-with-performance-stutter-and-latency-improvements.869413.0.html),
[AMD AFMF 2 release notes](https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-AFMF2-TECH-Preview.html)
(URL only — amd.com timed out on every fetch attempt 2026-09-05; the fallback quote is
Notebookcheck's), [OptiScaler guide](https://www.itechguides.com/optiscaler-explained-use-dlss-fsr-xess-and-frame-generation-in-more-pc-games/).

## 4. Vulkan backend, shaders, and what integrating them would take

### 4.1 Shaders are GLSL, compiled by glslang — a genuine fit

- `sdk/src/backends/vk/CMakeShadersFrameinterpolation.txt` and
  `CMakeShadersOpticalflow.txt` set the Vulkan compiler args to
  **`-compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1`**. The FFX
  shader core is a dual-dialect header set (`ffx_core_hlsl.h` / `ffx_core_glsl.h`,
  `*_callbacks_hlsl.h` / `*_callbacks_glsl.h`); the DX12 build goes through DXC, the
  Vulkan build does **not** — `FidelityFX_SC`'s `glsl_compiler.cpp` shells out to
  `glslangValidator` and reflects the SPIR-V with SPIRV-Reflect.
- The VK wrappers are small `#version 450` files (`GL_GOOGLE_include_directive`,
  `GL_EXT_samplerless_texture_functions`, `GL_EXT_shader_image_load_formatted`) that
  define bindings 0–10 and include the technique header; `ffx_core_glsl.h` additionally
  pulls `GL_EXT_shader_explicit_arithmetic_types`, `GL_EXT_shader_16bit_storage`,
  `GL_ARB_gpu_shader_int64` and the four `GL_KHR_shader_subgroup_*` extensions.
  gamescope's own shaders already use `GL_GOOGLE_include_directive`,
  `GL_EXT_shader_explicit_arithmetic_types_float16` and the same four subgroup
  extensions (`src/shaders/*.comp`, `ffx_a.h`), compiled by `src/meson.build`'s
  `glsl_generator` (`glslang -V @INPUT@ --vn @BASENAME@`).
- **So no DXC, no prebuilt SPIR-V blobs, no Windows tool is needed for the shaders
  themselves.** What `FidelityFX_SC` adds is *permutation expansion* — FI has three
  option axes (`FFX_FRAMEINTERPOLATION_OPTION_{LOW_RES_MOTION_VECTORS,JITTER_MOTION_VECTORS,INVERTED_DEPTH}={0,1}`)
  plus FP16 and wave-size variants, emitted as C arrays with reflection tables that
  `ffx_vk.cpp` looks up via `ffxGetPermutationBlobByIndex`. `FidelityFX_SC` itself is
  **Windows-only** (`ffx_sc.cpp`: `<Windows.h>`, `<pathcch.h>`, `CreateDirectoryW`,
  backslash path rewriting; `hlsl_compiler.cpp`: `LoadLibrary("dxcompiler.dll")`).
  gamescope would not use it: a compositor picks *one* permutation per shader and can
  list the wrappers in `shader_src` with `-D` defines, exactly as it builds
  `cs_easu.comp` / `cs_easu_fp16.comp` today. (For the record, DXC also ships official
  Linux binaries since v1.7.2212 — [Phoronix](https://www.phoronix.com/news/Microsoft-DX-Shader-Linux-Build),
  [releases](https://github.com/microsoft/DirectXShaderCompiler/releases) — so even the
  HLSL route would not be blocked; it is simply unnecessary.)

### 4.2 The host side is the real bulk

Sizes at v1.1.4 (blob sizes summed from the git tree, 2026-09-05):

| Piece | Files | Size | Needed for a gamescope port? |
|---|---|---|---|
| `gpu/opticalflow/` technique headers | 12 | 90 KB | yes, verbatim |
| `gpu/frameinterpolation/` technique headers | 16 | 140 KB | yes, **modified** (§3.2–3.3) |
| `gpu/ffx_core*.h` + `gpu/spd/` | 7 + 6 | ~380 KB | yes, verbatim (headers only) |
| VK GLSL wrappers (OF 7 + FI 11) | 18 | 54 KB | yes, adapted to gamescope's binding slots or kept as-is with a second descriptor layout |
| `components/opticalflow/ffx_opticalflow.cpp` + private | 3 | 72 KB | either vendor (with `#105`'s MSVC-ism fixes) or **re-implement** the resource/constant/dispatch orchestration on `CVulkanCmdBuffer` |
| `components/frameinterpolation/ffx_frameinterpolation.cpp` + private | 3 | 85 KB | same, plus the OF-only surgery |
| `backends/vk/ffx_vk.cpp` | 1 | **228 KB** | only if vendoring the host libs — it is a second, generic Vulkan RHI (resource pools, descriptor management, barrier tracking, `_WIN32` string code) that duplicates `CVulkanDevice`/`CVulkanTexture` |
| `backends/vk/FrameInterpolationSwapchain/` | 19 | 275 KB | **no** — swapchain replacement + pacing threads + UI-composition graphics pipelines; gamescope has no swapchain on DRM/Wayland and does its own pacing (§5) |
| `tools/ffx_shader_compiler/` | 12 | 151 KB | no (§4.1) |

Two integration shapes follow: **vendor** ~385 KB of host C++ (`ffx_vk.cpp` + both
components + `shared/`) behind gamescope's device, accepting a duplicate Vulkan
abstraction and the `_countof`/`wcscpy_s`/`sprintf_s` fixes from issue #105; or
**re-implement** the ~18 dispatches' resource plumbing (~25 internal textures, 2 constant
buffers each, SPD atomics) directly on `CVulkanCmdBuffer`, keeping only the GLSL. The
second is cleaner and is how DECISIONS #27 already treats bundled effects (native
compute pre-pass, shaders compiled by meson); it is also more work up front and
diverges from AMD's code permanently.

### 4.3 Device requirements — already met by gamescope

`ffx_vk.cpp` queries `VK_EXT_subgroup_size_control` (wave lane count) and
`VK_KHR_shader_float16_int8` (fp16); the shaders need Vulkan 1.1 subgroup ops, 16-bit
storage and int64. gamescope creates a Vulkan 1.3 instance (`src/rendervulkan.cpp:3718`),
requires ≥1.2 devices (`:357`), tracks fp16 as `m_bSupportsFp16` (`:532`), and runs
everything as compute (`VK_QUEUE_COMPUTE_BIT` preferred, `:368–401`). All OF/FI passes
are `VK_SHADER_STAGE_COMPUTE_BIT`; only the swapchain's UI composition (not needed) uses
graphics pipelines.

### 4.4 Non-AMD GPUs

The 3.1-era technique is vendor-neutral by design: OF/FI have the software `msad4`
fallback, AMD's memory table lists RTX 4080/3060, and AMD's launch tiers were "RX 5000
supported, RX 6000 recommended; RTX 20 supported, RTX 30 recommended", with Intel Arc
confirmed working in shipped FSR 3.1 titles. The current FG page states the analytical
fallback needs a "GPU supporting Shader Model 6.2 or above". **Intel integrated graphics
(the user's laptop) is unmeasured anywhere we could find** — it should run (ANV exposes
the needed features) but AMD's own guidance is that FG "runs best when interpolating
from a minimum of 60 fps pre-interpolation" and "Sub-30fps pre-interpolation should be
absolutely avoided", which an iGPU on a 60 Hz panel cannot satisfy at 2× (§5.2).

Sources: [CMakeShadersFrameinterpolation.txt](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/src/backends/vk/CMakeShadersFrameinterpolation.txt),
[CMakeShadersOpticalflow.txt](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/src/backends/vk/CMakeShadersOpticalflow.txt),
[CMakeCompileFrameinterpolationShaders.txt](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/include/FidelityFX/gpu/frameinterpolation/CMakeCompileFrameinterpolationShaders.txt),
[ffx_frameinterpolation_pass.glsl](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/src/backends/vk/shaders/frameinterpolation/ffx_frameinterpolation_pass.glsl),
[ffx_vk.cpp](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/src/backends/vk/ffx_vk.cpp),
[ffx_sc.cpp](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/tools/ffx_shader_compiler/src/ffx_sc.cpp),
[hlsl_compiler.cpp](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/tools/ffx_shader_compiler/src/hlsl_compiler.cpp),
[glsl_compiler.cpp](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/tools/ffx_shader_compiler/src/glsl_compiler.cpp),
[ffx-sc.md](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/docs/tools/ffx-sc.md),
[ffx_types.h](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/include/FidelityFX/host/ffx_types.h),
[OC3D on FSR 3 tiers](https://overclock3d.net/news/software/can_you_run_it_amd_details_fsr_3s_hardware_requirements/) (via search snippet; page 403s to fetch),
[Notebookcheck on FSR 3.1 + Intel Arc](https://www.notebookcheck.net/AMD-FSR-3-1-launched-frame-generation-feature-also-works-on-Nvidia-GeForce-RTX-and-Intel-Arc-GPUs.853935.0.html);
gamescope citations grep-verified 2026-09-05.

## 5. The compositor problems, answered for gamescope

### 5.1 Latency

**How gamescope paces today** (`src/steamcompmgr.cpp`, `src/vblankmanager.{hpp,cpp}`,
verified 2026-09-05): `CVBlankTimer::CalcNextWakeupTime()` wakes the steamcompmgr thread
`rollingMaxDrawTime + redZone` before the predicted vblank (red zone 1.65 ms, initial
draw time 3 ms, min-compositing draw time 2.4 ms — `vblankmanager.hpp:33–36`). The loop
(`steamcompmgr.cpp:9319`) takes the pending vblank, chooses a `FlipType` (`Normal` /
`Async` for tearing / `VRR`, `:9834–9859`), and on `Normal` paints iff
`vblank && (hasRepaint || hasRepaintNonBasePlane || bForceSyncFlip)` (`:9868–9871`) —
one `paint_all()` (`:2688`, called `:9925`), one `Present()` (`:3174`), one
`drmModeAtomicCommit` with `DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT`
(`DRMBackend.cpp:3031–3052`). `force_repaint()` (`:7829`) sets `g_bForceRepaint`;
`hasRepaint` (`:924`) is set when a base commit lands, `hasRepaintNonBasePlane` for
UI-only changes. The frame limiter is a *vblank divisor*: `steamcompmgr_should_vblank_window()`
(`:6463`) sends the game its frame-done callback only on `vblank_idx % (refresh /
target) == 0` at fixed refresh, and time-schedules it under VRR.

**Where a "hold N, present interp, then present N+1" scheme sits:** in the base-plane
path of `paint_all()`, keyed on `g_HeldCommits[HELD_COMMIT_BASE]` changing (`:2822`).
When a *new* base commit N+1 is latched at vblank V:

1. run OF(N, N+1) and FI on layer 0 (see 5.3 for where), producing G;
2. composite **G** + UI layers and present it at V;
3. at V+1, composite **N+1** + UI layers (no OF/FI work) and present;
4. UI-only repaints (`hasRepaintNonBasePlane`) re-composite whichever base image is
   current — no interpolation.

Real frame N+1 therefore reaches the screen at V+1 instead of V: **one refresh
interval later**. With the limiter pinning the game to `refresh / 2` (§5.2), one refresh
interval is **half a game frame** — 8.3 ms at 120 Hz, 4.2 ms at 240 Hz, 16.7 ms on a
60 Hz panel (where the game would be running at 30). On top of that the OF+FI dispatch
lengthens the composite that must finish before V, so `CVBlankTimer`'s rolling
draw-time pushes the wakeup earlier by that amount (~1–3 ms, §5.4), which shortens the
window in which a late game commit can still make vblank V. Total added motion-to-photon
latency: **≈ 1/refresh + t(OF+FI)**. This is the same scheme AMD's own
`FrameInterpolationSwapchain` implements (present the generated frame, then delay the
real one by "conservativeAvg = frameTime.getAverage() * 0.5 - variance" minus a safety
margin) and the same class of cost AMD quotes in shipped games (Forspoken 4K/7900 XTX:
55 ms with FSR3 Performance + FG vs 81 ms native — a *lower* absolute latency only
because the upscaler more than pays for FG, which a compositor-side scheme cannot
replicate).

**Correction to `lsfg-in-tree-port.md` §4 and `lsfg-vk-integration.md` §3:** both say
interpolation "adds at least one real frame's worth of latency". At a fixed refresh with
2× generation the delay is one *refresh* interval, i.e. **half** a real frame — the
sibling docs counted in the wrong unit. The qualitative point stands: it is a direct
trade against the tight vblank-synchronous pacing gamescope exists for, and nothing in
either option removes it.

### 5.2 Frame delivery rate

**Reframing:** generated frames need a slot *between* real ones, and gamescope's slot is
the vblank. If the FG target rate **equals the display refresh** and the game is limited
to **refresh / multiplier**, then every vblank presents exactly one image — real or
generated, alternating — and the loop keeps its shape: one composite, one `Present()`,
one atomic commit per vblank. `lsfg-in-tree-port.md` §4's "make the vblank-timer loop
fire multiple composite+present cycles inside one refresh interval" is only required if
the target exceeds the refresh (e.g. 3× a 60 fps game on a 120 Hz panel). Pinning the
target to the refresh avoids it entirely, and gamescope already has the knob: the
limiter's vblank divisor (`:6492–6497`) with `g_nSteamCompMgrTargetFPS = refresh / 2`
delivers frame-done callbacks to the game every second vblank, so the game *cannot*
outrun the cadence. **This lowers the sibling docs' single largest technical risk from
"new scheduling architecture" to "a mode that forces the limiter and alternates the base
image" — for both options (a) and (b).**

Per backend:

- **DRM/KMS at fixed refresh:** natural fit — one `drmModeAtomicCommit` per vblank
  already (`DRMBackend.cpp:4132`). Generated frames must take the full-composite path
  (`bNeedsFullComposite`, `:3661–3696`) because the generated image is an internal
  texture with no scanout Fb; `vulkan_native_effects_active()` (`:3681`) is the existing
  precedent for "a compute pass forces compositing", and the same flag would cover FG.
- **DRM with VRR** (`FlipType::VRR`, `:9846`; `cv_adaptive_sync`, `:302`): the commit
  cadence follows the game, so there is no "between" slot to fill; and the VRR path
  refuses to paint while a present is in flight (`:9909–9912`). **Disable FG under VRR**
  or force fixed refresh while FG is on — the same conclusion as `lsfg-in-tree-port.md`
  §4 and AMD's swapchain doc ("frame rate must fall within the variable refresh window;
  otherwise tearing occurs").
- **Tearing** (`FlipType::Async`, `cv_tearing_enabled`, `:462`): incompatible by
  construction — FG needs a fixed cadence; force `Normal` while FG is on.
- **Nested Wayland:** `CWaylandConnector::Present()` (`WaylandBackend.cpp:1150`) commits
  once per host frame with `wp_presentation` feedback driving `MarkVBlank`; one commit
  per host refresh, same alternation. `SupportsTearing()` is `false` (`:2495`), VRR is
  whatever the host does. Internally-rendered layers already force the full composite
  (`:1197–1212`), which FG's output would too.
- **SDL:** `VK_PRESENT_MODE_FIFO_KHR` swapchain (`rendervulkan.cpp:3321`), one
  `vkQueuePresentKHR` per vblank, `vkWaitForPresentKHR` marks vblank (`:3040–3055`). Same
  alternation works; FIFO would block a second present per interval, which is another
  reason to pin the target to the refresh.
- **What this does not solve:** a game that runs at 45 fps on a 120 Hz panel gets no
  clean multiplier. AMD's swapchain doc says "the application should ensure that the
  rendered frame rate is slightly below half the desired output frame rate"; gamescope's
  limiter can enforce exactly that. And on a **60 Hz panel** 2× FG forces the game to
  30 fps, which AMD says to "absolutely avoid" — FG only makes sense on ≥120 Hz outputs,
  which excludes most laptop panels (the user's Intel laptop, most likely) and the Steam
  Deck LCD.

### 5.3 UI exclusion

Correct and already half-solved by gamescope's layer model. `paint_all()` pushes, in
composite order (`steamcompmgr.cpp:2844–3170`, constants `steamcompmgr.hpp:34–66`): base
game plane (`g_zposBase`) → override / external overlay / **Steam overlay**
(`g_zposOverlay`) → **cursor** (`g_zposCursor`) → mura → **FPS HUD** (`g_zposFpsDisplay`)
→ **toasts** (`g_zposNotifications`) → **Shell** (`g_zposSettingsOverlay`). Every piece
of gamescope's own UI, the Steam overlay and the cursor is a *separate layer*; only
layer 0 is the game.

**The DECISIONS #27 pre-pass is the right tap point.** `vulkan_composite()`
(`rendervulkan.cpp:4306`) already runs a native compute pass on `layers[0].tex` alone,
at *source* resolution, on encoded sRGB values, and substitutes its output for layer 0
in a private copy of the `FrameInfo_t` (`SubstituteLayer0(g_output.effectsOutput)`,
`:4645`), gated by `bBaseLayerEffectsApplied` and skipped for HDR/passthru/YCbCr
(`:4483–4500`). Frame generation slots in at the same point:

- **input:** layer 0 of the *previous* base commit and of the current one (gamescope
  must retain one extra layer-0 texture — `g_HeldCommits[]` already holds commits; a
  second held reference is cheap, cf. `lsfg-in-tree-port.md` §3);
- **OF + FI run at layer-0 (render) resolution**, `displaySize == renderSize`, which
  FSR 3.1 explicitly allows ("or at native resolution") and which is cheaper than
  display resolution when FSR/NIS upscaling is on;
- **output:** a pooled texture substituted as layer 0, so the normal FSR/NIS/blit
  scaling, colour management and **UI layers composite on top of both real and generated
  frames afterwards**, per frame, untouched by interpolation.

Two things this does *not* exclude: **the game's own in-game HUD** (it is inside layer 0
— the SDK's `currentBackBuffer_HUDLess` path is unavailable to a compositor by
definition, so FI's "UI corrections" in the inpainting pass never engage) and, on DRM,
**direct scanout** (generated frames must be composited, §5.2). Note also that the
pre-pass today refuses HDR (`bSdr`), while OF/FI accept `SRGB`/`PQ`/`SCRGB` — FG could be
allowed wider than the effects if wanted.

### 5.4 Resolution and cost

AMD publishes **no per-pass timings** for OF or FI in the SDK docs (the tables are
memory only). What exists:

- AMD's launch numbers, from which the FG cost per generated frame can be *implied*
  when the GPU is the bottleneck: Forspoken, RX 7900 XTX, 4K, FSR3 Performance: 90 fps
  without FG → 164 fps with FG (real rate 82 fps) ⇒ `1/82 − 1/90 ≈ 1.1 ms` per generated
  frame including OF, FI and UI compose; Immortals of Aveum, same GPU/res: 107 → 167 fps
  (83.5 real) ⇒ `≈ 2.6 ms`. The 1440p / RX 6800 XT pairs (83 → 160, 113 → 128) are
  CPU-bound or otherwise not GPU-clean and give nothing usable.
- AMD's current FG page quotes **~2.2 ms at 4K on an RX 9070 XT** and **~2.1 ms at 1440p
  on an RX 9060 XT** — but for the *ML* frame generation (FSR 4), not this code.

**Working estimate for the analytical 3.1 path, at display resolution:** ~1–2.5 ms per
generated frame at 4K on an RX 7900 XTX class GPU; scaling by pixel count and by a
mid-range RDNA3 (RX 7700/7800 XT at roughly 55–70 % of a 7900 XTX) gives **≈ 1.5–4 ms at
4K, ≈ 0.7–2 ms at 1440p, ≈ 0.4–1.2 ms at 1080p**. Running at layer-0 render resolution
(§5.3) with FSR upscaling on scales that down further. Because the compositor shares
the GPU with the game, that time comes straight out of the game's frame budget — at a
60 → 120 target, 2 ms is 12 % of the 16.7 ms the game has. On the Intel laptop:
unknown; expect several ms at 1080p and the `msad4` software path.

Treat all of the above as order-of-magnitude. The §8 kill-test would produce real
numbers.

### 5.5 HDR / colour space

Both components take `backbufferTransferFunction ∈ {FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB,
_PQ, _SCRGB}` plus `minMaxLuminance[2]` and `FFX_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT`;
OF works on luminance internally; FI requires `currentBackBuffer`'s format to equal the
context's `backBufferFormat` and writes its output in the **same encoding as its input**.
gamescope's layer-0 colourspaces map directly: `GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB` →
`SRGB`, `_HDR10_PQ` → `PQ`, `_SCRGB` → `SCRGB`; `_LINEAR`, `_PASSTHRU` and YCbCr layers
would be skipped like the effects pre-pass skips them. Because the substituted layer 0
keeps its colourspace tag, the existing shaper/3D-LUT/CTM path
([hdr-color-management.md](../features/hdr-color-management.md)) is unaffected — the
composite still runs in gamescope's working space; only the *inputs* to OF/FI need to be
declared correctly. No blocker here.

### 5.6 Artifacts and disocclusion — what OF-only FG does on first-person titles

- **In-game HUD, crosshair, weapon model:** static or near-static pixels over a moving
  background. OF's 8×8 block vectors straddle HUD edges; without depth there is no
  disocclusion mask and without game MVs the fallback candidate is a crossfade (§3.3),
  so expect edge wobble/ghosting on HUD outlines and a doubled or smeared weapon edge
  during fast turns. This is the artifact class AFMF 1 avoided by *turning generation
  off* in "high-motion scenes"; the SDK has no such fallback logic, so gamescope would
  have to add its own (e.g. skip generation when the mean OF vector magnitude exceeds a
  threshold — cheap, but a heuristic).
- **Fast camera motion beyond 512 px/frame:** OF cannot track it; result is the
  crossfade. At 30 fps real (60 Hz panel) this happens on every flick.
- **Camera cuts / loading screens:** OF's scene-change detector zeroes the vectors;
  FI's `reset` copies the current frame — but `reset` is a *host* flag the game sets and
  a compositor cannot know. OF hands its SCD result to FI as a GPU resource; whether FI
  treats it as a reset (rather than just a zero field → crossfade for one frame) is
  something this scout did not verify (§9).
- **Menus, static screens:** zero motion → generated frame equals the real one; fine.
- **Transparency, particles, reflections:** exactly what FSR3's own design leans on OF
  for *in addition to* game MVs; alone it is weaker, but not worse than any other
  colour-only method.
- **Mouse cursor, Steam overlay, HUD, Shell, toasts:** excluded by §5.3 — the one
  artifact class the compositor position solves *better* than a Vulkan layer in the
  game's process.

**Decision, 2026-09-05:** the user chose to keep running **external LSFG** (`lsfg-vk` as a
layer in the game's process) rather than pursue any in-compositor option here. The one
piece of the in-game HUD that suffers most under it — the crosshair — is instead drawn by
gamescope itself, after interpolation, in the HUD's layer: see
[features/crosshair.md](../features/crosshair.md). That is §5.3's "compositor position
solves it better" argument applied to the one element that matters for aiming.

Net: **AFMF-1-class output at best**, probably below Lossless Scaling's purpose-built
colour-only model, on a codebase whose authors never tuned it for this input.

Sources for §5: gamescope citations grep-verified against this tree 2026-09-05 (paths
and line numbers above); [frame-interpolation-swap-chain.md @ v1.1.4](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/docs/techniques/frame-interpolation-swap-chain.md);
[FrameInterpolationSwapchainVK.cpp @ v1.1.3](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.3/sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.cpp);
[gpuopen.com/news/fsr3-in-games-technical-details](https://gpuopen.com/news/fsr3-in-games-technical-details/);
[gpuopen.com/amd-fsr-framegeneration](https://gpuopen.com/amd-fsr-framegeneration/);
[gpuopen FSR3 manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-interpolation/).

## 6. The three options compared

| | **(a) FidelityFX OF + FI, OF-only** | **(b) `lsfg-vk` in-tree port** ([`lsfg-in-tree-port.md`](lsfg-in-tree-port.md)) | **(c) Do nothing / `lsfg-vk` as a layer under gamescope** ([`lsfg-vk-integration.md`](lsfg-vk-integration.md) option (a)) |
|---|---|---|---|
| Licence | **MIT — clean.** Notice retention only. | **GPLv3 — forces relicensing** of the binary, or a separate-process/dlopen architecture. Plus the user must own `Lossless.dll` (Steam, ~$7). | None. |
| Algorithm fit for "colour only" | **Poor by design.** FI wants depth + MVs; OF-only requires modifying AMD's shaders/host code; no reference, no measurements. | **Purpose-built.** The model derives motion from two colour frames; that is its entire product. | Same algorithm as (b), in the game's process. |
| Expected quality | AFMF-1-class or below; HUD edge wobble, crossfade fallback on fast motion, no fallback heuristics unless written. Unknown until measured. | Commercially proven (Lossless Scaling); still ghosts on in-game HUDs, but tuned for it. | As (b). |
| Build / toolchain | **Good fit:** GLSL via glslang, all-compute, device features already enabled. | Good fit per sibling doc (dma-buf + timeline semaphore API; own `VkDevice`). | N/A. |
| Code to carry | ~600 KB GLSL/headers verbatim + either ~385 KB vendored host C++ (with MSVC-ism fixes) or a re-implementation of ~18 dispatches' plumbing; deprecated upstream line. | ~93 KB backend + ~116 KB common (or adapt to `CVulkanDevice`); moving `develop` API. | Config UI + env plumbing only (sibling doc §7). |
| Pacing work in gamescope | Same as (b): FG mode forcing fixed refresh, limiter = refresh/2, alternate base image, disable under VRR/tearing (§5.2 — smaller than the sibling docs feared). | Same. | None — the game's own swapchain paces. |
| Latency | +1/refresh + ~1–4 ms compute. | +1/refresh + model cost. | Same class, inside the game process; plus the documented `ENABLE_GAMESCOPE_WSI=0` pacing workaround. |
| Effort | **Largest.** Kill-test days; a presenting prototype weeks; making OF-only FI *look acceptable* is open-ended research on top. | Weeks-to-months per sibling doc, now with a smaller pacing component. | **Zero to days.** |
| Risk | **High** (quality unknown, frozen upstream, vendor-neutral but Intel iGPU unmeasured). | Medium (licence decision, API churn, pacing). | Low. |

**Best quality:** (b) — the only colour-only model in the comparison that was designed
and tuned for colour-only input — with (c) delivering the same pixels today at no cost.
**Least work:** (c). **(a) wins only on licence and toolchain**, which are the two
dimensions that do not decide whether the generated frames are watchable.

## 7. Risks (option (a) specifically)

- **Algorithmic:** OF-only FI is unsupported by the SDK; the required surgery (skip
  depth/MV passes, synthesise "no disocclusion", pick a sane fallback) is design work
  with no reference output to compare against.
- **Upstream:** the code is in AMD's deprecated v1 line; no fixes will come. The v2.x
  ML frame generation is DLL-only, DX12-only, RDNA4-only — not a future path for a
  Linux compositor either.
- **Quality on the target content:** first-person HUDs are the worst case for any
  colour-only interpolator, and the SDK's FI lacks AFMF's fallback logic.
- **Refresh-rate dependency:** 2× FG only makes sense at ≥120 Hz; on 60 Hz panels it
  forces 30 fps real frames, which AMD says to avoid. Excludes most laptops.
- **Latency:** +1/refresh + compute, unavoidable (§5.1), on a compositor whose pacing is
  designed to minimise exactly that.
- **Intel:** unmeasured; software `msad4`; likely too slow to be worth it on an iGPU.
- **Size:** the largest code import this fork would have made, for its least certain
  feature.

## 8. If someone still wants the answer: the cheap kill-test

Outside this tree, no gamescope code touched, a few days:

1. Clone SDK v1.1.4. Compile the 18 OF/FI GLSL wrappers with plain `glslangValidator -V
   --target-env vulkan1.2 -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=0
   -DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0 -DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0`
   (one permutation; the Windows `FidelityFX_SC` is not needed). This alone confirms
   §4.1's "glslang suffices" claim on Linux.
2. Write a standalone Vulkan harness (own instance; ~500–800 lines) that allocates the
   OF/FI internal resources per `ffx_opticalflow_private.h` /
   `ffx_frameinterpolation_private.h`, dispatches the seven OF passes on two PNG frames
   captured from a real gamescope session (first-person title, HUD visible), then the FI
   passes with **zeroed dilated MVs, constant depth, and `reset=false` after the first
   frame**, at `displaySize == renderSize`.
3. Inspect the interpolated PNG for HUD edges, weapon edge, and a fast-turn pair; time
   the dispatches with timestamp queries on the AMD desktop and the Intel laptop.

If the output is not clearly usable at step 3, stop; nothing in §5 becomes worth doing.
If it is, the next milestone is the §5.3 pre-pass integration *without presenting* (log
or dump the generated frame), then the §5.2 FG mode. Mirror
`lsfg-in-tree-port.md` §5's staging; resolve vendoring-vs-reimplementing (§4.2) before
the second milestone.

## 9. What we could not establish

- **Any measurement of FI's output without depth/MVs.** None exists publicly; §3.3 and
  §5.6 are reasoned from the shader structure.
- **Per-pass OF and FI GPU timings** from AMD. Only fps pairs (§5.4) and the FSR 4 ML
  figure are published; the estimates are derived.
- **Whether FI consumes OF's scene-change flag as a reset** or merely as a zero field
  (§5.6). The host header passes `opticalFlowSceneChangeDetection` in; the consuming
  shader was not read.
- **Whether FI 1.1.x can generate more than one frame per pair.** The `ffx_api`
  dispatch has `outputs[4]` / `numGeneratedFrames`; this scout did not confirm the 3.1
  shaders support `t ≠ 0.5`. Assume 2× only.
- **Intel integrated-GPU performance and correctness** for these shaders under ANV.
- **AMD's own AFMF requirements text** (amd.com timed out on every attempt); AFMF's
  fallback behaviour is cited from press coverage.
- **What the user wants FG for** — a 120 Hz+ desktop panel (where §5.2 works) or the
  laptop (where it does not) — and whether the *in-game* HUD artifacts that every
  colour-only method shares are acceptable to them, which is the same open question
  `lsfg-in-tree-port.md` §"Open questions" 4 already asks.

---
*Checked 2026-09-05 against gamescope-ritz `feature/overlay-e2` (HEAD `1b6eb6f`) and
FidelityFX SDK tags `v1.1.3` / `v1.1.4` (headers, host `.cpp`, GLSL wrappers, CMake and
docs fetched raw from GitHub; tree listings and sizes via `gh api git/trees`), the SDK
`main` README and `version_2_0_0.md`, and the GPUOpen manuals. Line numbers in the SDK
citations are as fetched; re-verify before any implementation work. Nothing in this
tree was modified other than adding this file.*
