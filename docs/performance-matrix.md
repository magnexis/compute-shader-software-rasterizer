# Performance Matrix Template

This page is intentionally structured as a lightweight baseline for future profiling runs across GPU generations.

| GPU | Driver | Resolution | Frame Time (ms) | Wave Coverage % | Atomic Contention % | Avg Tile Refs | Max Tile Refs | Notes |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| RTX-class | TBD | 1280x720 | TBD | TBD | TBD | TBD | TBD | Fill in after GPU capture |
| RDNA-class | TBD | 1280x720 | TBD | TBD | TBD | TBD | TBD | Fill in after GPU capture |
| Integrated | TBD | 1280x720 | TBD | TBD | TBD | TBD | TBD | Fill in after GPU capture |

## Suggested Capture Procedure

1. Run the demo in Release mode
2. Capture 300 steady-state frames
3. Record debug-output counters for wave coverage, contention proxy, and acoustic fields
4. Correlate with a GPU profiler capture for cache and occupancy validation
