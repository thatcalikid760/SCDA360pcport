# DLSS (like the Bloodborne PC port) - plan (2026-10-31)

Reference: github.com/Supermedo/bloodborne_pc `gpu/dlss_bridge` (MIT; the repo itself is GPL-2.0, so design
reference only). Its evaluate call takes colour, hardware depth, render-resolution motion vectors (current to
previous), and the frame's sub-pixel jitter; `nvngx_dlss.dll` is loaded at run time and the feature is off when it
is missing.

## What DLSS needs from us

| Input | Status here |
|-|-|
| Colour at the internal resolution | Yes: the frontbuffer resolve (before the gamma ramp). |
| Depth | Yes: the scene's EDRAM depth target (`depthTargets`), needs picking the main scene pass. |
| Jitter | Possible: a per-frame sub-pixel offset added in our viewport transform (`SharedConstants`, translated VS). |
| Motion vectors | Missing. The game never made them. Camera-only vectors can be rebuilt from depth and the current and previous view-projection matrices, once those are found in the scene VS constants. Moving characters and objects would still ghost without per-object vectors. |
| SDK | NVIDIA DLSS SDK (NGX, D3D11 path). Its license allows shipping `nvngx_dlss.dll`; it is not GPL, so it stays an optional run-time DLL like the reference. |

## Steps

1. Find the scene's view-projection matrix in VS constants per frame (trace VS c0-c31 on the main pass,
   `SCDA_TRACE_PS` / `SCDA_TRACE_CONSTS` already dump them).
2. Jitter (Halton 2,3) in the viewport transform, behind an option; check the image stays stable without DLSS.
3. Camera motion-vector pass: depth + inverse(current VP) + previous VP -> RG16F at the internal resolution.
4. NGX D3D11 bridge loaded at run time (`nvngx_dlss.dll` next to scda.exe); Upscaling menu gains DLSS
   Quality / Balanced / Performance / Ultra Performance / DLAA when it loads.
5. Fallback: today's Lanczos path when DLSS is missing (AMD/Intel GPUs).

Risk: steps 1 and 3 decide quality; with camera-only vectors, fast-moving NPCs may smear.
