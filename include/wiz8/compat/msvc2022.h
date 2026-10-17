#pragma once

/* msvc2022 port: VC6 library behavior that modern MSVC changed and that no
   compiler switch restores. Force-included right after compiler.h, only by the
   modern-MSVC build (cmake/ModernMsvc.cmake). Each item reproduces what the
   VC6 headers did, so recovered sources stay unedited. */

#if defined(_MSC_VER) && _MSC_VER >= 1900 && !defined(__clang__)

/* Retail took _msize from MSVCRT.dll, the heap malloc/free use. libucrt's
   _msize cannot be overridden (its object also defines _msize_base), so route
   the recovered calls (sgp/MemMan.cpp) to the MSVCRT.dll wrapper in
   src/msvc2022_compat/msvcrt_crt_bridge.c. Defined before <malloc.h> is read,
   so the header declares the wrapper under the same signature. */
#define _msize w8_msvcrt_msize
/* Same for atoi/atol (libucrt's atox.obj also defines _atoi_l and friends):
   retail imported atoi from MSVCRT.dll, which wraps on overflow where the UCRT
   clamps (found by the w8diff harness in ParseMonsterCycleName). */
#define atoi w8_msvcrt_atoi
#define atol w8_msvcrt_atol

#ifdef __cplusplus

/* VC6 <stdlib.h> declared only int abs(int), so abs(unsigned) converted the
   argument to int. Modern C++ adds long / long long / float overloads, which
   makes abs(unsigned) ambiguous. Restore the single VC6 conversion. */
extern "C" int __cdecl abs(int);
inline int abs(unsigned int value)
{
    return abs(static_cast<int>(value));
}
inline int abs(unsigned long value)
{
    return abs(static_cast<int>(value));
}

#endif /* __cplusplus */

#endif
