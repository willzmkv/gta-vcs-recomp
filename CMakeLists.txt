cmake_minimum_required(VERSION 3.21)
project(vcs_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generated C++ from the user's own SLUS_215.90 (tools/recompile.py). Never committed.
set(VCS_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated/ee" CACHE PATH "Recompiled EE code")

set(PS2X_LTO "THIN" CACHE STRING "Link-time optimisation for the game: THIN (default, the game to play), OFF (quick dev builds), FULL")
set(PS2X_PGO "" CACHE STRING "Profile-guided optimisation: empty (off), GEN (instrumented), or a .profdata path (tools/build.py --opt)")
set(PS2X_CPU "AVX2" CACHE STRING "The CPU the build runs on: AVX2 (Haswell / Zen and newer, the default), AVX (Sandy / Ivy Bridge, Bulldozer) or SSE4 (no AVX); tools/build.py picks it from this PC")
set_property(CACHE PS2X_CPU PROPERTY STRINGS AVX2 AVX SSE4)
option(VCS_BUILD_TOOLS "Build the PS2Recomp recompiler/analyzer" ON)

set(PS2X_BUILD_RECOMP ${VCS_BUILD_TOOLS} CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER ${VCS_BUILD_TOOLS} CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)

# Linux (docs/LINUX.md) and Windows' llvm-mingw (DECISIONS.md 2026-11-03): clang 19+ only - the same compiler as
# clang-cl, and PS2_GUEST_CC needs its preserve_none. Linked with lld (ThinLTO).
if(NOT MSVC)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
        message(FATAL_ERROR "The game needs clang 19 or newer (found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}). "
                            "python tools/build.py picks it up (docs/LINUX.md; Windows: llvm-mingw, downloaded)")
    endif()
    add_link_options(-fuse-ld=lld)
endif()
# llvm-mingw: mingw-w64's headers default to Windows 7, the Windows SDK's to Windows 10 - the same APIs as clang-cl's
# build. Every program is linked -static: libc++, libunwind and winpthread inside, only the system's UCRT left.
if(MINGW)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
    add_link_options(-static)
endif()
# Linux: compile and ThinLTO jobs follow the memory - a unity batch of generated code takes up to ~2 GB to compile.
if(NOT WIN32)
    cmake_host_system_information(RESULT _vcs_mem_mb QUERY TOTAL_PHYSICAL_MEMORY)
    cmake_host_system_information(RESULT _vcs_cores QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR _vcs_jobs "${_vcs_mem_mb} / 2048")
    if(_vcs_jobs LESS 1)
        set(_vcs_jobs 1)
    endif()
    if(_vcs_jobs GREATER _vcs_cores)
        set(_vcs_jobs ${_vcs_cores})
    endif()
    set(VCS_COMPILE_JOBS ${_vcs_jobs} CACHE STRING "Parallel compile jobs (default: one per 2 GB of memory, at most one per core)")
    set(PS2X_LTO_JOBS ${_vcs_jobs} CACHE STRING "Parallel ThinLTO backend jobs at the game's link")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS vcs_compile=${VCS_COMPILE_JOBS})
    set(CMAKE_JOB_POOL_COMPILE vcs_compile)
    message(STATUS "Linux build: ${VCS_COMPILE_JOBS} compile jobs, ${PS2X_LTO_JOBS} ThinLTO jobs (${_vcs_mem_mb} MB memory)")
endif()

set(VCS_HAVE_GENERATED OFF)
if(EXISTS "${VCS_GENERATED_DIR}/register_functions.cpp")
    set(VCS_HAVE_GENERATED ON)
endif()
set(PS2X_BUILD_RUNTIME ${VCS_HAVE_GENERATED} CACHE BOOL "" FORCE)

file(GLOB_RECURSE VCS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)
list(REMOVE_ITEM VCS_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp")
# The platform layer, RHI and renderer are their own libraries (src/*/CMakeLists.txt), linked into the runtime.
list(FILTER VCS_SOURCES EXCLUDE REGEX "/src/(platform|rhi|render)/")
# VU1 microprograms compiled to C++ by tools/vu1_recomp.py (from the user's ELF; see game/vcs/vu1_uploads.txt)
if(EXISTS "${CMAKE_SOURCE_DIR}/generated/vu1/vu1_compiled.cpp")
    list(APPEND VCS_SOURCES "${CMAKE_SOURCE_DIR}/generated/vu1/vu1_compiled.cpp")
endif()
# Worker copies of the animation node code (tools/anim_worker_gen.py, from generated/ee; src/game/vcs_anim.cpp): made
# again when the generated code or the tool changes
set(VCS_ANIM_WORKER OFF)
if(VCS_HAVE_GENERATED)
    find_package(Python3 COMPONENTS Interpreter) # (tools/build.py passes its own; without one: no worker copies)
endif()
if(VCS_HAVE_GENERATED AND Python3_FOUND)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/anim_worker_gen.py" "${VCS_GENERATED_DIR}/register_functions.cpp")
    execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/anim_worker_gen.py" "${VCS_GENERATED_DIR}"
                            "${CMAKE_SOURCE_DIR}/generated/anim/anim_worker.cpp"
                    RESULT_VARIABLE _vcs_anim_result OUTPUT_VARIABLE _vcs_anim_out ERROR_VARIABLE _vcs_anim_out)
    message(STATUS "${_vcs_anim_out}")
    if(_vcs_anim_result EQUAL 0 AND EXISTS "${CMAKE_SOURCE_DIR}/generated/anim/anim_worker.cpp")
        list(APPEND VCS_SOURCES "${CMAKE_SOURCE_DIR}/generated/anim/anim_worker.cpp")
        set(VCS_ANIM_WORKER ON)
    endif()
endif()

set(PS2X_RUNNER_SOURCE_DIR "${VCS_GENERATED_DIR}" CACHE PATH "" FORCE)
set(PS2X_RUNNER_EXTRA_SOURCES "${VCS_SOURCES}" CACHE STRING "" FORCE)
set(PS2X_RUNNER_MAIN "${CMAKE_SOURCE_DIR}/src/main.cpp" CACHE FILEPATH "" FORCE)

set(PS2X_PARALLEL_GS_DIR "${CMAKE_SOURCE_DIR}/third_party/parallel-gs" CACHE PATH "" FORCE)
if(VCS_HAVE_GENERATED)
    add_subdirectory(src/platform)
    add_subdirectory(src/rhi)
    add_subdirectory(src/render)
    add_library(vcs_host INTERFACE)
    target_link_libraries(vcs_host INTERFACE vcs_platform vcs_render)
    set(PS2X_HOST_PLATFORM_TARGET vcs_host CACHE STRING "" FORCE)
endif()
add_subdirectory(third_party/PS2Recomp)

if(VCS_HAVE_GENERATED)
    set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME "vcs")
    # The main build tree (build/release) puts vcs.exe, its DLLs and vcs.ini at the top of build/ instead of
    # build/release/third_party/PS2Recomp/ps2xRuntime; other trees (PGO training's build/pgo-gen) keep theirs.
    # VCS_GAME_DIR: the build folder chosen elsewhere (tools/toolchain.py passes it for the main tree; docs/LAUNCHER.md).
    set(VCS_GAME_DIR "" CACHE PATH "Where vcs.exe, its DLLs and vcs.ini go (empty: build/ for build/release)")
    string(TOLOWER "${CMAKE_BINARY_DIR}" _vcs_binary_dir)
    string(TOLOWER "${CMAKE_SOURCE_DIR}/build/release" _vcs_main_tree)
    if(VCS_GAME_DIR)
        set_target_properties(ps2EntryRunner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${VCS_GAME_DIR}")
    elseif(_vcs_binary_dir STREQUAL _vcs_main_tree)
        set_target_properties(ps2EntryRunner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
    endif()
    target_include_directories(ps2EntryRunner PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(ps2EntryRunner PRIVATE vcs_host) # main.cpp: settings menu (Dear ImGui), presenter aspect
    # Windows: our manifest in place of the linker's, for the UTF-8 code page (non-ASCII folders; src/vcs.manifest)
    # ... and the exe's icon and version information (src/vcs.rc): VERSION in the repository root, as a header only
    # the resource file sees - a compile definition would rebuild every generated file at each new version
    if(WIN32)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
        file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" VCS_VERSION LIMIT_COUNT 1)
        string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _vcs_version_numbers "${VCS_VERSION}")
        if(NOT _vcs_version_numbers)
            message(FATAL_ERROR "VERSION must start with major.minor.patch (it holds '${VCS_VERSION}')")
        endif()
        file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/vcs_version/vcs_version.h" CONTENT "#define VCS_VERSION_MAJOR ${CMAKE_MATCH_1}
#define VCS_VERSION_MINOR ${CMAKE_MATCH_2}
#define VCS_VERSION_PATCH ${CMAKE_MATCH_3}
#define VCS_VERSION_TEXT \"${VCS_VERSION}\"
")
        target_sources(ps2EntryRunner PRIVATE "${CMAKE_SOURCE_DIR}/src/vcs.rc")
        # the resource compiler doesn't report the files it embeds
        set_source_files_properties("${CMAKE_SOURCE_DIR}/src/vcs.rc" TARGET_DIRECTORY ps2EntryRunner
            PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/vcs.manifest;${CMAKE_SOURCE_DIR}/src/vcs.ico;${CMAKE_BINARY_DIR}/vcs_version/vcs_version.h"
                       INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/vcs_version")
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE /MANIFEST:NO)  # lld in mingw mode makes none of its own
        endif()
    endif()
    # Our own sources aren't generated code; keep them out of the unity batches.
    set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
        TARGET_DIRECTORY ps2EntryRunner
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE SKIP_PRECOMPILE_HEADERS TRUE)
    if(VCS_ANIM_WORKER)
        set_source_files_properties("${CMAKE_SOURCE_DIR}/src/game/vcs_anim.cpp" TARGET_DIRECTORY ps2EntryRunner
            PROPERTIES COMPILE_DEFINITIONS VCS_ANIM_WORKER=1)
    endif()
else()
    message(STATUS "No generated code in ${VCS_GENERATED_DIR}: building tools only. Run python tools/build.py --recompile.")
endif()

# Debug symbols for the runtime and our code (not the 240 MB of generated code),
# so the sampling profiler and debuggers can name host functions.
option(VCS_RUNTIME_DEBUG_INFO "Emit CodeView debug info for runtime + launcher" ON)
if(VCS_RUNTIME_DEBUG_INFO AND MSVC)
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE /Z7)
        endif()
    endforeach()
    if(VCS_HAVE_GENERATED)
        set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "/Z7")
        target_link_options(ps2EntryRunner PRIVATE /DEBUG)
        target_link_libraries(ps2EntryRunner PRIVATE dbghelp)
    endif()
elseif(VCS_RUNTIME_DEBUG_INFO AND MINGW)
    # llvm-mingw: CodeView in a PDB, as clang-cl's /Z7 /DEBUG - dbghelp (src/debug: crash handler, profiler) and
    # tools/minidbg.py read it the same way
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -g -gcodeview)
        endif()
    endforeach()
    if(VCS_HAVE_GENERATED)
        set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "-g;-gcodeview")
        target_link_options(ps2EntryRunner PRIVATE -g -Wl,--pdb=)
        target_link_libraries(ps2EntryRunner PRIVATE dbghelp)
    endif()
elseif(VCS_RUNTIME_DEBUG_INFO)
    # Linux: line tables for the crash handler / profiler (src/debug, symbolized with llvm-symbolizer); frame pointers
    # for their backtraces
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -gline-tables-only -fno-omit-frame-pointer)
        endif()
    endforeach()
    if(VCS_HAVE_GENERATED)
        set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "-gline-tables-only;-fno-omit-frame-pointer")
        target_link_options(ps2EntryRunner PRIVATE -rdynamic)
    endif()
endif()
# llvm-mingw: the profiler's __try / __except (src/debug/profiler.cpp) need the Microsoft extensions clang-cl has on
# (here, after the debug info's COMPILE_OPTIONS above, which would replace them)
if(MINGW AND VCS_HAVE_GENERATED)
    set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/debug/profiler.cpp" TARGET_DIRECTORY ps2EntryRunner APPEND PROPERTY
                 COMPILE_OPTIONS -fms-extensions)
endif()
