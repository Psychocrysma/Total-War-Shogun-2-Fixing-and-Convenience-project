# Test checklist for build 0.8.4

Each test takes a minute. Send back `tws2fix.log` after the last one; it is the main thing I need.

## 1. Loader (nothing should change)

- Copy `dinput8.dll` and `tws2fix.ini` into the game folder. Leave `tws2fix.ini` as shipped (`borderless=0`).
- Start the game, reach the main menu, start or load a campaign, play one turn, quit.
- Expected: the game behaves exactly as before. `tws2fix.log` in the game folder contains
  `engine: empire.retail.dll ... (known May 2023 build)`, `installing patches (launcher GetProcAddress)`,
  and one or more `CreateWindowExW` lines.
- If the game fails to start: delete `dinput8.dll`, confirm it starts again, and send the log (or say
  that no log was written).

## 2. Cursor clip

- With two monitors, in campaign and in a battle, push the mouse against the screen edge on the side of
  the other monitor. The map should scroll and the cursor should stay on the game screen.
- Alt-tab out: the cursor must be free. Alt-tab back: clipped again.
- Open an in-game dialog (quit confirmation): the cursor should be free while the dialog has focus. If it
  is not, note it; the clip only follows the main window being in the foreground.

## 3. Borderless (optional)

- In the game's graphics options set Windowed and your desktop resolution; restart the game.
- Set `borderless=1` in `tws2fix.ini` and start the game.
- Expected: no title bar or frame, the window fills the monitor, alt-tab is instant.
- Note anything odd: wrong size, a visible frame, mouse offset, flicker on alt-tab.
- Set `borderless=0` again afterwards if you prefer exclusive fullscreen.

## 4. Agent chance (the Metsuke 95% bug)

- Start or load a campaign with a fresh (level 1) metsuke and an enemy monk or ninja of level 4 or higher
  nearby; a save from the mid-game is ideal. A level-1 ninja against a high-level general works too.
- Hover the apprehend / assassinate action: the tooltip chance should now be a small number (5% or a
  little more), where before it showed 95%.
- Also check a strong agent against a weak target still shows a high chance, and that a mid-range match
  shows a sensible middle value. The log line `agent_chance_clamp: 18 of 18 bytes patched` confirms the
  patch went in; if it says `byte mismatch` or `skipped`, send the log.

## 5. UI scaling

- Default `ui_scale=auto`: above 1920x1080 the UI gets the size it has at 1080p (1.333 at 2560x1440, 2.0
  at 4K); at 1080p or below nothing changes. Start the game.
- Expected: menus, campaign and battle UI enlarged, crisp, mouse aligned everywhere, including
  campaign-map selection, battle unit cards and the battle drag-selection box (it starts under the cursor).
  Loading screens stay as they were.
- Report any screen where the mouse and the UI disagree, with a screenshot. For a developer:
  `ui_scale_log_callers=1` logs the engine code that asks for the UI rect.

## 6. Video memory override (0.4.0)

- Set `vram_mb=4095` (DirectX 11) or `vram_mb=3072` (DirectX 9) and start the game. The log should contain
  `vram_mb: force_total_video_memory = 4095 MB`.
- Play a few turns and a battle at your usual texture settings. What to look for: textures that used to
  drop to low detail (unit textures after alt-tab, far buildings) staying sharp, and no new crash when
  loading a battle. If the game crashes at battle load in DX9 mode, lower the figure or set it to 0.

## 7. Settlement and tree render box

- Only matters on an enlarged map. With `settlement_box=1000` the log should say
  `settlement_box: bounds set to +-1000 map units (ok)`; on the vanilla map nothing visible changes.
- 0.7.0: trees had the same ±350 limit in their own container. The log should also say
  `tree_box: campaign tree bounds set to +-1000 map units (ok)`. On the enlarged map, forests beyond
  the old line (Hokkaido, the far south-west) should now be drawn; with `settlement_box=0` they vanish
  again, which is the A/B check.
- On the Expanded Japan / Hokkaido map: settlement and building models beyond the old ±350 line should now
  be drawn. If they still are not, the models are missing for another reason (the data side).

## 7b. Movement-range area on rescaled maps (automatic)

- Default `movement_cell_size=auto`. At start-up the log says `movement_cell_size: auto ON ... (budget
  scaling on)`.
- Vanilla campaign: the log lists the map's grid with `2.0000-unit cells (overlay already matches)`;
  the yellow area is unchanged.
- Expanded Japan k200 pack: the log says `map grid N has 4.0000-unit cells ...`. Select an army with full
  movement: the yellow area ends where a right-click-held path turns red (confirmed for armies; fleets
  and agents come out slightly short, known).
- `movement_debug_log=1` logs each budget scaling (`movement-range budget 50 -> 25 (cell 4.0000)`).

## 8. Battle fixes (0.5.0)

- Start the game with the shipped INI (`[battle]` all on). In `tws2fix.log` every fix should log that it is ON
  (`pathfix: ...`, `artfix: ...`, `fbrfix: ...`, `siegefix: ...`, `wallfix: ...`, `zombiefix: ...`,
  `collapsefix: ...`, `groupfix: ...`, `guardfix: ...`, `kneelfix: ON ...`). A `REFUSED` line means that fix
  found different bytes and installed nothing; send the log.
- Play one field battle and one siege (attack and defence if you can). Things that should now be different:
  - Select one unit and press G or Ctrl+1: it forms a group.
  - Order a unit through a gate: it runs through as a column and re-forms beyond instead of filing one man at a time.
  - Artillery ordered to fire at a unit aims at the middle of the enemy line.
  - Matchlocks with Fire by Rank start their first volley without the ~10 s wait; Hattori/Ikko gun units keep firing past three volleys.
  - Guard-mode archers stay put when their target walks out of range.
  - Siege defence: order a unit onto a wall section; it goes straight there. Men on a gatehouse survive Start Battle with open gates.
  - FotS: a unit with Kneel Fire on takes a move order.
- Any `SELF-CHECK` line in the log means a fix switched itself off; send the log with a note of what was happening.
- Do not use these in multiplayer; set `battle_fixes=0` first.

## 9. Alt group-move

- Battle, select 4–6 units in some shape. Hold Alt, press and hold the left button on the ground and move
  the mouse: green ghosts in the selection's shape follow the cursor; each unit keeps its frontage and
  facing. Release: the units march there and re-form in that shape.
- While holding, also hold Ctrl and move the mouse sideways: the block turns in place about its middle and
  does not move; release Ctrl and it follows the cursor again from where it stands.
- Plain left click / drag still selects, plain right click / drag still orders as in vanilla.
- If something is off: set `alt_debug_log=1` under `[battle]`, repeat once, and send `tws2fix.log`
  (it then records per-hold traces: `alttrace:`, `altmeasure:`, `altmove:` lines).

## What to send back

- `tws2fix.log` after the tests.
- For any crash: the Windows "Shogun 2 has stopped working" details if shown, and whether it happened
  before the main menu, at the menu, at campaign load, or in play.
