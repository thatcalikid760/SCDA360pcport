# Remaining bugs (2026-10-30)

Ranked by impact on a first release. Each entry: symptom, repro, evidence, suspected cause, status.
Evidence paths under `_run/` are local only (git-ignored).

## FIXED. Scale 2+ showed stale tiles (2026-10-30)

- **Symptom:** at `Scale=2`, screen rows 256 and below (EDRAM tiles 1-2) froze on an old image (01_Intro cutscene).
- **Cause:** predicated tiling resolves tiles 1-2 to bases inside tile 0's resolve texture. The renderer picked the
  containing texture by the first match in an `unordered_map`; an older texture that also spanned the base could win,
  and the map's order changes as it rehashes (hence timing- and run-dependent, worse at Scale 2).
- **Fix:** the most recently copied containing texture wins (`Resolved::lastCopy`). Verified on Intro frames
  380-440 at Scale 2 (`_run/s2/seq2/`); same frames froze before (`_run/s2/seq/`).

## FIXED. Sniper scope: cyan band and pink rows at the bottom of the screen (2026-10-31)

- **Symptom:** in Kinshasa scope view, the bottom ~20% of the screen turned flat cyan `(0,198,198)`, green over the
  scope body, and the last 5 rows pink. (The purple lens was fixed earlier by the resolve-container fix.)
- **Repro:** `SCDA_LAUNCH_DATA=C:/SCDA-Recomp/_run/maps/07_Kinshasa01.launch SCDA_AUTOPRESS=2150:X,2250:RS`
  (X draws the rifle, R3 raises the scope), dump frame 2400.
- **Cause:** the last post pass (PS `3a7a1a91e12b339a`) blends a 640x360 texture over the bottom of the screen with
  alpha `a * 2 * (5y - 4)`. That texture is resolved from EDRAM base 0x1C0, which the title "clears" with two
  D3D clear draws at the start of the frame. Both were lost:
  1. The clears put z at -2^-28 with `PA_CL_CLIP_CNTL.CLIP_DISABLE` set; D3D11 depth-clipped them. The rasterizer now
     honours bit 16 (`DepthClipEnable = !clip_disable`). This fixed the pink rows.
  2. The second clear is depth-only (`RB_MODECONTROL` 5) through a 4x alias with depth at the colour base. On
     Xenos the depth write replaces the colour tiles; our host depth target is separate, so the colour kept last
     frame's mask (cyan background, green scope body, alpha 0.8). Such draws now also write colour 0
     (`DepthOnlyOverwritesColor`).
- **Evidence:** `_run/scope3/` (traces `t4.txt`..`t7.txt`, per-draw dumps `dd*/`, fixed frames `fix/`).

## Open feature. Extra Settings inside the game's own main menu

- **Wanted:** an "EXTRA SETTINGS" entry in the game's main menu that opens the same settings.
- **Now:** the settings are the F1 / L3+R3 overlay; after sign-in the update notification names the shortcut.
- **Needs:** reverse-engineering the title's UnrealScript menu pages (UE2.5 GUI in `game/System` packages) to add
  an item and route its selection to the overlay. Not started.

## P2. Fog and light shafts differ slightly from the console

- **Symptom:** small brightness differences in fog and light shafts (Iceland Osprey bay, Okhotsk ice-hole edge).
- **Evidence:** `_run/census/` against `_run/ref/ref.mp4`.
- **Suspected cause:** Xenos clamps each blend write; Direct3D 11 does not (FP16 targets reach alpha 2-8).
- **Status:** open, low priority.

## P3. Scale 2+ edge row/column and faint line at a tile split

- **Symptom:** above `Scale=1`, some post passes miss one pixel row/column at the screen edge; a faint horizontal
  line can show about a third of the way down (EDRAM tile split).
- **Status:** open, cosmetic.

## P3. Brightness slider may not change the gamma ramp

- **Symptom:** unverified. The title uploads its gamma ramp twice at boot; the in-game brightness slider was never
  reached in a test, so whether it re-uploads the ramp is unknown.
- **Repro:** Options -> brightness, with `SCDA_TRACE_GAMMA=1`.
- **Status:** needs a check.
