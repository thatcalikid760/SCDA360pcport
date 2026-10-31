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

## P1. Sniper scope lens renders purple, with a cyan band

- **Symptom:** looking through the rifle scope in Kinshasa, the lens was a flat purple/magenta disc (FIXED by the
  resolve-container fix above: the lens now shows the scene). Still open: the lower screen outside the scope body
  turns flat cyan `(0,198,198)` from about row 590, and the last 5 rows pink `(255,198,255)`.
- **Repro:** `SCDA_LAUNCH_DATA=C:/SCDA-Recomp/_run/maps/07_Kinshasa01.launch`, scope raised around frames 2400-2800.
- **Evidence:** `_run/scope/final.png` (sheet), `_run/scope/scda_frame_2400.bmp`..`2800`, draw dumps `_run/scope/dd2/`,
  shader skips `_run/scope/skip_*.bmp`, traces `_run/scope/trace*.log`.
- **Suspected cause:** unknown. The constant colours look like unwritten memory or an uncleared target sampled by a
  full-screen scope pass. After-fix frame: `_run/s2/scope/scda_frame_2400.bmp`.
- **Status:** open (band only).

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
