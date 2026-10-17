# msvc2022 port: compiler/linker policy that reproduces the VC6 SP5 language
# model on MSVC 14.x (VS 2022), x86. Included from the top-level CMakeLists
# before CompileSettings.cmake, only when WIZ8_MODERN_MSVC is ON.
#
# Every flag here replaces a VC6 default that modern MSVC changed. Source files
# are edited only where no flag or compat header can express the old behavior.

set(WIZ8_MODERN_MSVC_FLAGS
    # VC6 library behavior restored in one header (after compiler.h)
    "/FI${CMAKE_CURRENT_SOURCE_DIR}/include/wiz8/compat/msvc2022.h"
    /Zc:wchar_t-          # VC6: wchar_t is a typedef of unsigned short (mangles as G)
    /Zc:forScope-         # VC6: a for-init variable stays visible after the loop
    /Zc:threadSafeInit-   # VC6: no magic statics (no TLS init guard in the exe)
    /permissive           # VC6-era two-phase-less template / conversion rules
    /GS-                  # VC6 had no stack cookies; retail overruns stay non-fatal
    /arch:IA32            # VC6 x87 code generation (x86 default is SSE2 now)
    /source-charset:utf-8 # non-ASCII bytes appear only in UTF-8 comments
    /bigobj
    /wd4430               # implicit int (missing type specifier) is legal VC6 C++
    /wd4996               # deprecated CRT names (backup for the defines below)
    /wd4068               # '#pragma clang ...' lines kept for the lint lane
    /wd4530               # unused <ostream> wchar_t-as-ushort specialization has try/catch
)

set(WIZ8_MODERN_MSVC_DEFINES
    _CRT_SECURE_NO_WARNINGS
    _CRT_SECURE_NO_DEPRECATE
    _CRT_NONSTDC_NO_DEPRECATE
    _CRT_NON_CONFORMING_SWPRINTFS # 2-argument swprintf/vswprintf (VC6 signature)
    _CRT_NON_CONFORMING_WCSTOK    # 2-argument wcstok (VC6 signature)
    _USE_32BIT_TIME_T             # VC6 time_t is 32-bit; keep struct layouts
)

# Linker options shared by the modern-MSVC images (replaces the VC6-only
# /PDBTYPE:SEPT, /MAPINFO:LINES and subsystem 4.0, which LINK 14 rejects).
set(WIZ8_MODERN_MSVC_LINK_OPTIONS
    /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:NOICF
    /SAFESEH:NO /DYNAMICBASE:NO /NXCOMPAT:NO
    /LARGEADDRESSAWARE:NO
    /SUBSYSTEM:WINDOWS,5.01
    /MANIFEST:NO          # retail Wiz8.exe has no manifest: keep legacy UAC virtualization/appcompat
    /TSAWARE:NO           # retail DLL characteristics are 0 (no TERMINAL_SERVER_AWARE bit)
)

# sr-assert-import.lib: hand-built import member that maps the fixed-arity
# caller ?srAssertFail@@YAXPBD0J0@Z onto SR.dll's variadic export
# ?srAssertFail@@YAXPBD0J0ZZ. Produced by the decomp's own stdlib-only helper
# (tools/wiz8decomp/binary/coff_archive.py) at configure time.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(WIZ8_SR_ASSERT_IMPORT "${CMAKE_BINARY_DIR}/sr-assert-import.lib")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/msvc2022/make_sr_assert_import.py"
        "${WIZ8_SR_ASSERT_IMPORT}"
    RESULT_VARIABLE wiz8_sr_assert_result
)
if(NOT wiz8_sr_assert_result EQUAL 0)
    message(FATAL_ERROR "Could not generate ${WIZ8_SR_ASSERT_IMPORT}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/msvc2022/make_sr_assert_import.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/wiz8decomp/binary/coff_archive.py")

# msvcrt-bridge-import.lib: IAT slots that bind private names to MSVCRT.dll's
# heap and x87 helpers (src/msvc2022_compat/msvcrt_heap_bridge.cpp and
# msvcrt_crt_bridge.c). Each caller name is the decorated symbol after __imp_.
set(WIZ8_MSVCRT_BRIDGE_IMPORT "${CMAKE_BINARY_DIR}/msvcrt-bridge-import.lib")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/msvc2022/make_import_aliases.py"
        "${WIZ8_MSVCRT_BRIDGE_IMPORT}" msvcrt.dll
        "_w8crt_new=??2@YAPAXI@Z"
        "_w8crt_delete=??3@YAXPAX@Z"
        _w8crt_malloc=malloc
        _w8crt_free=free
        _w8crt_calloc=calloc
        _w8crt_realloc=realloc
        _w8crt_msize=_msize
        _w8crt_strdup=_strdup
        _w8crt_wcsdup=_wcsdup
        _w8crt_atof=atof
        _w8crt_atoi=atoi
        _w8crt_atol=atol
        _w8crt_CIpow=_CIpow
        _w8crt_CIacos=_CIacos
        _w8crt_CIasin=_CIasin
    RESULT_VARIABLE wiz8_msvcrt_bridge_result
)
if(NOT wiz8_msvcrt_bridge_result EQUAL 0)
    message(FATAL_ERROR "Could not generate ${WIZ8_MSVCRT_BRIDGE_IMPORT}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/msvc2022/make_import_aliases.py")