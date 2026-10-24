/* Experimental (2026-10-22, the patch author's request): fixes of the original game's own bugs,
   one switch for all of them ([Experimental] OriginalFixes=1, off by default; see
   include/wiz8/experimental.h for the rules of the experimental features). With the switch off
   the patched spots behave exactly as the original. Design: docs/experimental.md.

   Fixes 1-3 are small patches of the original machine code, installed by the hybrid
   (HybridInstall) whatever the switch, and each one checks the switch every time it runs, so
   the setting applies at once:

   1. W8Octree::ReadRegionLinkFile (0x00432e90) leaves the level's region link file (.RLK) open
      when it cannot read the 4-byte count at its start (0x00432f6c jumps past the FileClose).
      Five .RLK files in Levels\Levels.slf are empty (Arnika Road from the starter dungeon,
      Arnika - Trynton, Trynton, Savant's tower, Ascension), so every load of these levels
      leaks one open file of that archive.
   2. OpenFileFromLibrary (0x004133d0) grows an archive's open file array with
      realloc(array, count + 20) - bytes, while each entry is 16 bytes (0x00413570). At the
      20th file open at once (e.g. 19 leaked by 1.) the array shrinks to 40 bytes, entries past
      it are garbage, and a later file name check reads through one of them: the silent crash
      on Arnika Road. Fixed: (count + 20) * 16 bytes, the new entries zeroed (a free entry is
      one with file id 0).
   3. Closing the window (Alt+F4, the X in window mode) crashes on exit. WM_DESTROY runs
      ShutdownStandardGamingPlatform first (fonts, video, memory freed); then the atexit handler
      SGPExit (0x004017f0) still calls GameloopExit (0x004e34b0, at 0x0040181b), whose first
      SetFontObjectPalette16BPP touches a freed font (0xC0000005 at 0x00406dcf). A quit from
      the menu runs SGPExit first, so GameloopExit comes before the shutdown there. Fixed:
      SGPExit skips GameloopExit when the shutdown has already run (its flag 0x00650db4).
   4. The radar and the formation board never delete the surfaces their sprites were copied
      from (a leak on every level load and radar show). Not a patch here: RefreshRadarMap and
      RefreshFormationBoard are transplanted, and their C++ (MGSRadarMap.cpp, MGSFormation.cpp)
      deletes the surfaces when the switch is on. */
#include <windows.h>
#include <string.h>
#include "wiz8/experimental.h"
#include "patch_util.h"

#include <stdio.h>

namespace {

/* Read by the patched code on every pass: 0 = the original's behaviour. */
volatile int s_enabled = -1;
/* Once an open file array has been sized in entries, it is never shrunk again by the
   original's byte count (the switch turned off later in the session). */
volatile int s_open_files_resized;

const unsigned kReadRegionLinkFile = 0x00432e90;
const unsigned kOpenFileFromLibrary = 0x004133d0;
const unsigned kRlkCountFailJump = 0x00432f6c; /* je rel32 -> 0x004331c7 (no FileClose) */
const unsigned kRlkExit = 0x004331c7;          /* epilogue, returns al */
const unsigned kFileClose = 0x00404e10;        /* cdecl void FileClose(HWFILE) */
const unsigned kLibraryRealloc = 0x00413570;   /* call dword ptr [0x005eb1f8] (realloc) */
const unsigned kReallocImport = 0x005eb1f8;
const int kOpenFileEntrySize = 16;              /* FileOpenStruct */
const int kOpenFilesStep = 20;                  /* NUM_FILES_TO_ADD_AT_A_TIME */
const unsigned kSgpExit = 0x004017f0;
const unsigned kSgpExitLoopCall = 0x0040181b;  /* call rel32 -> GameloopExit, then add esp, 4 */
const unsigned kGameloopExit = 0x004e34b0;     /* cdecl void GameloopExit(BOOLEAN) */
const unsigned kSgpShutdownDone = 0x00650db4;  /* ShutdownStandardGamingPlatform's Reenter */

char s_status[320];

/* Fix 1: reached instead of the original's direct jump to the epilogue; edi holds the file. */
__declspec(naked) void RegionLinkCountFail(void)
{
    __asm {
        cmp s_enabled, 0
        jle done
        push edi
        mov eax, kFileClose
        call eax
        add esp, 4
    done:
        xor eax, eax /* al = 0: the original returns the failed read's result */
        mov ecx, kRlkExit
        jmp ecx
    }
}

/* Fix 2: called instead of realloc (cdecl, the caller pops both arguments). */
void* __cdecl LibraryOpenFilesRealloc(void* array, unsigned count)
{
    typedef void*(__cdecl * Realloc)(void*, size_t);
    Realloc original = *reinterpret_cast<Realloc*>(kReallocImport);
    if ((s_enabled <= 0 && !s_open_files_resized) || count < static_cast<unsigned>(kOpenFilesStep)) {
        return original(array, count);
    }
    void* grown = original(array, count * kOpenFileEntrySize);
    if (grown != 0) {
        memset(static_cast<char*>(grown) + (count - kOpenFilesStep) * kOpenFileEntrySize, 0,
               kOpenFilesStep * kOpenFileEntrySize);
        s_open_files_resized = 1;
    }
    return grown;
}

/* Fix 3: called instead of GameloopExit from SGPExit (cdecl, the caller pops the argument). */
void __cdecl SgpExitGameloopExit(unsigned char release_screens)
{
    typedef void(__cdecl * GameloopExitFn)(unsigned char);
    if (s_enabled > 0 && *reinterpret_cast<volatile unsigned char*>(kSgpShutdownDone) != 0) {
        return;
    }
    reinterpret_cast<GameloopExitFn>(kGameloopExit)(release_screens);
}

} // namespace

bool ExperimentalOriginalFixesEnabled(void)
{
    if (s_enabled < 0) {
        s_enabled = ExperimentalReadSwitch("OriginalFixes") ? 1 : 0;
    }
    return s_enabled == 1;
}

void ExperimentalSetOriginalFixes(bool enabled)
{
    s_enabled = enabled ? 1 : 0;
    ExperimentalWriteSwitch("OriginalFixes", enabled);
}

const char* ExperimentalInstallOriginalFixes(void)
{
    ExperimentalOriginalFixesEnabled();
    const unsigned char* jump = reinterpret_cast<const unsigned char*>(kRlkCountFailJump);
    int jump_target = 0;
    memcpy(&jump_target, jump + 2, 4);
    const unsigned char realloc_call[6] = {0xff, 0x15, 0xf8, 0xb1, 0x5e, 0x00}; /* call [kReallocImport] */
    const unsigned char pop_argument[3] = {0x83, 0xc4, 0x04};                   /* add esp, 4 */
    /* only over the exact original bytes (GOG/Steam 1.2.4), and only while both functions are
       the original's (a hook list that replaced one would leave its retail body dead); the
       same for SGPExit */
    const bool jump_ok = jump[0] == 0x0f && jump[1] == 0x84 &&
                         kRlkCountFailJump + 6 + jump_target == kRlkExit && PatchOriginal(kReadRegionLinkFile);
    const bool call_ok = PatchBytes(kLibraryRealloc, realloc_call, sizeof(realloc_call)) &&
                         PatchOriginal(kOpenFileFromLibrary);
    const bool exit_ok = PatchCallsTo(kSgpExitLoopCall, kGameloopExit) &&
                         PatchBytes(kSgpExitLoopCall + 5, pop_argument, sizeof(pop_argument)) &&
                         PatchOriginal(kSgpExit);
    bool jump_done = false;
    if (jump_ok) {
        int rel = static_cast<int>(reinterpret_cast<unsigned>(&RegionLinkCountFail) - (kRlkCountFailJump + 6));
        unsigned char code[6] = {0x0f, 0x84};
        memcpy(code + 2, &rel, 4);
        jump_done = PatchWrite(kRlkCountFailJump, code, 6);
    }
    const bool call_done =
        call_ok && PatchBranch(kLibraryRealloc, 0xe8, reinterpret_cast<const void*>(&LibraryOpenFilesRealloc), 6);
    const bool exit_done =
        exit_ok && PatchBranch(kSgpExitLoopCall, 0xe8, reinterpret_cast<const void*>(&SgpExitGameloopExit), 5);
    _snprintf(s_status, sizeof(s_status),
              "%s; region link close %s, open file array %s, exit after window close %s",
              s_enabled == 1 ? "on" : "off (the original's behaviour)", PatchResult(jump_ok, jump_done),
              PatchResult(call_ok, call_done), PatchResult(exit_ok, exit_done));
    s_status[sizeof(s_status) - 1] = '\0';
    return s_status;
}