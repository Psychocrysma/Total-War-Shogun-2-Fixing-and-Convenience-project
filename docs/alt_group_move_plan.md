# Alt group-move (WH3-style): design notes

Goal: with several units selected, holding Alt and left-click-holding on the ground moves the selection
as a block — every unit keeps its offset from the others; dragging sets the facing and the block rotates
with it — instead of being re-laid on a line.

## What the engine does today (May 2023 build, Ghidra names at base 0x10000000)

Move orders go through two placement builders, called both for the ground preview and in the command
executor: `FUN_10400870` (click) and `FUN_10401c50` (drag; also via `FUN_1048c7d0`).

Plain multi-selection (`param_5[1] == 0`): the builder filters the units that can move
(`FUN_1015c3f0`), makes a selection-formation object (`FUN_1043e200` → `FUN_1040bec0`, 0x44 bytes,
vtable `0x115e4a6c`), and calls its **line layout** vfunc +0x28 (`0x1041d690`; args: unit list, ground
point, orientation (u16), flags) which writes each unit's target into the per-unit placement handle
(unit+0xC); then vfunc +0x44 (`0x1042da90`) builds the `BCQ_MULTIPLE_SELECTION_ORDER_MOVE*` command from
those handles and `FUN_10419b40` submits it.

Locked group (the other branch, `param_4` = the group's unit list): a local layout object of class
`0x115e4ac4` (ctor `FUN_1040be70(1)`) whose +0x28 thunk (`FUN_1041cdd0`) forwards to +0x2c
(`FUN_1041cdf0`, 2198 bytes) — the **group layout** that preserves relative positions — and
`FUN_1042d670` builds the group command. Moving a locked group is exactly the behaviour wanted.

## Two ways to implement

A. **Layout substitution (engine level).** Detour the two builders; when Alt is held and the selection
   is a plain multi-selection of >1 unit, run the line-layout call site with a replacement layout: for
   each unit target = point + R(θ)·(pos − centroid), θ = drag orientation − current mean facing (0 for a
   plain click), facing = unit facing + θ; write the targets into the placement handles exactly as
   `0x1041d690` does, then let the normal +0x44 command build run. Needs the placement-handle layout
   (what `0x1041d690` → `FUN_1041d810` / `FUN_10432df0` write) decoded. Clean, no UI side effects,
   same code path for preview and executor. Estimated: 2–3 sessions of reverse-engineering plus the
   usual in-game testing.

B. **Temporary locked group (UI level).** On left-button-down with Alt and >1 units selected: make a
   group of the selection through the UI's own path (`FUN_10f89a40(manager, slot −1)` — accepts one unit
   now thanks to the single-unit-group fix) and lock it (the Ctrl+G path: `BattleButtonLockFormation`
   click `0x11211d20` → `FUN_1121ff80`); the order is issued on button-up several frames later, by which
   time the engine-side group exists and the click/drag is a locked-group move. After button-up (or on
   cancel) dissolve the group (`FUN_10f97e80`) and refresh the cards. Uses only existing game behaviour.
   Risks: card/UI flicker from the group appearing and vanishing; the lock state is set through the
   command queue, so the timing between button-down and button-up must cover it (poll the queue or wait
   two ticks); a click without a drag must not change facing (the locked-group click move keeps it).
   Estimated: 1 session for the UI plumbing + the input hook (we are the DirectInput proxy, so the mouse
   button and Alt state can be read from the device state we forward).

Recommendation: try B first (fast, reuses tested game behaviour); fall back to A if the group
create/lock round trip cannot be made invisible.

## Open questions for the user

- Should Alt+click (no drag) keep the current facing (WH3: yes), and Alt+drag rotate the block so its
  front faces the drag direction? Assumed yes to both.
- Should Alt-move also apply to an already locked group? (It already behaves that way; nothing to do.)

## Decision (2026-10-28): route A, WH3 semantics confirmed from the user's video

Alt + hold LMB: the selection's current layout follows the cursor as a block, no rotation. While still
holding LMB, Ctrl + mouse movement rotates the block about its centre. Releasing LMB issues the order.

## Insertion point found: the per-unit order record

Both builders end in the same place: the selection formation's +0x44 (`FUN_1042da90`) walks the formation
slots and, per unit, computes the slot's world point (`FUN_1047d120`), then `FUN_1040bd90(out, unitHandle,
location, orientation u16, width)` fills a 0x20-byte order record `{unit, loc[5] = x,y,z,region,aux,
orientation, width}` and `FUN_10471950(record)` submits it (the single-unit click path uses the same two
calls). The preview and the executor both go through it. So the block move needs no formation code at all:

1. Detour the two builders at entry (`FUN_10400870` click, `FUN_10401c50` drag) to latch the context for
   the duration of the call: the ground point (`FUN_10399f80` on the point object; in drag mode the cursor
   end = start + width·dir(orientation), with dir from the sin/cos table `0x11bd9560/4[orientation]`), the
   unit list, and `param_4[9]`-style context needed to locate a point.
2. Detour `FUN_1040bd90`: while a builder call is active and Alt mode is on, replace `location` and
   `orientation` for each record: target = cursor + R(θ)·(offset_i), facing_i = facing0_i + θ, where
   offset_i and facing0_i are latched at Alt+LMB-down (unit centre = mean of the soldiers at unit+0x18 /
   +0x1C, soldier x/z at +0x524 / +0x52C; unit heading u16 at [unit+0x440]+0x28, width at +0x24). A
   located point is made the way `FUN_1042da90` does it: `FUN_10125500(&xz, 5.0f, ctx)` then
   `FUN_1037b430(out20, that, 1)`. Width stays the unit's own.
3. Input: Alt / Ctrl / LMB through `GetAsyncKeyState`; θ accumulates from the horizontal mouse delta
   (`GetCursorPos`) while Ctrl is held during the hold; everything resets on LMB-up after the executor's
   call. Latching the offsets at press time keeps the preview and the executor identical even while the
   units drift.
4. INI key `alt_group_move=1` in `[battle]`; single player only (the executor runs the same detour, so a
   replay or an opponent without it desyncs).

Still to decode before coding: which argument of the builders is the located-point context (the
`param_4[9]` passed to `FUN_10125500`), and the exact orientation convention of the u16 (which index is
"north", direction of increase) — one disassembly of `FUN_10432df0` against the sin/cos table settles it.
