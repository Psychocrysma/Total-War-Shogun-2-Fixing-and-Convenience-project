# Campaign-map limits: minimap click alignment, the 255-region minimap, settlement box, VRAM

Findings from the engine (empire.retail.dll, May 2023 build), the vanilla UI scripts and the vanilla map
files. Two of the four items are engine patches and ship in TWS2-Fixing-Project 0.4.0; two are data.

## 1. Minimap clicks misaligned on an enlarged map — data fix

### Where the mapping lives

The campaign minimap is the UI component `campaign_radar/radar/map` (a `template.map_image`) with a
`map_overlay` child. Clicking is handled in Lua, not in C++: `ui/campaign ui/map_overlay.lua` has

```lua
function OnLClickUp(x, y)
    local posx, posy = this:Position()
    local offset_x = (x - posx) / w             -- w, h = this:Dimensions()
    local offset_y = (y - posy) / h
    local theatre = CampaignUI.HomeTheatre(CampaignUI.PlayerFactionId())
    local tx, ty, tw, th = CampaignUI.TheatreMapDimensionsBL(theatre)
    CampaignUI.SetCameraTarget(tx + tw * offset_x, ty + th * (1 - offset_y))
end
```

(the shipped file is compiled Lua; this is the listing read back from the bytecode). The camera rectangle
drawn on the minimap uses the same four numbers the other way round (`campaign_radar.lua`,
`ScaleMapPosition`, and the engine's `UpdateRadarView`).

`CampaignUI.TheatreMapDimensionsBL` (engine 0x1114de10) returns `min_x, min_y, max_x−min_x, max_y−min_y`
of the **theatre** record. Theatres are loaded from `campaign_maps/<map>/regions.esf`, node `theatres` →
`theatre`: a key (the `campaign_map_playable_areas` index, `1263229068` = `japan_main`), then two `vec2`
nodes — the minimum corner and the maximum corner, in campaign world units — then a bool. A second copy
sits under `theatres_and_region_keys` (used for region → theatre lookup).

Vanilla `sho_japan`: min (−305, −240), max (270, 70): 575 × 310 units. The radar image `japan_map.tga` /
`japan_lookup.tga` (1024 × 512) is stretched onto exactly that rectangle: left edge = x −305, right edge
= x 270, bottom = y −240, top = y 70. Nothing in the engine derives these from the heightmap or the image;
they are authored numbers. (The world itself is ±640 × ±320; `set_map_bounds` from Lua sets *camera
scroll limits*, not the minimap mapping; `playable_area/min_x/max_x` in the engine are battle-map
strings. None of those matter here.)

### Why an enlarged map breaks

If the radar images are repainted to cover a larger area (Hokkaido to the north, say) but the theatre
bounds stay at the vanilla values, every click is interpreted as if the image still spanned −305…270 ×
−240…70, so the click lands proportionally wrong: click Kyoto, land somewhere else. It is a pure
coordinate mismatch, not a bug in the engine.

A second effect of stale bounds: `HomeTheatre`, `CameraTarget` and several other calls find the theatre by
"which theatre rectangle contains this point" (engine 0x108eefc0). With the camera over land outside the
rectangle they return nothing, so the camera rectangle on the minimap stops updating there.

### The fix

1. Decide the world rectangle the **new** radar image covers (x_min, y_min, x_max, y_max). To keep the
   vanilla pixel scale, extend the image by the same units-per-pixel as vanilla: 575/1024 = 0.5615 units
   per pixel horizontally and 310/512 = 0.6055 vertically (the vanilla image is slightly anisotropic, the
   component stretches it; keep whatever ratio you like, only the rectangle has to be right).
2. Set the bounds in the mod's `regions.esf` with the tool in this repository:

   ```
   python tools/theatre_bounds.py regions.esf                       # show current bounds
   python tools/theatre_bounds.py regions.esf -o regions_new.esf  X_MIN Y_MIN X_MAX Y_MAX
   ```

   It rewrites only the eight floats (both copies), so the file size and everything else are unchanged.
   It reads CA's original ABCF files and the ABCA form that ESF editors / RPFM save. Then repack.
3. Both image files must cover the same rectangle: `japan_map.tga` (what you see) and `japan_lookup.tga`
   (the region overlay; see §2). The lookup image must stay 8-bit paletted with the region colours from
   `regions_tables` (r, g, b) in its palette.
4. If the new rectangle has a different aspect ratio, the `map` / `map_overlay` component sizes in the
   `campaign_radar` layout decide how it is displayed; the Lua `ZoomToFit`/`SetInitialZoom` scale from
   `map:Dimensions()`, so a differently shaped image just shows stretched unless the layout is also edited.

The current Expanded Japan `regions.esf` still has the vanilla bounds and the current images are still
1024 × 512, so nothing needs changing until the enlarged radar images go in.

## 2. 255 regions on the minimap — structural, needs a component rewrite

The region overlay `japan_lookup.tga` is an 8-bit **paletted** TGA (256 palette entries). The engine
identifies a region on the minimap by its palette index: `InitRegionMap` (engine 0x110667d0) walks the
regions, matches each region's `r,g,b` from `regions_tables` to a palette entry and stores that index as
`PaletteEntry` in the Lua region table; `FindRegionIndex` returns the palette index under the mouse; and
recolouring for ownership / diplomacy / public-order modes is done by rewriting palette entries
(`SetPaletteEntry` → 0x10f78840 → 0x1136ea90, which checks `index < palette_count` and writes the colour).
Index 0 is the background, so 255 regions is the ceiling, and it is a property of the format, not a
constant that can be patched.

Lifting it would mean replacing that component's texture path: a 16-bit (or RGB) index image, a palette
of more than 256 entries (a 1-D lookup texture instead of the hardware palette), the pixel shader that
samples the overlay doing the lookup, and the three handlers above rewritten against the new texture.
That is a self-contained project (a few thousand lines of hook code plus a shader) rather than a patch,
and it should wait until a map actually needs more than 255 regions. Expanded Japan is under the limit.

## 3. Settlement models not drawn beyond ±350 — engine patch, shipped (0.4.0)

The campaign display puts settlement and building models in a quadtree built with fixed bounds
(−350, −10, −350) … (350, 100, 350) (engine 0x1082d130 → 0x108b7910; the vector in `.rdata` at RVA
0x0162A100 and one immediate at RVA 0x0082D537). Models outside the box never enter the tree and are never
drawn. `settlement_box=<half-size>` in `tws2fix.ini` rewrites the bounds before the tree is built. Flags,
units and the terrain use other containers and were never affected, which is why only the models vanish.

## 4. Video memory misdetected (2.9 GB on an 8 GB card) — engine patch, shipped (0.4.0)

The renderer gets a video-memory total from the device, except when the debug variable
`force_total_video_memory` (object at RVA 0x01C9BEF0, value at +0x5C, bytes) is non-zero, in which case
that figure is used instead (0x113df9bc). Debug variables cannot be set from `preferences.script.txt`, so
`vram_mb=<MB>` writes it before the device is created. 4095 is the maximum (32-bit field). In DirectX 9
mode a lower figure (3072) is safer because the D3D9 runtime keeps system-memory copies of managed
textures inside the same 4 GB address space.

DudeCmonMan/s2fix (a separate GPL-3 project) also addresses VRAM reporting from the D3D11 side; the two
approaches can coexist, but set only one.

## 5. Battle minimap dark since the May 2023 update (B6) — data fix, shipped as a pack

The battle minimap is generated when a battle loads (`RADAR_GENERATOR`, engine 0x10c24170): a height
overlay (low_h → high_h colours by elevation), ground-type colours, trees and buildings, then a **colour
overlay** pass that blends `colour_overlay_colour` from `battleterrain/radar/<season>_radar_settings.xml`
over the whole image (0x10c1ae00: multiply, strength = the colour's alpha), and finally the area outside
the playable bounds is halved. The shipped setting is black at alpha 185 (73%), which is what darkens the
map and crushes the elevation bands. Confirmed in game on 2026-10-28: with the overlay alpha at 0 the
minimap is bright with the contour bands clearly visible; at 90 it is halfway; at the stock 185 it is the
dark map everyone reports (mean brightness of the minimap area 130 / 108 / 56).

Fix: `packs/@tws2_battle_minimap_fix.pack`, a "movie"-type pack (loads by itself from the game's `data`
folder, no mod manager) holding the three `*_radar_settings.xml` with `colour_overlay_colour alpha="0"`
and nothing else changed. Source in `packs/battle_minimap_fix_src/`, rebuilt with
`python tools/pfhwrite.py out.pack movie packs/battle_minimap_fix_src`. To keep a little shading, set the
alpha to 40–60 instead of 0 and rebuild. Whether the 2023 build changed the blend or the overlay texture
cannot be told without the earlier build; the data override gives the readable map either way.
