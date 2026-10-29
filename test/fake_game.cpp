#include <windows.h>
#include <cstdio>
int main(){ HMODULE m = LoadLibraryW(L"empire.retail.dll"); if(!m){ printf("load failed %lu\n", GetLastError()); return 1; }
 auto f = (int(*)())GetProcAddress(m, "RunEngine"); int r = f(); printf("fake game: done %d\n", r); return r; }
