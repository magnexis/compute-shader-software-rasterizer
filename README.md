# Ultimate Compute Software Rasterizer

[![Build Release](https://github.com/theworker02/compute-shader-software-rasterizer/actions/workflows/build-release.yml/badge.svg?branch=main)](https://github.com/theworker02/compute-shader-software-rasterizer/actions/workflows/build-release.yml)
[![Docs](https://github.com/theworker02/compute-shader-software-rasterizer/actions/workflows/docs.yml/badge.svg?branch=main)](https://github.com/theworker02/compute-shader-software-rasterizer/actions/workflows/docs.yml)
[![Latest Release](https://img.shields.io/github/v/release/theworker02/compute-shader-software-rasterizer?display_name=tag)](https://github.com/theworker02/compute-shader-software-rasterizer/releases/tag/v1.0.0)
[![Downloads](https://img.shields.io/github/downloads/theworker02/compute-shader-software-rasterizer/total)](https://github.com/theworker02/compute-shader-software-rasterizer/releases/tag/v1.0.0)
[![C++](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](https://isocpp.org/)
[![HLSL](https://img.shields.io/badge/HLSL-Compute%20Shaders-1F6FEB)](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl)
[![Driver](https://img.shields.io/badge/Driver-D3D12%20User--Mode-0A7EA4)](https://learn.microsoft.com/en-us/windows/win32/direct3d12/directx-12-programming-guide)
[![Graphics](https://img.shields.io/badge/Graphics-GPU%20Rasterization-8A2BE2)](https://en.wikipedia.org/wiki/Rasterisation)
[![Shaders](https://img.shields.io/badge/Shaders-HLSL%20%7C%20DXIL-228B22)](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl)

Ultimate is a compute-only, tiled, GPU-accelerated software rasterizer built around HLSL compute shaders and a compact D3D12 host wrapper. The project intentionally bypasses the fixed-function raster pipeline and executes the full visibility and shading path inside compute:

- vertex transformation
- triangle setup
- backface and guard-band culling
- near-plane clipping
- tile binning
- coarse Hi-Z rejection
- fine rasterization
- perspective-correct interpolation
- manual texture sampling
- atomic depth and color updates
- temporal reconstruction
- diagnostics and experimental side-channel outputs

This repository also includes a small Win32 and D3D12 demo that uploads a test scene, dispatches the compute rasterizer, resolves the packed render target, and presents the result to the swapchain.

## Quick Links

- Repository: `https://github.com/theworker02/compute-shader-software-rasterizer`
- Documentation site: `https://theworker02.github.io/compute-shader-software-rasterizer/`
- Current release: `https://github.com/theworker02/compute-shader-software-rasterizer/releases/tag/v1.0.0`
- Current downloadable release asset: `https://github.com/theworker02/compute-shader-software-rasterizer/releases/download/v1.0.0/ultimate.zip`

## Why this project exists

Most GPU rasterization tutorials stop at either:

- the hardware graphics pipeline, or
- a toy software rasterizer running on the CPU

This project explores a middle ground:

- keep the work on the GPU
- keep the whole visibility path programmable
- use compute dispatch and explicit buffers instead of graphics pipeline state
- expose enough internal state to profile and experiment with non-traditional rendering behaviors

That makes the repository useful for:

- low-level graphics learning
- compute-driven rendering research
- tile-based raster experiments
- atomic contention and occupancy profiling
- unusual projection and topology work
- future integration ideas such as audio-field generation and procedural cluster streams

## Project status

The repository is in an advanced prototype state.

What is already present:

- multi-pass compute raster pipeline in HLSL
- D3D12 host wrapper and demo application
- manual bilinear texture sampling in compute
- 64-bit packed depth and color atomic commit path
- compatibility-oriented 32-bit packed fallback path
- tile heatmaps and profiling counters
- portal and topology experiment hooks
- procedural cluster-stream mode
- stochastic sub-pixel jitter
- temporal reprojection and history blending
- GitHub Actions for docs and release packaging
- GitHub Pages documentation publishing

What is still important to understand:

- this is not yet a drop-in engine module
- several paths are experimental and tuned for inspection rather than product polish
- the D3D12 integration is intentionally compact and educational
- some release workflow steps are permissive around non-Windows shader blob generation because the shader is DXIL-first

## Repository layout

### Core source files

- `shaders/ComputeSoftwareRasterizer.hlsl`
  The main compute shader file containing the raster passes and utility logic.

- `src/SoftwareRasterizerD3D12.h`
  Public API, settings, constants, and host-side structures used by the wrapper.

- `src/SoftwareRasterizerD3D12.cpp`
  D3D12 integration, pipeline creation, descriptor setup expectations, resource sizing, and dispatch orchestration.

- `src/DemoApp.cpp`
  Demo application setup, swapchain flow, upload path, timing, validation, toggles, and profiling output.

- `src/DemoApp.h`
  Demo-level configuration and state.

- `src/main.cpp`
  Win32 entry point.

### Documentation files

- `docs/index.md`
  Main documentation landing page used by Doxygen.

- `docs/architecture.md`
  Focused notes on buffer packing, atomic updates, temporal metadata, and acoustic side-channel outputs.

- `docs/performance-matrix.md`
  Lightweight capture template for profiling across hardware targets.

- `CONTRIBUTING.md`
  Contributor and workflow guidance.

### Automation

- `.github/workflows/build-release.yml`
  Multi-platform build and release packaging workflow.

- `.github/workflows/docs.yml`
  Doxygen build plus GitHub Pages publishing workflow.

## High-level architecture

The renderer is organized as a sequence of compute passes. Each pass writes explicit buffers that feed the next stage. No fixed-function raster state is required to produce the final image.

### Pass overview

1. `CS_Clear`
   Resets counters, tile bins, packed render targets, temporal metadata, debug buffers, and acoustic fields.

2. `CS_TransformVertices`
   Transforms object-space vertices into clip space, computes reciprocal `w`, screen coordinates, and pre-divided attributes.

3. `CS_SetupAndCull`
   Reads indexed or procedural triangles, performs near-plane clipping, rejects trivial out-of-bounds primitives, computes signed area, applies backface culling, and writes visible triangle setup data.

4. `CS_BinTriangles`
   Maps visible triangles into `16x16` screen tiles using fixed-capacity tile queues and tracks overflow.

5. `CS_FineRasterize`
   Launches `[numthreads(16, 16, 1)]`, one group per tile, evaluates edge functions, computes barycentrics, reconstructs perspective-correct attributes, shades fragments, and performs atomic depth and color commits.

6. `CS_Present`
   Resolves debug view modes, applies temporal history blending when enabled, and writes the presentable output texture.

### Work decomposition

The central design choice is:

- one tile equals one compute thread group during fine rasterization
- one lane equals one pixel inside that tile
- triangle setup is performed once and shared
- global memory traffic is reduced by using `groupshared` staging for the tile-local triangle reference list

This gives a clean mental model:

- setup work scales with triangle count
- fine raster work scales with tile coverage and tile occupancy
- the packed pixel update path isolates write hazards to an atomic commit loop

## Data flow and resource model

The pipeline works on explicit SRV and UAV resources rather than render targets and depth attachments from the graphics pipeline.

### Main input resources

- `StructuredBuffer<Vertex>`
  Stores positions, normals, and UVs.

- `StructuredBuffer<uint>`
  Indexed triangle lookup.

- `Texture2D<float4>`
  Source texture sampled manually in the compute path.

- constant buffer
  Viewport dimensions, matrix data, debug toggles, temporal settings, projection controls, and experimental mode flags.

### Main intermediate resources

- transformed vertex output buffer
- visible triangle setup buffer
- tile count and tile offset buffers
- tile triangle reference buffer
- packed depth and color buffer
- auxiliary packed metadata buffer
- acoustic and material debug buffers
- profiling counters
- history textures and history metadata

### Main output resources

- packed framebuffer
- resolved present texture
- optional history texture for temporal accumulation

## Raster algorithm details

### Triangle area and backface culling

Backface culling is performed from screen-space triangle area:

`area2 = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)`

If `area2 <= 0`, the triangle is culled.

### Edge functions

Coverage uses standard edge-function tests evaluated at pixel centers or sample locations:

- `E12(P)`
- `E20(P)`
- `E01(P)`

Fragments pass when all three edge functions are non-negative under the chosen winding convention.

### Perspective-correct interpolation

The setup pass stores:

- `1 / w`
- `uv / w`
- `normal / w`

The fine pass reconstructs attributes at the pixel:

- `invWp = alpha * invW0 + beta * invW1 + gamma * invW2`
- `Wp = 1 / invWp`
- `uv = (alpha * uv0OverW + beta * uv1OverW + gamma * uv2OverW) * Wp`

Normals are reconstructed similarly and normalized before shading.

### Manual texture filtering

Texture sampling is not delegated to hardware filtering. The compute path:

- converts UV into texel space
- computes integer sample locations
- loads four texels explicitly
- linearly interpolates them using the fractional offset

Analytical mip selection is supported by evaluating neighboring UV gradients in compute.

### Atomic depth and color update

The preferred path uses a `uint64_t` packed payload:

- upper 32 bits: unsigned normalized depth
- lower 32 bits: `RGBA8`

The commit loop:

1. reads the current packed value
2. unpacks the previous depth
3. rejects the incoming fragment if it is farther away
4. packs the new depth and color
5. attempts `InterlockedCompareExchange`
6. retries on contention until success or rejection

This keeps the depth and color commit race-free in the high-fidelity path.

### 32-bit fallback path

The fallback mode packs:

- `depth24`
- `luminance8`

This path exists primarily for compatibility and debugging. It is not intended to match the full visual quality of the 64-bit mode.

## Tile-based execution model

Tiles are fixed at `16x16`.

Why `16x16`:

- straightforward mapping from thread lane to pixel
- compact tile-local state
- natural occupancy across modern GPU scheduling models
- easy bounds math for binning

Fine pass thread group shape:

```hlsl
[numthreads(16, 16, 1)]
```

This gives `256` threads per tile. In practice that maps to:

- 8 wave32 groups on hardware using wave32 execution
- 4 wave64 groups on hardware using wave64 execution

The shader keeps divergence manageable by:

- having all lanes process the same tile triangle list
- caching tile triangle IDs in `groupshared` memory
- early-outing work with wave-level predicates where appropriate
- using coarse rejection before expensive interpolation and sampling

## Experimental systems included in the repository

This project is more than a single raster loop. Several experimental systems are intentionally left visible because they are useful for future research.

### Heatmap diagnostics

The fine pass can write tile occupancy and contention-related counters that the present pass visualizes as a heatmap.

### Portal redirection

Portal-tagged triangles can write a mask that the present pass uses to tint or redirect the final image region.

### Topology folding

Topology-tagged triangles can remap interpolated UV space to emulate folded or redirected surface traversal behavior.

### Procedural cluster-stream mode

Instead of consuming only the uploaded index buffer, the setup path can emit triangles procedurally from triangle index and control settings. This is useful for experiments that bypass traditional mesh indexing.

### Micro-triangle splat path

Tiny triangles are detected analytically during setup and can route into a splat-like fast path instead of incurring the full multi-sample inner loop.

### Stochastic sub-pixel sampling

Deterministic jitter keyed from frame index, pixel location, sample index, and triangle seed helps test anti-aliasing and sub-pixel coverage strategies without a larger MSAA target.

### Temporal reprojection

The renderer stores compact previous-frame metadata and uses that data during present to blend history, reject unstable reprojection, and reduce temporal noise.

### Acoustic side-channel output

The fine pass can emit:

- per-pixel material density
- per-tile acoustic occlusion estimates
- per-tile diffraction indicators

These fields are exploratory and intended for future audio and simulation experiments rather than a final acoustics product.

## Host integration model

The included D3D12 wrapper is meant to be understandable first and extensible second.

What it already demonstrates:

- DXC compilation of compute entry points
- root-signature-compatible descriptor table expectations
- UAV barrier sequencing between passes
- dispatch ordering
- swapchain resolve and copy path
- readback for validation and profiling

What an engine integrator should still expect to do:

- adapt descriptor heap management to the engine
- adapt resource lifetime and residency rules
- replace the sample scene upload path
- connect real frame graph or task graph barriers
- integrate the wrapper with engine debug and telemetry systems

## Build requirements

### Windows runtime build

The demo is Windows and D3D12 focused.

Recommended environment:

- Windows 10 or newer
- Visual Studio 2022 or compatible MSVC toolchain
- D3D12-capable GPU and drivers
- DXC available on path if you want DXIL blob generation in workflow parity
- CMake

Build steps:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Expected output:

- `build/Release/woah_man.exe`

### Documentation-only build

If you only want docs generation:

```powershell
cmake -S . -B build-docs -DWOAH_MAN_BUILD_DEMO=OFF -DWOAH_MAN_BUILD_DOCS=ON
cmake --build build-docs --target docs
```

### Notes on non-Windows builds

The project can still configure and package sources outside Windows for CI and documentation purposes, but the full runtime demo is intentionally centered on D3D12.

## Runtime controls

The demo exposes runtime switches for visual inspection and regression testing.

- `F1`
  Toggle tile heatmap diagnostics.

- `F2`
  Toggle the 32-bit packed compatibility path.

- `F3`
  Cycle MSAA sample count between `1x`, `2x`, and `4x`.

- `F4`
  Cycle projection mode between linear, cylindrical, spherical, and hyperbolic.

- `F5`
  Save a reference CRC from the current frame.

- `F6`
  Validate the current frame against the saved CRC.

- `F7`
  Toggle portal traversal visualization.

- `F8`
  Toggle topology fold traversal.

- `F9`
  Toggle procedural cluster-stream mode.

- `F10`
  Toggle stochastic sub-pixel jitter.

- `F11`
  Cycle audio debug view:
  - off
  - tile acoustic field
  - per-pixel material density

- `F12`
  Toggle temporal reprojection and history blending.

## Validation and profiling support

The repository contains several layers of verification support.

### Runtime diagnostics

- timestamp-based GPU timing
- CRC-based output validation
- tile occupancy counters
- overflow counters
- wave coverage ratio proxy
- atomic contention proxy
- acoustic field summaries

### Performance matrix

The `docs/performance-matrix.md` page provides a template for gathering comparative data across:

- GPU vendors
- driver versions
- resolutions
- occupancy characteristics
- tile reference distribution

### Suggested workflow

1. build in `Release`
2. run a stable scene
3. collect CRC and frame time baselines
4. capture several hundred frames
5. compare debug counters before and after changes
6. supplement with a real GPU capture tool when diagnosing occupancy or cache effects

## Release and deployment model

### Release artifacts

The current manual release asset is:

- `ultimate.zip`

The release workflow now names future packaged artifacts using the `ultimate-*` convention:

- `ultimate-windows`
- `ultimate-ubuntu`
- `ultimate-macos`

### GitHub Pages

The documentation workflow:

- configures a docs-only build
- runs Doxygen
- uploads the generated HTML
- publishes it through GitHub Pages

Published site:

- `https://theworker02.github.io/compute-shader-software-rasterizer/`

### Workflow caveat

The release workflow treats non-Windows SPIR-V blob compilation as best-effort because the shader code currently prioritizes DXIL-first features such as the 64-bit packed path and related syntax choices. Failed SPIR-V blob attempts are surfaced as warnings rather than failing the full release pipeline.

## Current assumptions and constraints

This section is intentionally blunt so future contributors know where the sharp edges still are.

- shader model `6.6` is required for the main 64-bit packed atomic path
- the project is D3D12-first
- the demo is not a full engine abstraction
- tile binning uses a fixed-capacity queue and must watch overflow counters
- some experimental features are visual or dataflow probes rather than finished rendering features
- non-Windows packaging in CI is not equivalent to a full native runtime
- the 32-bit fallback is intentionally lower fidelity
- full clip-space robustness beyond the implemented near-plane handling may need future expansion if scenes become more pathological

## Known limitations

- no claim of feature parity with hardware graphics pipelines
- no full material system
- no shadowing system
- no full scene streaming architecture
- no exhaustive shader portability layer
- no production frame graph integration
- no automated image-diff CI at this time

## Reading order for new contributors

If you are onboarding to the project, this sequence will usually be the fastest path:

1. read this README fully
2. open `docs/index.md`
3. read `docs/architecture.md`
4. skim `src/SoftwareRasterizerD3D12.h`
5. inspect the pass order in `src/SoftwareRasterizerD3D12.cpp`
6. read `shaders/ComputeSoftwareRasterizer.hlsl`
7. run the demo and exercise all debug toggles

## Recommended next engineering steps

The repository already contains a large number of experimental features. The most valuable next steps are likely:

- stronger automated validation against known reference frames
- additional shader-side debug visualizations
- deeper profiling on multiple vendor architectures
- cleaner engine-facing abstraction boundaries
- clearer separation between stable and experimental feature flags
- optional data export for profiling counters and acoustic fields

## FAQ

### Is this a hardware-raster replacement?

Not in the sense of a production-ready universal replacement. It is a programmable compute rasterizer prototype with enough structure to render, profile, validate, and experiment on real GPU hardware.

### Why does the project still build an executable named `woah_man.exe`?

Because the branding and artifact naming have moved faster than the internal CMake target naming. The current runtime target name is still tied to the original project identifier. That can be renamed later if you want a full end-to-end naming pass.

### Why are some workflows permissive?

Because the project currently values successful packaging and documentation publication while still surfacing shader portability issues honestly. The warnings remain visible so portability debt does not disappear.

### Why publish README content through Doxygen and Pages?

Because the README acts as a strong single source of truth for:

- architecture overview
- build instructions
- feature inventory
- debugging guidance
- release and docs entry points

That keeps the repository landing page and the generated docs aligned.

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. The repository includes:

- issue templates
- a pull request template
- docs publishing workflow
- release packaging workflow

## License and usage note

No separate license file is described in this README today. If you plan to distribute or commercialize derivative work from this repository, add an explicit license file first so the legal posture is clear for contributors and downstream users.

## Closing summary

Ultimate is a deliberately ambitious compute-raster project. It is part renderer, part profiling sandbox, part research vehicle. The codebase is most useful when treated as a transparent system you can inspect, measure, and evolve rather than a black-box drop-in renderer. The README and GitHub Pages site are intentionally detailed so the project remains understandable even as the shader path and experiments grow more complex.
