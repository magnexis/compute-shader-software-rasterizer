# Architecture Notes

## Packed Pixel Layouts

### 64-bit depth/color payload

- Bits `63:32`: unsigned normalized depth
- Bits `31:24`: alpha
- Bits `23:16`: blue
- Bits `15:8`: green
- Bits `7:0`: red

### 32-bit temporal payload

- Bits `31:16`: quantized depth16
- Bits `15:8`: signed normalized velocity X
- Bits `7:0`: signed normalized velocity Y

When the compatibility path is enabled, the same 32-bit resource is repurposed as:

- Bits `31:8`: quantized depth24
- Bits `7:0`: luminance8

## Interlocked Commit Loop

The fine rasterizer uses optimistic compare-exchange on the packed depth/color buffer:

1. Read the current packed value
2. Unpack depth and reject if the incoming fragment is farther away
3. Blend or replace color locally
4. Attempt `InterlockedCompareExchange`
5. Retry on contention until successful or rejected

## Temporal Reconstruction

Temporal reconstruction stores depth plus a compact motion vector in the 32-bit per-pixel metadata buffer. The present pass reprojects previous history using that velocity, rejects history if depth divergence exceeds a threshold, and stores the resolved color into a persistent history texture for the next frame.

## Acoustic Fields

The renderer accumulates:

- Per-pixel material density identifiers
- Per-tile acoustic occlusion estimates
- Per-tile diffraction edge counts

These fields are intended as reusable side-channel data for future audio experiments rather than a finished acoustics simulation.
