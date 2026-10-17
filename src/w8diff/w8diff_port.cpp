/* Differential harness host in Wiz8Runtime.exe (mod, 2026-10-13). Dormant
   unless wiz8wide.ini has [Debug] DiffHarness=1. See w8diff_core.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w8diff_core.h"
#include "wiz8/w8diff_port.h"

/* The game code in this exe calls rand() from the same static CRT. */
static void __cdecl SeedPortRandom(unsigned int seed)
{
    srand(seed);
}

void W8DiffPortPoll(void)
{
    static int enabled = -1;
    static char dir[MAX_PATH];
    static W8DiffHost host;
    if (enabled < 0) {
        char ini[MAX_PATH];
        char* slash;
        GetModuleFileNameA(0, dir, MAX_PATH);
        slash = strrchr(dir, '\\');
        if (slash) {
            slash[1] = 0;
        }
        _snprintf(ini, sizeof(ini), "%swiz8wide.ini", dir);
        ini[sizeof(ini) - 1] = 0;
        enabled = GetPrivateProfileIntA("Debug", "DiffHarness", 0, ini) != 0;
        host.name = "port";
        host.dir = dir;
        host.seed = SeedPortRandom;
    }
    if (enabled) {
        W8DiffPoll(&host);
    }
}
