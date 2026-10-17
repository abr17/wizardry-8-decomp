/* msvc2022 port: the C heap and the floating-point helpers that retail
   Wiz8.exe imported from MSVCRT.dll, routed back to MSVCRT.dll.

   Heap. Retail Wiz8.exe (VC6 /MD) imported malloc, free, calloc, realloc,
   _wcsdup, ??2 and ??3 from MSVCRT.dll, so operator new and malloc shared ONE
   heap (msvcrt's private HeapCreate heap), and so did SR.dll, which imports
   the same names. The modern image links the static UCRT (/MT), whose heap is
   GetProcessHeap(). Without this file, malloc/free would stay on the UCRT heap
   while new/delete (msvcrt_heap_bridge.cpp) go to MSVCRT.dll: any new + free,
   malloc + delete or buffer exchanged with SR.dll that retail tolerated would
   free into the wrong heap. Defining the public names here makes them win over
   the libucrt members (an object file always beats a library member; UCRT's
   own internals call _malloc_base/_free_base, so the CRT keeps its private
   allocations to itself and never mixes them with ours).

   Every target is a load-time import from msvcrt.dll (an IAT slot built by
   tools/msvc2022/make_import_aliases.py); there is no run-time lookup and no
   silent fallback to another heap. If msvcrt.dll were missing, the loader
   would refuse to start the exe with a clear "entry point not found" error.

   Floating point. VC6 /O2 expanded sin, cos, tan, atan2 and sqrt inline as
   fsin, fcos, fptan + fstp st(0), fpatan and fsqrt (retail Wiz8.exe has 113 fsin,
   99 fcos, 2 fptan, 11 fpatan, 308 fsqrt and imports none of these functions),
   and called MSVCRT.dll's _CIpow, _CIacos and _CIasin. MSVC 2022 instead calls
   __CIsin and friends from the static UCRT, which pick SSE2 implementations at
   run time and round differently. The _CI helpers take their arguments on the
   x87 stack (ST(1) = first argument, ST(0) = second) and return in ST(0), so
   the bodies below are exactly the instructions VC6 put inline, and the three
   functions retail imported jump straight into MSVCRT.dll. atof and atoi are
   routed too: retail imported them, and they parse game data (Monster.cpp,
   AutomapScreen.cpp). */

#include <stddef.h>

/* MSVCRT.dll imports under private names (see make_import_aliases.py). */
__declspec(dllimport) void* __cdecl w8crt_malloc(size_t size);
__declspec(dllimport) void __cdecl w8crt_free(void* block);
__declspec(dllimport) void* __cdecl w8crt_calloc(size_t count, size_t size);
__declspec(dllimport) void* __cdecl w8crt_realloc(void* block, size_t size);
__declspec(dllimport) size_t __cdecl w8crt_msize(void* block);
__declspec(dllimport) char* __cdecl w8crt_strdup(const char* text);
__declspec(dllimport) unsigned short* __cdecl w8crt_wcsdup(const unsigned short* text);
__declspec(dllimport) double __cdecl w8crt_atof(const char* text);
__declspec(dllimport) int __cdecl w8crt_atoi(const char* text);
__declspec(dllimport) long __cdecl w8crt_atol(const char* text);

/* IAT slots of the x87-convention helpers, used from inline assembly. */
extern void* _imp__w8crt_CIpow;
extern void* _imp__w8crt_CIacos;
extern void* _imp__w8crt_CIasin;

void* __cdecl malloc(size_t size)
{
    return w8crt_malloc(size);
}

void __cdecl free(void* block)
{
    w8crt_free(block);
}

void* __cdecl calloc(size_t count, size_t size)
{
    return w8crt_calloc(count, size);
}

void* __cdecl realloc(void* block, size_t size)
{
    return w8crt_realloc(block, size);
}

/* libucrt's msize.obj also defines _msize_base, which other UCRT members pull
   in, so _msize itself cannot be overridden. compat/msvc2022.h renames the
   recovered code's _msize calls (MemMan.cpp) to this function instead. */
size_t __cdecl w8_msvcrt_msize(void* block)
{
    return w8crt_msize(block);
}

char* __cdecl _strdup(const char* text)
{
    return w8crt_strdup(text);
}

unsigned short* __cdecl _wcsdup(const unsigned short* text)
{
    return w8crt_wcsdup(text);
}

double __cdecl atof(const char* text)
{
    return w8crt_atof(text);
}

/* Retail imported atoi too. MSVCRT wraps on overflow where the UCRT clamps to
   INT_MAX/INT_MIN, which changes values parsed from game data (found by the
   w8diff harness in ParseMonsterCycleName). atox.obj cannot be overridden, so
   compat/msvc2022.h renames the recovered calls to these wrappers. */
int __cdecl w8_msvcrt_atoi(const char* text)
{
    return w8crt_atoi(text);
}

long __cdecl w8_msvcrt_atol(const char* text)
{
    return w8crt_atol(text);
}

/* Retail imported these three from MSVCRT.dll: same code, same results. */
__declspec(naked) void __cdecl _CIpow(void)
{
    __asm jmp dword ptr [_imp__w8crt_CIpow]
}

__declspec(naked) void __cdecl _CIacos(void)
{
    __asm jmp dword ptr [_imp__w8crt_CIacos]
}

__declspec(naked) void __cdecl _CIasin(void)
{
    __asm jmp dword ptr [_imp__w8crt_CIasin]
}

/* VC6 expanded these inline; same instructions, same results. */
__declspec(naked) void __cdecl _CIsin(void)
{
    __asm {
        fsin
        ret
    }
}

__declspec(naked) void __cdecl _CIcos(void)
{
    __asm {
        fcos
        ret
    }
}

__declspec(naked) void __cdecl _CItan(void)
{
    __asm {
        fptan
        fstp st(0)
        ret
    }
}

/* atan2(y, x): ST(1) = y, ST(0) = x; fpatan leaves atan2(y, x) in ST(0). */
__declspec(naked) void __cdecl _CIatan2(void)
{
    __asm {
        fpatan
        ret
    }
}

__declspec(naked) void __cdecl _CIsqrt(void)
{
    __asm {
        fsqrt
        ret
    }
}
