#!/bin/bash
# Cross-build TWS2-Fixing-Project (dinput8.dll) for 32-bit Windows with mingw-w64.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build dist
CXX=i686-w64-mingw32-g++
FLAGS="-std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -fno-exceptions -fno-rtti -Isrc/shaku"
LDFLAGS="-shared -static -static-libgcc -static-libstdc++ -Wl,--kill-at -Wl,--enable-stdcall-fixup"
$CXX $FLAGS -c src/dllmain.cpp -o build/dllmain.o
$CXX $FLAGS -c src/log.cpp -o build/log.o
$CXX $FLAGS -c src/config.cpp -o build/config.o
$CXX $FLAGS -c src/hooks.cpp -o build/hooks.o
$CXX $FLAGS -c src/patch_window.cpp -o build/patch_window.o
$CXX $FLAGS -c src/patch_agent_chance.cpp -o build/patch_agent_chance.o
$CXX $FLAGS -c src/patch_uiscale.cpp -o build/patch_uiscale.o
$CXX $FLAGS -c src/patch_campaign.cpp -o build/patch_campaign.o
$CXX $FLAGS -c src/patch_battle.cpp -o build/patch_battle.o
$CXX $FLAGS -c src/patch_altmove.cpp -o build/patch_altmove.o
$CXX $FLAGS -c src/patch_altinput.cpp -o build/patch_altinput.o
$CXX $FLAGS -c src/patch_uidiag.cpp -o build/patch_uidiag.o
$CXX $FLAGS -c src/patch_movecell.cpp -o build/patch_movecell.o
for f in src/shaku/*.cpp; do
    $CXX $FLAGS -c "$f" -o "build/shaku_$(basename "${f%.cpp}").o"
done
CORE="build/dllmain.o build/log.o build/config.o build/hooks.o build/patch_window.o build/patch_agent_chance.o build/patch_uiscale.o build/patch_campaign.o build/patch_battle.o build/patch_altmove.o build/patch_altinput.o build/patch_uidiag.o build/patch_movecell.o"
$CXX $LDFLAGS -o dist/dinput8.dll $CORE build/shaku_*.o src/dinput8.def -luser32 -lkernel32
i686-w64-mingw32-strip dist/dinput8.dll
cp src/tws2fix.ini dist/tws2fix.ini
echo "built dist/dinput8.dll"
