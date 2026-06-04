# Compute Software Rasterizer

[![Build Release](https://github.com/theworker02/compute-shader-software-rasterizer/actions/workflows/build-release.yml/badge.svg)](https://github.com/theworker02/compute-shader-software-rasterizer/actions/workflows/build-release.yml)
[![Docs](https://github.com/theworker02/compute-shader-software-rasterizer/actions/workflows/docs.yml/badge.svg)](https://github.com/theworker02/compute-shader-software-rasterizer/actions/workflows/docs.yml)
[![Release](https://img.shields.io/github/v/release/theworker02/compute-shader-software-rasterizer?display_name=tag)](https://github.com/theworker02/compute-shader-software-rasterizer/releases)

This repository now contains a compute-only tiled software rasterizer implemented in HLSL and a minimal D3D12 host wrapper for dispatching it.

It also includes a tiny Win32/D3D12 demo shell that opens a window, creates a swapchain, uploads a small test scene and texture, dispatches the compute rasterizer, and copies the compute-produced color texture into the swapchain backbuffer for presentation.

## Files

- `shaders/ComputeSoftwareRasterizer.hlsl`
  Multi-pass GPU rasterizer covering clear, vertex transform, triangle setup/cull, tile binning, fine rasterization, perspective-correct interpolation, manual bilinear filtering, depth testing, and atomic depth/color commit.

- `src/SoftwareRasterizerD3D12.h`
  Public host-side configuration and dispatch interface.

- `src/SoftwareRasterizerD3D12.cpp`
  Minimal D3D12 integration skeleton showing resource sizing, DXC shader compilation, PSO creation, descriptor-table binding, UAV barriers, and pass dispatch order.

- `src/DemoApp.cpp`
  Minimal application shell that sets up the device, swapchain, upload resources, descriptor heap, dispatch loop, and present copy path.

- `src/main.cpp`
  Win32 entry point for the demo application.

- `CMakeLists.txt`
  Basic Windows build entry for the demo and rasterizer wrapper.

## Pipeline

1. `CS_Clear`
   Resets visible counters, tile bins, tile depth hints, and the packed depth/color framebuffer.

2. `CS_TransformVertices`
   Transforms object-space vertices into clip space, computes screen-space coordinates, `1 / w`, and pre-divided attributes.

3. `CS_SetupAndCull`
   Performs trivial clip rejection, guard-band rejection, backface culling, and screen-space triangle setup.

4. `CS_BinTriangles`
   Assigns visible triangles into fixed-capacity `16x16` tile bins using atomic tile counters.

5. `CS_FineRasterize`
   Runs one `[numthreads(16,16,1)]` workgroup per tile, evaluates edge functions, computes perspective-correct barycentrics, shades, bilinearly samples the texture manually, and commits depth/color with a 64-bit CAS loop.

6. `CS_Present`
   Unpacks the color payload from the packed depth/color buffer into an `R8G8B8A8` UAV texture that the host copies into the swapchain.

## Runtime toggles

- `F1`: Toggle tile heatmap diagnostics.
- `F2`: Toggle the 32-bit packed fallback path.
- `F3`: Cycle MSAA sample count between `1x`, `2x`, and `4x`.
- `F4`: Cycle projection mode between linear, cylindrical, spherical, and hyperbolic warps.
- `F5`: Save the current frame CRC as a validation reference.
- `F6`: Compare the current frame CRC against the saved reference.
- `F7`: Toggle portal redirection hooks on the medium triangle.
- `F8`: Toggle topology-fold traversal on the primary triangle.
- `F9`: Toggle the procedural cluster-stream path that bypasses static mesh indexing.
- `F10`: Toggle stochastic sub-pixel sampling jitter.
- `F11`: Cycle audio debug visualization between off, tile acoustic field, and per-pixel material density.
- `F12`: Toggle temporal reprojection and history blending.

## Build

From a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Important assumptions

- Shader model `6.6` is required for the packed `uint64_t` depth/color commit path.
- The current implementation uses guard-band rejection instead of full homogeneous clipping for straddling triangles.
- Tile binning uses fixed per-tile capacity. Overflow is counted in `gTileOverflow` so the renderer can detect when `gMaxTrianglesPerTile` needs to be increased.
- The current fine pass cooperatively caches tile triangle IDs into `groupshared` memory and clamps per-tile processing capacity to `256` triangle references per tile.
- The D3D12 wrapper is a compact integration skeleton. Descriptor heap binding and UAV/SRV table wiring still need to be connected to your engine.
- The dispatch entry point expects prebuilt GPU descriptor-table handles for `t0..t2` and `u0..u15`, and your command list must already have the matching descriptor heap bound.
- The bundled demo now renders a small multi-triangle test scene, including a micro-triangle that exercises the tiny-triangle splat path.
- The 32-bit fallback path packs `24-bit` depth plus `8-bit` luminance. It is intended as a compatibility/debug path rather than a full-fidelity color mode.
- The wrapper supports shader hot-reload by watching `shaders/ComputeSoftwareRasterizer.hlsl` and rebuilding the compute PSOs when the file timestamp changes.
- Near-plane clipping is now handled during triangle setup with a Sutherland-Hodgman style clipper that can emit up to two triangles per input primitive.
- The manual texture sampler now supports analytical mip selection by evaluating perspective-correct UVs at neighboring pixel offsets inside the compute shader.
- The demo emits timestamp-based GPU frame timings to the debug output and supports CRC-based frame validation via present-texture readback.
- Pass 1 now supports non-linear projection warps controlled by a projection mode and strength value in the constant buffer.
- A dedicated profiling counter buffer now reports tile occupancy, Hi-Z skips, wave coverage ratio, atomic contention proxy, micro-splat hits, and portal/topology activity through the debug output.
- Portal routing now writes a per-pixel portal mask during fine rasterization and composites a redirected view tint in the present pass, while topology traversal can fold interpolated UV space on a per-triangle basis.
- The setup pass now supports a procedural cluster-stream mode that emits triangles analytically from `triIndex`, allowing the renderer to bypass the uploaded index buffer for higher-detail experimental geometry paths.
- Fine rasterization now supports deterministic stochastic sub-pixel jitter keyed by frame index, pixel location, triangle seed, and sample index to reduce sub-pixel aliasing without allocating a larger MSAA target.
- The rasterizer also writes per-pixel material density and per-tile acoustic occlusion/diffraction fields, which the demo can summarize in debug output or visualize directly via the audio debug mode.
- The present pass now performs temporal reprojection using compact per-pixel velocity metadata packed into the 32-bit auxiliary buffer, reprojects previous history, and rejects unstable history samples when depth divergence exceeds a configurable threshold.
- The repository now includes GitHub Actions workflows for tagged release packaging and GitHub Pages documentation publishing, along with Doxygen-backed architecture notes and contributor templates under `.github/`.
