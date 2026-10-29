#!/bin/bash
# Developer check for one ported battle fix: links a test DLL from the project's objects plus this
# fix's object (every other battle fix stubbed), loads it beside the real engine image under Wine
# and prints what the fix logged. Usage: tools/dev/check_fix.sh <object basename> <Name prefix>
#   e.g. tools/dev/check_fix.sh fix_kneel KneelFix
# Needs: build/*.o of the core (run build.sh once or compile them), test/real_engine_check.exe,
# test/empire.retail.dll (a copy or symlink of the real engine), a 32-bit Wine prefix at /root/.wine32.
set -euo pipefail
cd "$(dirname "$0")/../.."
obj=$1; name=$2
CXX=i686-w64-mingw32-g++
FLAGS="-std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -fno-exceptions -fno-rtti -Isrc/shaku -Isrc"
work=$(mktemp -d /tmp/tws2check.XXXXXX)
all="Pathfix ArtFix FbrFix SiegeFix WallFix ZombieFix CollapseFix GroupFix GuardFix KneelFix"
{
  echo '#include "common.h"'
  for n in $all; do
    [ "$n" = "$name" ] && continue
    echo "void ${n}_Install() { Log(\"$n: stubbed for this check\"); } void ${n}_Remove() {}"
  done
} > "$work/stubs.cpp"
$CXX $FLAGS -c "$work/stubs.cpp" -o "$work/stubs.o"
$CXX $FLAGS -c "src/shaku/$obj.cpp" -o "$work/$obj.o"
for f in dllmain log config hooks patch_window patch_agent_chance patch_uiscale patch_campaign patch_battle patch_altmove patch_altinput patch_uidiag; do
  [ -f "build/$f.o" ] || $CXX $FLAGS -c "src/$f.cpp" -o "build/$f.o" 2>/dev/null
done
for f in hook gamemem; do
  [ -f "build/shaku_$f.o" ] || $CXX $FLAGS -c "src/shaku/$f.cpp" -o "build/shaku_$f.o"
done
$CXX -shared -static -static-libgcc -static-libstdc++ -Wl,--kill-at -Wl,--enable-stdcall-fixup -o "$work/dinput8.dll" \
  build/dllmain.o build/log.o build/config.o build/hooks.o build/patch_window.o build/patch_agent_chance.o \
  build/patch_uiscale.o build/patch_campaign.o build/patch_battle.o build/patch_altmove.o build/patch_altinput.o build/patch_uidiag.o build/shaku_hook.o build/shaku_gamemem.o \
  "$work/$obj.o" "$work/stubs.o" src/dinput8.def -luser32 -lkernel32
cp test/real_engine_check.exe test/tws2fix.ini "$work/"
ln -s "$(readlink -f test/empire.retail.dll)" "$work/empire.retail.dll"
( cd "$work" && HOME=/root WINEPREFIX=/root/.wine32 WINEDLLOVERRIDES="dinput8=n,b" WINEDEBUG=-all timeout 120 xvfb-run -a wine real_engine_check.exe >/dev/null 2>&1 || true )
echo "--- tws2fix.log lines from the fix and the loader ($work) ---"
grep -i "engine:\|$name\|${obj#fix_}fix\|REFUSED\|SELF-CHECK\|stubbed for" "$work/tws2fix.log" || cat "$work/tws2fix.log"
