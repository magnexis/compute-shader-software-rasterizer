# Woah Man Architecture Hub

## Overview

`woah_man` is a compute-only software rasterizer that bypasses the fixed-function graphics pipeline and executes visibility, tiling, fine rasterization, temporal reconstruction, and experimental acoustic-field generation in HLSL compute passes.

## Documentation Map

- [Architecture Notes](architecture.md)
- [Performance Matrix Template](performance-matrix.md)

## Key Components

- `CS_Clear`: resets per-frame counters, packed targets, history metadata, and acoustic fields
- `CS_TransformVertices`: performs clip-space transformation and non-linear projection warps
- `CS_SetupAndCull`: handles indexed or procedural triangle setup, clipping, culling, and metadata assignment
- `CS_BinTriangles`: assigns visible triangles to `16x16` tile queues
- `CS_FineRasterize`: evaluates coverage, interpolates attributes, updates packed depth/color, stores temporal metadata, and writes acoustic/material fields
- `CS_Present`: resolves debug views, applies temporal history blending, and writes the display surface

## Pages Deployment

The GitHub Pages workflow builds this documentation with Doxygen and publishes the generated HTML from the `docs` target.
