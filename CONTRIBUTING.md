# Contributing

## Scope

This project is a compute-shader software rasterizer with experimental rendering, temporal reconstruction, and acoustic-field side channels. Contributions should preserve determinism where possible and document any intentional tradeoffs clearly.

## Style Guide

- Keep host code in modern C++20
- Keep shader code explicit and low-level; favor direct math over hidden abstractions
- Preserve constant-buffer alignment exactly between C++ and HLSL
- Prefer ASCII unless a file already requires Unicode

## Validation Expectations

Before opening a pull request:

1. Configure and build the project with CMake
2. Run the demo in Release mode if you changed runtime behavior
3. Capture CRC validation when touching rasterization math
4. Note any determinism changes, ghosting risks, or descriptor-layout changes

## Projection and Setup Changes

If you add a new projection mode or modify Pass 1:

1. Update both the HLSL constant buffer and the mirrored C++ struct
2. Re-check tile classifier assumptions after any screen-space warp changes
3. Verify clipped triangles still use the same warp path as unclipped geometry

## Temporal and Acoustic Features

- Temporal changes should explain how packed velocity metadata is written and consumed
- Acoustic changes should identify whether they affect tile-level occlusion, diffraction, or material-density outputs

## Pull Requests

- Keep changes focused
- Include before/after behavior notes
- Mention if the change affects descriptor counts, history resources, or workflow automation
