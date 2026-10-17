/* msvc2022 port: route the executable's global operator new / delete to
   MSVCRT.dll, exactly where retail Wiz8.exe (VC6 /MD) sent them.

   Why: retail SR.dll is a VC6 /MD image, so its own ::operator new/delete are
   MSVCRT.dll!??2@YAPAXI@Z / ??3@YAXPAX@Z. Several objects cross the exe/DLL
   boundary with their ownership:
     - game_timer.cpp does `new srTimer(...)` in the exe and later `delete`s it;
       srTimer's virtual destructor (and so its scalar deleting destructor,
       which calls operator delete) lives in SR.dll's vtable;
     - inline SurRender header code (srIOManager importer lists,
       srIStreamOpener::registerType, srTypeRegistry::ChildList) allocates with
       plain new in whichever image compiled it, and the other image may free;
     - exe-defined srVertexProcessor / srIStreamOpener::Opener subclasses can be
       destroyed from inside SR.dll.
   A modern CRT (/MT or /MD-ucrt) has a different heap from MSVCRT.dll, so any
   of those would free a block into the wrong heap. Routing the global operator
   pair to MSVCRT.dll makes both images share one allocator again.

   srRuntimeClass-derived classes and srVector2T/3T/4T and srARGB arrays use their own
   class operator new/delete (srHeap inside SR.dll) and are unaffected.

   VC6 operator new returned NULL on failure instead of throwing; MSVCRT's
   ??2 keeps that behavior, which the recovered code relies on. */

#include <new>

/* Load-time imports of MSVCRT.dll's ??2@YAPAXI@Z / ??3@YAXPAX@Z under private
   names (IAT slots from tools/msvc2022/make_import_aliases.py). No run-time
   lookup and no fallback: the loader binds them or refuses to start the exe.
   malloc/free/realloc/calloc are routed to MSVCRT.dll the same way in
   msvcrt_crt_bridge.c, so new and malloc share msvcrt's single heap exactly
   as in retail. */
extern "C" __declspec(dllimport) void* __cdecl w8crt_new(size_t size);
extern "C" __declspec(dllimport) void __cdecl w8crt_delete(void* block);

namespace {

inline void* BridgeNew(size_t size)
{
    return w8crt_new(size);
}

inline void BridgeDelete(void* block)
{
    w8crt_delete(block); /* MSVCRT's ??3 ignores NULL, like VC6 */
}

} // namespace

void* __cdecl operator new(size_t size)
{
    return BridgeNew(size);
}

void* __cdecl operator new[](size_t size)
{
    return BridgeNew(size);
}

void* __cdecl operator new(size_t size, const std::nothrow_t&) noexcept
{
    return BridgeNew(size);
}

void* __cdecl operator new[](size_t size, const std::nothrow_t&) noexcept
{
    return BridgeNew(size);
}

void __cdecl operator delete(void* block) noexcept
{
    BridgeDelete(block);
}

void __cdecl operator delete[](void* block) noexcept
{
    BridgeDelete(block);
}

void __cdecl operator delete(void* block, size_t) noexcept
{
    BridgeDelete(block);
}

void __cdecl operator delete[](void* block, size_t) noexcept
{
    BridgeDelete(block);
}

void __cdecl operator delete(void* block, const std::nothrow_t&) noexcept
{
    BridgeDelete(block);
}

void __cdecl operator delete[](void* block, const std::nothrow_t&) noexcept
{
    BridgeDelete(block);
}
