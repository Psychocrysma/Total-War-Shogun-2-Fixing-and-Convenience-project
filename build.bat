@echo off
rem Build TWS2-Fixing-Project (dinput8.dll) on Windows with mingw-w64 (32-bit), for example from an
rem MSYS2 "MINGW32" shell with the mingw-w64-i686-gcc package, or any i686-w64-mingw32-g++ on PATH.
rem The battle fixes in src\shaku use GCC assembly, so Visual Studio cannot build this project.
setlocal
if not exist build mkdir build
if not exist dist mkdir dist
set CXX=i686-w64-mingw32-g++
set FLAGS=-std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -fno-exceptions -fno-rtti -Isrc/shaku
for %%f in (src\*.cpp) do (
    %CXX% %FLAGS% -c %%f -o build\%%~nf.o || exit /b 1
)
for %%f in (src\shaku\*.cpp) do (
    %CXX% %FLAGS% -c %%f -o build\shaku_%%~nf.o || exit /b 1
)
%CXX% -shared -static -static-libgcc -static-libstdc++ -Wl,--kill-at -Wl,--enable-stdcall-fixup -o dist\dinput8.dll build\dllmain.o build\log.o build\config.o build\hooks.o build\patch_window.o build\patch_agent_chance.o build\patch_uiscale.o build\patch_campaign.o build\patch_battle.o build\patch_altmove.o build\patch_altinput.o build\patch_uidiag.o build\patch_movecell.o build\shaku_*.o src\dinput8.def -luser32 -lkernel32 || exit /b 1
i686-w64-mingw32-strip dist\dinput8.dll
copy /y src\tws2fix.ini dist\tws2fix.ini >nul
echo built dist\dinput8.dll
