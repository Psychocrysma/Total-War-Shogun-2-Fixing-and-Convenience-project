// Maps the real engine DLL without running it, triggers TWS2-Fixing-Project via GetProcAddress, then reads back the patched bytes.
#include <windows.h>
#include <cstdio>
#include <cstdint>
int main(){
    HMODULE di = LoadLibraryW(L"dinput8.dll"); printf("dinput8 proxy: %p\n", di);
    HMODULE eng = LoadLibraryExW(L"empire.retail.dll", nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if(!eng){ printf("map failed %lu\n", GetLastError()); return 1; }
    printf("engine mapped at %p\n", eng);
    FARPROC ep = GetProcAddress(eng, "entry_point"); printf("entry_point = %p\n", ep);
    Sleep(300);
    uint32_t sites[] = {0x0086EE25,0x0086EE4A,0x0086F02A,0x0086F04E,0x0086F44A,0x0086F46E,0x0086F7AA,0x0086F7CF,0x0086F8E8,0x0086F90C,0x0087001B,0x00870040,0x00870346,0x0087036B,0x008705A3,0x008705C6,0x008707BB,0x008707DE};
    int ok=0; for (uint32_t r : sites){ uint8_t b = *((uint8_t*)eng + r); printf("rva %08X = %02X\n", r, b); if (b==0x7D||b==0x4F) ok++; }
    printf("patched sites: %d of 18\n", ok);
    { uint8_t* g=(uint8_t*)eng+0x007621F0; printf("grid ctor head: %02X %02X %02X %02X %02X %02X\n",g[0],g[1],g[2],g[3],g[4],g[5]); uint8_t* b1=(uint8_t*)eng+0x007E437A; uint8_t* b2=(uint8_t*)eng+0x007E46E0; printf("budget sites: %02X %02X | %02X %02X\n",b1[0],b1[5],b2[0],b2[5]); uint8_t* c=(uint8_t*)eng+0x0077558B; printf("cell site 1: %02X %02X %02X %02X %02X\n",c[0],c[1],c[2],c[3],c[4]); }
    return ok==18?0:2;
}
