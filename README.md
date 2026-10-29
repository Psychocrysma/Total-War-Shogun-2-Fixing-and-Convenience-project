# TWS2-Fixing-Project — engine fix library for Total War: SHOGUN 2

A small DLL that loads beside the original (32-bit) Total War: SHOGUN 2 and fixes engine behaviour at run
time. It contains no Creative Assembly code and modifies no game file: everything happens in memory after
the game starts, and removing one file removes the library.

Target: the Steam build of 24 May 2023 (`empire.retail.dll` linked 2023-05-12, PE timestamp `0x645E92BB`).
Patches that work through Windows import tables apply to any build; patches that use engine addresses are
only applied on the verified build unless you override that in `tws2fix.ini`.

## Install

The library is still delivered as `dinput8.dll` — that file name is how the game loads it and cannot change. Its own files are `tws2fix.ini` and `tws2fix.log`. (Earlier test builds were called s2fix; if you have an `s2fix.ini` in the game folder, rename it.)

1. Copy `dinput8.dll` and `tws2fix.ini` into the game folder, next to `shogun2.exe`
   (for example `C:\Program Files (x86)\Steam\steamapps\common\Total War SHOGUN 2`).
2. Start the game normally. A file `tws2fix.log` appears in the game folder.
3. To uninstall, delete `dinput8.dll`.

If you already use another `dinput8.dll` in that folder (Rebrond's borderless mod, a ReShade or DXVK
component), only one can be named `dinput8.dll`; remove the other first.

Linux (Proton): add `WINEDLLOVERRIDES="dinput8=n,b" %command%` to the game's Steam launch options, or
Proton will use its own DirectInput and ignore the library. Use the Windows build of the game through Proton;
Feral's native Linux port is a different executable and cannot load it.

## How it loads

The engine imports one function from `dinput8.dll`, so Windows loads this file from the game folder when
the engine loads. The library forwards every DirectInput call to the real `dinput8.dll` in `System32`, and
installs its own hooks when the launcher (`shogun2.exe`) asks the engine for its entry point. That moment is
after the engine is fully loaded and before it runs any code, so the hooks are in place for the first
window the game creates.

## Patches in this build (0.8.0)

| Key in `tws2fix.ini` | Default | What it does |
| --- | --- | --- |
| `cursor_clip` | on | Keeps the mouse inside the game window while the game is in the foreground, so edge-scrolling no longer throws the cursor onto a second monitor. Released on alt-tab or when a dialog is up. |
| `borderless` | off | Turns the game's windowed mode into a borderless window covering the monitor. Set the game to Windowed and your desktop resolution in its graphics options first. Has no effect in exclusive fullscreen. |
| `agent_chance_clamp` | on | Fixes the agent-action success chance. Nine of the engine's fourteen action-chance functions (assassinate, sabotage, apprehend and others) clamp the percentage to 5..95 with an *unsigned* comparison, so a negative total (weak agent against a strong target) wraps around and comes out as 95% instead of 5%. The patch changes two opcodes per function (`jae`→`jge`, `cmova`→`cmovg`), after verifying every byte, and only on the verified build. |
| `ui_scale` | auto | `auto` gives the UI the size it has at 1920×1080 on any screen (nothing happens at 1080p or below); `off` disables it; a number is a factor of your own (1.333 at 2560×1440). It revives the engine's dormant "UI rect" variables: the UI is laid out and projected on a smaller logical screen, while the viewports and the mouse mapping keep the real screen, so the UI is rasterised at native resolution, not upscaled. The upper limit is `min(width/1280, height/1024)` because the engine floors the layout at 1280×1024. Loading screens are not scaled. The battle drag-selection box, which the engine positions in screen pixels, is mapped onto the scaled layout so it starts under the cursor. Tested at 2560×1440 under Proton. |
| `settlement_box` | 1000 | Half-size, in map units, of the box inside which the campaign display draws settlement and building models. The engine builds that quadtree with fixed bounds (−350,−10,−350)…(350,100,350); models placed outside never enter it and are never drawn, which is why large map mods lose settlements past ±350 while flags and units still show. The patch rewrites the four bounds constants before the tree is built. The campaign tree display has the same limit (its spatial tree is built for ±350 on both axes), so trees past ±350 vanish too; since 0.7.0 the same setting widens that box as well. On by default at 1000, which covers any map; the vanilla map is unaffected because everything it has is inside ±350 either way. |
| `vram_mb` | 0 (off) | Overrides the video-memory total the renderer budgets its textures from. The engine has a debug variable `force_total_video_memory` that replaces the device query when non-zero; this writes the configured figure (MB, max 4095) into it before the device is created. A 32-bit process is usually told ~2.9 GB whatever the card. |
| `log` | on | Writes `tws2fix.log`. Keep it on while testing; it records every window the engine creates and every patch applied. |

## Battle fixes (from ShakuEight's Shogun2-fix)

[ShakuEight/Shogun2-fix](https://github.com/ShakuEight/Shogun2-fix) is an Apache-2.0 project of battle-engine fixes
for the same build, also delivered as a `dinput8.dll` proxy, so the two could never be installed together. Its ten
fixes are ported into this library (`src/shaku/`, attribution in each file and in `NOTICE`) and switched by the
`[battle]` section of `tws2fix.ini`, all on by default. Every fix byte-checks its own patch sites against the
analysed build before writing anything and installs nothing on a difference; several carry run-time self-checks
that switch the fix off for the session on failure. **They change the battle simulation: single player and
replays only** — a changed simulation desyncs multiplayer. `battle_fixes=0` turns the whole section off.

| Key | Fix |
| --- | --- |
| `fix_path` | Units ordered through gates, doorways and round corners stop in a stretched block and file through one man at a time. The formation is laid past the narrow stretch so the men run through as a column and re-form beyond it. (ShakuEight's public release 1.0.) |
| `fix_artillery` | Artillery ordered to fire at a unit aims at the end of the enemy line instead of its middle. |
| `fix_fire_by_rank` | Fire by Rank: about 10 s before the first volley; Hattori and Ikko Ikki gun units stop firing after three volleys; ranks beyond the third never fire; reload waits. |
| `fix_siege` | In large sieges, once attackers climb the walls, the defender's units answer move orders late or not at all. |
| `fix_wall_manning` | A unit ordered to man a wall section marches to a staging point inside the castle first, re-forms, and only then walks onto the wall. |
| `fix_zombie_yari` | A yari wall unit that breaks mid-charge sometimes stands in its wall, counted as routing, and keeps killing. |
| `fix_wall_collapse` | Siege defence: men deployed on a gatehouse die at Start Battle when the campaign left the gates open. |
| `fix_single_unit_group` | G, Ctrl+G and Ctrl+number refuse a single unit; the fix lets one unit form a soft or locked group. |
| `fix_guard_mode` | Missile units in guard mode chase a target that left their range and keep shooting at shattered units. |
| `fix_kneel_fire` | Fall of the Samurai: units with Kneel Fire on refuse every move order. |

The port keeps each fix's logic unchanged and removes ShakuEight's development scaffolding (toggle keys, tones,
research probes, test exports). Two things changed for the compiler: the detour stubs are emitted as GCC assembly
(byte-identical to the MSVC ones, so the generated stack-offset headers still apply) and the guarded memory reads
use `ReadProcessMemory` instead of SEH. Apart from `fix_path`, ShakuEight's own builds of these fixes are
development builds that have not had a public release; treat them as such until they have been played.

## Alt group-move (Total War: WARHAMMER-style block move)

A new control, not a bug fix. With several units selected:

- hold **Alt**, press and hold the **left** button on the ground, and move the mouse: green ghosts of the
  selection follow the cursor as a block, every unit keeping its place in the formation, its frontage and
  its facing;
- while still holding the button, hold **Ctrl** and move the mouse sideways: the block stays where it is
  and turns about its own middle; release Ctrl to carry on moving it;
- release the button to issue the order.

Keys in `[battle]`: `alt_group_move` (on), `alt_rotate_speed` (64; higher turns faster), `alt_debug_log`
(off; per-hold traces for testing). Single player only, like the battle fixes.

How it works. In Shogun 2 the left button only selects (a left drag is the selection box) and move orders
are on the right button. `src/patch_altinput.cpp` shows an Alt + left hold to the engine as a right-button
hold (DirectInput mouse, window messages, the engine's key-state imports) and hides Alt from it meanwhile,
so the engine runs its own formation drag with ghosts. `src/patch_altmove.cpp` latches the selection's
layout when the hold begins (position, heading and width of each unit, read through the engine's own unit
handle) and, while the engine builds the order, replaces each unit's slot point with the block position,
gives each unit its own facing in the placement, and keeps its width. The cursor's ground position is
taken from the engine's drag line (its cursor end), the block centre is frozen while Ctrl is held, and the
same replacement runs when the queued command is executed, so what the ghosts show is what is ordered.

## Things that are not engine patches

Two campaign-map limits that come up with large map mods turned out to be data, not engine code. They are
documented in `docs/minimap_and_map_limits.md` together with a small tool (`tools/theatre_bounds.py`) that
fixes one of them:

- **Minimap clicks land in the wrong place on an enlarged map.** The click → world mapping is a linear map
  from the radar image onto the *theatre bounds* stored in `campaign_maps/<map>/regions.esf`
  (vanilla: −305,−240 … 270,70). Change those bounds to the world rectangle the new radar image covers.
- **Battle minimap too dark since the May 2023 update.** The generator's colour-overlay pass darkens the
  map by 73%; `packs/@tws2_battle_minimap_fix.pack` (drop into the game's `data` folder) sets that overlay
  to 0 and the minimap is readable again. Confirmed in game.
- **255 regions on the minimap.** The region overlay is an 8-bit paletted TGA and regions are identified by
  palette index, so 255 is structural. Lifting it means replacing the region-map component's texture
  path; see the document for what that involves.

## Building

Windows: install a 32-bit mingw-w64 (for example MSYS2's `mingw-w64-i686-gcc` from a MINGW32 shell) and run `build.bat`. Visual Studio cannot build it: the battle fixes use GCC assembly.

Linux cross-build: `apt install gcc-mingw-w64-i686 g++-mingw-w64-i686` then `./build.sh`. Output is `dist/dinput8.dll`.

## Layout

- `src/dllmain.cpp` — DirectInput proxy exports, engine detection, hook installation
- `src/hooks.cpp` — import-table hook and inline detour helpers
- `src/patch_window.cpp` — borderless window and cursor clipping
- `src/patch_agent_chance.cpp` — the agent-action chance clamp fix (engine byte patch, with addresses)
- `src/patch_uiscale.cpp` — UI scaling through the engine's UI rect variables and a caller-aware detour of the rect getter
- `src/patch_campaign.cpp` — settlement and tree render boxes and video-memory override (engine bytes, with addresses)
- `src/patch_battle.cpp` — registry of the battle fixes, gated by the `[battle]` keys
- `src/shaku/` — ShakuEight's hook engine (`hook.*`), guarded memory (`gamemem.*`) and the ten fixes (`fix_*.cpp`), Apache-2.0
- `third_party/`, `NOTICE` — licences and attribution
- `docs/` — findings on the campaign-map limits that are data rather than engine code
- `tools/` — `theatre_bounds.py` (minimap click alignment for enlarged maps) and the ESF/pack readers it needs
- `src/config.cpp`, `src/log.cpp` — `tws2fix.ini` and `tws2fix.log`
- `test/` — a stub launcher and stub engine used to smoke-test the loader under Wine

## Related projects

[JakeTheRipperYT/Shogun-2-2026-ultimate-engine-bug-fix-patch](https://github.com/JakeTheRipperYT/Shogun-2-2026-ultimate-engine-bug-fix-patch)
is a binary-only installer (no source, no licence) that rebuilds modified game packs and patches the engine files on
disk; nothing of it can be reused here. Of its engine-side items, the ones not covered by ShakuEight's fixes
(night-battle reinforcements, locked-formation overlap, projectile sound selection, tooltip wrapping) are candidates
for this project's own reverse-engineering; its balance changes (morale numbers, rosters, AI difficulty bonuses)
are out of scope.

[DudeCmonMan/s2fix](https://github.com/DudeCmonMan/s2fix) is a separate, GPL-3 project of **performance** fixes
for the same engine build (multithreaded draw submission, flip-model swap chain, VRAM reporting). It loads as a
`d3d11.dll` proxy, so the two can be installed together. Both implement borderless window and cursor clipping:
enable that in one of them only.

## Credits

- **Claude** (Anthropic's AI model) did nearly all of the work: reverse-engineering the engine, writing the
  code, the tools and the documentation, and porting ShakuEight's battle fixes.
- **Psychocrysma** started and directed the project, chose what to fix, designed the Alt group-move, and
  tested every build in game (logs, screenshots and videos that drove each fix).
- **ShakuEight**: the ten battle fixes in `src/shaku/` come from
  [ShakuEight/Shogun2-fix](https://github.com/ShakuEight/Shogun2-fix) (Apache 2.0, see `NOTICE`).

## Licence

MIT. See `LICENSE`.
