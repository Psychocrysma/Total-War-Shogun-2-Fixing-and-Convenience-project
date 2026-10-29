# Porting a ShakuEight battle fix into TWS2-Fixing-Project

Source project: ShakuEight/Shogun2-fix (Apache License 2.0), https://github.com/ShakuEight/Shogun2-fix,
cloned locally (call the clone `$SP`).
Target: `src/shaku/fix_<short>.cpp`. Template: `src/shaku/fix_kneel.cpp`
(ported from `$SP/fixes/kneel-fire-move-fix/src/kneelfix.cpp`; diff the two to see exactly what a port
removes and changes).

Shared infrastructure already ported in `src/shaku/`: `hook.h` / `hook.cpp` (HookDef tables, trampolines,
`HOOK_DETOUR` and `HOOK_DETOUR_RET` as GCC top-level assembly, byte-identical stubs), `gamemem.h` /
`gamemem.cpp` (guarded reads/writes, now through ReadProcessMemory instead of SEH), `log.h` (maps `Log()`
to this project's logger, same `Log(const char* fmt, ...)` signature). Do not include `keys.h`.

## Rules

1. **Behaviour of the fix itself is unchanged.** The port is the dev source with the fix permanently ON
   (the state the dev build has with no environment variables set), and nothing else. Keep every
   self-check, every byte check, every counter the self-checks or the Remove log line use. Do not
   "improve" game logic, constants, or offsets. If the dev source has several variants/modes, keep the
   default one and remove the others only if that is clearly what the release would do (pathfix's
   release source already did this; for the others keep the default mode and say so in the report).
2. **Remove the development-only parts**: toggle keys (`Keys_*`), tones (`Tone_*`), `MessageBeep`,
   environment-variable switches (`EnvIs`, `*_OFF`, `*_ON`, `*_PROBE`, `*_START_OFF`), research probes /
   capture runs / CSV writers / run folders, `__declspec(dllexport)` harness exports (`*TestKeys`,
   `*TestCounters`, and anything else exported), crash filters / dump writers, and the critical sections
   that only served the probe. Where a function only had a probe call in it, drop the call. Where a
   log line reported the dev state ("dev build", "I toggles"), reword to the ON state.
3. **MSVC → GCC (mingw-w64 i686)**:
   - `__declspec(naked)` + `__asm` blocks → a GCC top-level `__asm__(...)` block in Intel syntax with an
     explicit asm label, as in `fix_kneel.cpp` (`extern "C" T __fastcall F(...) asm("tws2_<short>_f"); __asm__(".text\n.intel_syntax noprefix\n tws2_<short>_f:\n ... \n.att_syntax prefix\n");`).
     Globals referenced from asm get an asm label too (`extern "C" uint32_t g asm("tws2_<short>_g"); uint32_t g = 0;`).
     Keep the instruction sequence identical to the MSVC version.
   - `HOOK_DETOUR(name, handler, trampArray, index)` works as before, with two requirements: `handler` is
     `extern "C" ... __stdcall handler(const uint8_t* ctx)` and `trampArray` is a file-scope array
     (static is fine). **Prefix every `extern "C"` symbol with the fix's short name** (e.g.
     `art_RelHandler0`, `art_RelDetour0`) so the ten fixes link into one DLL without clashes. A macro that
     generates `RelHandler##i` can simply be renamed to generate `art_RelHandler##i`.
   - `__try { ... } __except (...) { ... }` → the same reads through `MemcpyTry` / `ReadDword` /
     `ReadByte` / `CopyGuarded` from `gamemem.h`, returning what the `__except` branch returned on a failed
     read. Keep the order and the range checks.
   - `__fastcall`, `__stdcall`, `__thiscall`, `__cdecl` are available in mingw; `_snprintf` → `snprintf`,
     `_countof(a)` → `(int)(sizeof(a)/sizeof((a)[0]))`, `_InterlockedXxx` → `InterlockedXxx`,
     `__forceinline` → `inline`. `<windows.h>` is fine; `#define WIN32_LEAN_AND_MEAN` before it if not set.
   - Unused-variable / unused-function warnings left over from the removed dev code: remove the dead code.
4. **Stack-offset headers**: copy the fix's `stack_offsets_*.h` from `$SP/fixes/<name>/src/` into
   `src/shaku/` unchanged (they are generated for the detour stub sequence, which the GCC macro reproduces
   byte for byte). If a header name would collide with another fix's, prefix it with the short name and
   change the include.
5. **Entry points** keep their exact names: `Pathfix_Install/Remove`, `ArtFix_`, `FbrFix_`, `SiegeFix_`,
   `WallFix_`, `ZombieFix_`, `CollapseFix_`, `GroupFix_`, `GuardFix_`, `KneelFix_` (as in
   `$SP/proxy/fixes.cpp`). `src/patch_battle.cpp` calls them.
6. **Header comment**: same form as `fix_kneel.cpp`: file name, one-line purpose, the attribution block
   ("From ShakuEight/Shogun2-fix (path), Copyright 2026 ShakuEight, Apache License 2.0
   (third_party/LICENSE-APACHE-2.0). Ported to this project: ..."), then ShakuEight's own description of
   the fault, the engine decode and the fix, condensed but with every address, offset and constant kept.
7. **Log prefix**: `<short>fix:` as in the source (`pathfix:`, `artfix:`, `fbrfix:`, `siegefix:`,
   `wallfix:`, `zombiefix:`, `collapsefix:`, `groupfix:`, `guardfix:`). The Install log should end with one
   line saying the fix is ON and what it does; Remove logs the counters as the source did.

## Compile and check

```
cd <your clone of this repository>
i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -fno-exceptions -fno-rtti \
    -Isrc/shaku -c src/shaku/fix_<short>.cpp -o build/shaku_fix_<short>.o
tools/dev/check_fix.sh fix_<short> <NamePrefix>      # e.g. tools/dev/check_fix.sh fix_art ArtFix
```

The check links a test DLL with only this fix, loads it beside the real engine image under Wine and
prints the fix's log lines. Success = the fix logs that it installed (its byte checks passed on the real
build) and no `REFUSED`. A `REFUSED ... differ from the analysed binary` means a byte table was damaged
in the port (the source's tables are correct for this build); compare with the source.

## Report back

In the final report: the target file; what was removed (one line each); every MSVC→GCC change beyond the
mechanical ones; the exact check-script output lines for the fix; anything in the source that the port
could not keep faithfully, with the reason. Do not touch any file outside `src/shaku/` and `build/`.
