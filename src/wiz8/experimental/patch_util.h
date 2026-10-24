/* Experimental (see include/wiz8/experimental.h): small helpers for patching the original
   machine code, shared by OriginalFixes.cpp, FixedTick.cpp and FrameRateFixes.cpp. Header
   only: no C runtime start-up, no globals. */
#ifndef WIZ8_EXPERIMENTAL_PATCH_UTIL_H
#define WIZ8_EXPERIMENTAL_PATCH_UTIL_H

#include <windows.h>
#include <string.h>

/* Writes `length` bytes over the original code at `address`. */
static inline bool PatchWrite(unsigned address, const void* bytes, unsigned length)
{
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(address), length, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    memcpy(reinterpret_cast<void*>(address), bytes, length);
    VirtualProtect(reinterpret_cast<void*>(address), length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), length);
    return true;
}

/* A 5-byte call (0xe8) or jump (0xe9) to `target` at `address`, the rest up to `length`
   filled with nops. */
static inline bool PatchBranch(unsigned address, unsigned char opcode, const void* target, unsigned length)
{
    unsigned char code[40];
    if (length < 5 || length > sizeof(code)) {
        return false;
    }
    memset(code, 0x90, length);
    code[0] = opcode;
    int rel = static_cast<int>(reinterpret_cast<unsigned>(target) - (address + 5));
    memcpy(code + 1, &rel, 4);
    return PatchWrite(address, code, length);
}

/* The code at `address` is exactly these bytes. */
static inline bool PatchBytes(unsigned address, const unsigned char* expected, unsigned length)
{
    return memcmp(reinterpret_cast<const void*>(address), expected, length) == 0;
}

/* The function at `address` is still the original's (the hook list puts a jmp at its start). */
static inline bool PatchOriginal(unsigned address)
{
    return *reinterpret_cast<const unsigned char*>(address) != 0xe9;
}

/* The instruction at `address` is a call rel32 to `target`. */
static inline bool PatchCallsTo(unsigned address, unsigned target)
{
    const unsigned char* code = reinterpret_cast<const unsigned char*>(address);
    int rel = 0;
    memcpy(&rel, code + 1, 4);
    return code[0] == 0xe8 && address + 5 + rel == target;
}

/* The word for wiz8hy.log: done - "patched", the bytes matched but the write failed -
   "FAILED", not the expected bytes - "SKIPPED". */
static inline const char* PatchResult(bool ok, bool done)
{
    return done ? "patched" : ok ? "FAILED" : "SKIPPED";
}

#endif
