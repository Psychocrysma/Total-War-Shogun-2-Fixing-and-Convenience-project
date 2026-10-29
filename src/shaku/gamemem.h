// gamemem.h -- guarded access to game memory, shared by every battle fix.
//
// From ShakuEight/Shogun2-fix (proxy/gamemem.h), Copyright 2026 ShakuEight, Apache License 2.0
// (third_party/LICENSE-APACHE-2.0). Interface unchanged; see gamemem.cpp for the GCC port.
//
// Every read or write a hook handler makes in game memory goes through one of these: each is
// a guarded copy, so a stale pointer costs a failed read instead of a crash. The range
// checks use the highest user-mode address (Shogun2.exe is large-address-aware, so on 64-bit
// Windows the heap reaches 0xFFFEFFFF); call Mem_Init() once at install.
//
// No logging here: the public DLL compiles this file too.
#pragma once
#include <windows.h>
#include <cstdint>

void     Mem_Init();                // reads the user-mode ceiling; safe to call more than once
uint32_t Mem_UserMax();

// Pointer tests: in user space above the first 64 KB (and 4-byte aligned for PtrPlausible).
bool PtrPlausible(uint32_t p);
bool PtrInUser(uint32_t p);

// Range-checked copy (fails outside 0x10000..user max), SEH-guarded.
bool MemcpyTry(void* out, uint32_t addr, uint32_t n);
// SEH-guarded dword / byte read without a range check; *out = 0 on a fault.
bool ReadDword(uint32_t addr, uint32_t* out);
bool ReadByte(uint32_t addr, uint8_t* out);
// Every page of [addr, addr + n) committed, readable and not a guard page.
bool RangeReadable(uint32_t addr, uint32_t n);
// RangeReadable, then a guarded copy; returns n or 0.
uint32_t CopyGuarded(uint32_t addr, uint8_t* out, uint32_t n);

// Range-checked, SEH-guarded writes.
bool WriteByteTry(uint32_t addr, uint8_t v);
bool WriteFloatTry(uint32_t addr, float v);
bool WriteDwordTry(uint32_t addr, uint32_t v);

// Battle tick (0.1 game s per tick) through the pointer path the autorun tool reads
// (tools/autorun/src/game_offsets.h): [base+0x017C755C] -> +0x50 -> +0x00 -> +0x1C -> +0x50.
// 0 outside a battle or when any read fails. base = empire.retail.dll's load address.
uint32_t GameTickAt(uint32_t base);
