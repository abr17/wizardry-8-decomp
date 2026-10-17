cmake_minimum_required(VERSION 3.20)

project(wizardry8_decomp LANGUAGES C CXX)

# Modern-MSVC port (VS 2022, x86): builds only the game executable against the
# retail SR.dll / Mss32.dll / binkw32.dll. Detected from the compiler version so
# the original VC6 path stays untouched when MSVC 6 is used.
if(MSVC AND MSVC_VERSION GREATER_EQUAL 1900)
    set(WIZ8_MODERN_MSVC ON)
else()
    set(WIZ8_MODERN_MSVC OFF)
endif()

# Global policy only: shared compile settings and immediate lint configuration.
# Component sources, includes, defines and products belong to the component
# directory, never here.
if(WIZ8_MODERN_MSVC)
    include(cmake/ModernMsvc.cmake)
endif()
include(cmake/CompileSettings.cmake)
include(cmake/Lint.cmake)

if(NOT WIZ8_ANALYSIS_BUILD)
    if(NOT MSVC)
        message(FATAL_ERROR "The matching build requires the pinned MSVC 6 container")
    endif()
    enable_language(RC)
    # CRT selection is explicit on the component targets, not a build-type default.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "")
    # Final images stay at the build root even though components own sources.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    include(cmake/ProductSupport.cmake)
endif()

add_subdirectory(src/sgp)
# msvc2022 port: SURRENDER (partial sr.dll), srext_jpegimporter, srext_unzip,
# tests/differential and reccmp are not built - the exe uses the retail DLLs and
# the IJG jpeg / unzip sources are not available here.
add_subdirectory(src/wiz8)
