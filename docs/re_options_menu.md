# Extra Settings inside the game's OPTIONS page (RE notes, 2026-10-31)

Goal: an EXTRA SETTINGS entry in the solo OPTIONS page that opens the F1 menu.

## What the OPTIONS page is

Solo menu > OPTIONS (3x DOWN from LOAD GAME, then A; `SCDA_AUTOPRESS=320:A,700:B,800:DOWN,850:DOWN,900:DOWN,980:A`,
frame 1060 shows it). One page:

- value rows: INVERT LOOK UP/DOWN, INVERT LOOK LEFT/RIGHT, AIMING SENSITIVITY, BRIGHTNESS, CONTRAST;
- two buttons: TV CALIBRATION, CONTROLLER LAYOUT.

## Where it comes from

- The UI is native C++ (no layout or widget names in `menu.lin`, `Common.lin` or `menu.tex`). Page classes have
  ASCII names followed by their vtable in `.rdata`: `P_Options` at 0x8212D2C4, vtable 0x8212D2D0
  (0x82A89B78, 0x82A88DE8, 0x82A89070, ..., slot 18 = 0x82A89D88).
- Text: `Localize(section, key)` = `sub_8258C230(r3 = section, r4 = key, r5 = package, ...)`, keys are UTF-16BE
  strings in the image (OPTIONSTITLE 0x8212C8D0, Calibrate 0x8212D5B0, ControllerTitle 0x8212D5C4,
  CControllerInvertLook 0x8212D568, CControllerAimingSensitivity 0x8212D4F8, CDisplayBrightness 0x8212D4D0,
  CDisplayContrast 0x8212D4AC). The strings live in the `.int` sections embedded in `Common.lin`
  (`[OPTIONSTITLE] Calibrate=TV CALIBRATION`).
- `sub_82A89D90` builds the page: finds children by name (`sub_82A812D8(container, L"name")`), sets their text
  (`sub_8278A0F8(widget, text)`), and adds list items (`sub_82C0B7F8(list, text, 0)`, list at page+128).
- Widgets are looked up by name in a tree: `sub_82BFC6A8` makes a name, `sub_82BFA690` finds a page,
  `sub_82C0BB90` finds a child (P_OptionPopup / LST_Options in `sub_82787EB8`).

## Done (2026-10-31, runtime/src/menu_hooks.cpp)

TV CALIBRATION is now EXTRA SETTINGS: `sub_8258C230` override rewrites the label (same length, English only),
`sub_82A88E60` override opens the F1 menu for B_Adjust_display on P_Options (`SCDA_NATIVE_CALIBRATION=1` = old).
Verified with autopress (`_run/re/nav_f`, `nav_g`). BRIGHTNESS and CONTRAST rows stay on the page.

## Original plan (a new button instead of reusing calibration)

1. Override `sub_82A89D90` (pattern: `runtime/src/audio_trace.cpp`, `PPC_FUNC(sub_X)` calling `__imp__sub_X`):
   after the original, add one more entry. Either a third button cloned from CONTROLLER LAYOUT's widget, or a row.
2. Find the A-press handler of the page (a vtable slot; trace which slot runs on A with a logging override) and,
   for our entry, post `kOverlayToggleMessage` instead.
3. Text: our own string (no localization lookup), "EXTRA SETTINGS".

Tools: `tools/lin_inflate.py` (inflate a .lin), `tools/xref_addr.py <addr>` (lis/addi references in generated/).
