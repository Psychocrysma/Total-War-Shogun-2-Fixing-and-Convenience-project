# Test checklist for build 0.8.9

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

## 4b. Clan selection (exact key)

- Needs a mod with a clan whose key contains another clan's key and sorts before it (Expanded Japan with
  the Hokkaido pack F51 or older: "menasunkur" contains "nasu").
- On the Sengoku campaign selection screen pick Nasu and start: the campaign must be Nasu's. The log says
  `clan_select_exact: ON` at start-up and `clan_select_exact: button_nasu matched a longer key first ...`
  when the fix changed the answer.
- Pick a few other clans (Oda, Hojo, Uesugi, Aki, Hatakeyama): each starts as itself, as before.

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

## 10. Battle rule fixes (new in 0.8.9)

Set `battle_rules_debug_log=1` in `[battle]` for these tests.

**AI accuracy on Hard / Very Hard (`ai_difficulty_accuracy`)**
- Campaign on Hard or above, a land battle against AI missile units (bows or matchlocks).
- With the default `ai_difficulty_accuracy=1` the log should show a line like
  `missile fire from a AI army with battle handicap 1` (Hard) or `2` (Very Hard / Legendary), and one for
  your own army (`human army with battle handicap 0`, or `-1` / `-2` depending on the setting).
- Set `ai_difficulty_accuracy=0`, restart, fight a similar battle: the log should say
  `ai_difficulty_accuracy: off ... (6 of 6 sites)` at start-up and `- its accuracy bonus is removed` on
  the AI line. AI archers should kill noticeably fewer men per volley than before.
- Note: if the AI line shows handicap 0 on Hard, the AI outnumbered you; the engine lowers its bonus
  when it already has the bigger army.

**Firing sounds (`projectile_sound_priority`)**
- Any battle with matchlocks, bows and fire arrows / torches, plus cannons if available. Listen for
  matchlock or bow shots that sound like cannons.
- The log lists every shot whose sound the fix changed (first 40):
  `projectile_sound: gun_type .. shot_type .. unit .. - sound A instead of the game's B`. No lines means
  the game's choice was already right for everything that fired.

**Fear (`fear_stacking`)**
- A general with fear traits (Boshin "Bloodthirsty", items like the menpo masks) near enemy units: the
  enemy's morale panel should show a fear penalty equal to the general's total fear points instead of a
  flat 4. A general with +1 from a trait and -1 from "Dreaded Force" now shows none, as before.

**Naval defender AI (diagnostics only, no fix yet)**
- With `battle_rules_debug_log=1`, attack an AI fleet in a naval battle (you attacking, the AI defending)
  and wait a minute before engaging. Then send the log: the `battle_ai: state ...` lines show which plan
  the defending ships were given.

## 11. European voices for Portugal's units (new in 0.8.9)

Needs Expanded Japan with the Portugal add-on, **DLL edition** (v20.9). Set `fots_voice_debug_log=1` in `[voices]`.

- Start-up: the log should say `unit_voices: ON - 28 unit keys ...`, then one
  `record: voice nationality 3 -> 0 (British) for Port_...` line per listed unit when the database loads.
- Land battle with Portugal (campaign or custom): the log should show
  `battle land unit uses the European voice set: Port_...` for each listed unit in the battle.
  Select Pikemen or Musketeers and give move / attack orders: they should speak Portuguese (English
  if the Portuguese voice files are not installed). Models and unit cards should look normal.
- Cavalry, generals and artillery should be unchanged (Japanese voices, no log lines).
- Naval battle with a Caravel or Galleon: `battle ship uses the European voice set: Port_...` and
  Portuguese orders.
- The battle's other sounds and music should stay as in any Sengoku battle.
- With `fots_voice_units=` (empty) everything is Japanese again.
- Worth noting if it happens: a listed unit that is silent on selection or on a specific order.

## 12. With s2fix installed (new in 0.8.9)

Install s2fix's original-game package beside this DLL and play in DirectX 11 mode.
- `tws2fix.log`: `s2fix_compat: s2fix found in the game folder ...` and
  `SetWindowLongW, SetWindowPos and MoveWindow are left to s2fix`.
- s2fix's log (`perf-logs\..._s2prof.log`): its borderless line should say `borderless: on`, not
  `the import slot does not hold ...`.
- The game runs in s2fix's borderless window. Check the UI size is right (`ui_scale: ...` lines in
  our log), the drag-selection box lines up with the cursor, and Alt group-move works.
- Also try a game resolution below the monitor's (s2fix stretches the picture and rescales the mouse):
  the drag box and Alt group-move should still follow the cursor.
- Alt-tab out and back: the cursor should be released and clipped again.

## What to send back

- `tws2fix.log` after the tests.
- For any crash: the Windows "Shogun 2 has stopped working" details if shown, and whether it happened
  before the main menu, at the menu, at campaign load, or in play.
