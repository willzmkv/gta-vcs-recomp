# The launcher (docs/LAUNCHER.md): finds and checks the disc, installs what the build needs, runs tools/build.py and
# starts the game. A project of its own - no generated code, no disc, no third-party code - so CI builds it in seconds
# for a release:
#   python tools/build.py --launcher        (build/launcher, VCSLauncher(.exe) in the repository root)
# Its own window, renderer and UI (uppies' design). Windows: Win32, Direct3D 11, DirectWrite. Linux: X11 or Wayland,
# Vulkan, FreeType + fontconfig - the system's libraries, nothing downloaded. Layers, each using only those below it:
# screens/ -> ui/ -> render/ -> platform/ -> core/; model/ is the state and the tools it runs.
cmake_minimum_required(VERSION 3.21)
project(vcs_launcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(VCS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VCS_LAUNCHER_SHADERS "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${VCS_LAUNCHER_SHADERS}")
set(VCS_UI_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/render/d3d11/ui.hlsl")

set(VCS_LAUNCHER_COMMON
    main.cpp
    core/png.cpp
    render/draw.cpp
    render/font.cpp
    render/icons.cpp
    ui/ui.cpp
    ui/widgets.cpp
    screens/screens.cpp
    screens/backdrop.cpp
    screens/titlebar.cpp
    screens/setup.cpp
    screens/home.cpp
    screens/task.cpp
    screens/sheets.cpp
    screens/overlays.cpp
    screens/gallery.cpp
    model/tasks.cpp
    model/process.cpp
    model/disc_check.cpp
    model/json.cpp
    "${VCS_ROOT}/src/disc/iso_image.cpp"
)

if(WIN32)
    # the shader, compiled at build time into headers (no runtime compiler, shader errors are build errors): Windows'
    # own d3dcompiler_47.dll through tools/hlsl_dxbc.py, fxc's DXBC without the Windows SDK
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(stage vs ps)
        add_custom_command(
            OUTPUT "${VCS_LAUNCHER_SHADERS}/ui_${stage}.h"
            COMMAND "${Python3_EXECUTABLE}" -I "${VCS_ROOT}/tools/hlsl_dxbc.py" --profile ${stage}_4_0 --entry ${stage}_main
                    --name g_ui_${stage} --out "${VCS_LAUNCHER_SHADERS}/ui_${stage}.h" "${VCS_UI_HLSL}"
            DEPENDS "${VCS_UI_HLSL}" "${VCS_ROOT}/tools/hlsl_dxbc.py"
            COMMENT "hlsl_dxbc ui.hlsl ${stage}_main"
            VERBATIM)
    endforeach()

    add_executable(VCSLauncher WIN32
        ${VCS_LAUNCHER_COMMON}
        platform/platform_win32.cpp
        render/font_dwrite.cpp
        render/d3d11/renderer_d3d11.cpp
        model/process_win32.cpp
        model/system_win32.cpp
        "${VCS_LAUNCHER_SHADERS}/ui_vs.h"
        "${VCS_LAUNCHER_SHADERS}/ui_ps.h"
        launcher.rc
    )
    # the resource compiler doesn't report the files it embeds: a redrawn icon would otherwise not rebuild
    set_source_files_properties(launcher.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/launcher.ico")
    target_link_libraries(VCSLauncher PRIVATE d3d11 dxgi dwrite dwmapi ole32 shell32 uuid winmm advapi32)
    target_compile_definitions(VCSLauncher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MINGW)  # mingw-w64's headers default to Windows 7; the Windows SDK's (and the launcher) to Windows 10
        target_compile_definitions(VCSLauncher PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    endif()
else()
    # Linux. Everything comes from the distribution (python3 tools/build.py --check-deps lists the packages):
    # pkg-config modules for the window systems, fonts and D-Bus; Vulkan headers (the loader is opened at run time);
    # glslang for the shader; wayland-scanner for the Wayland protocols.
    find_package(PkgConfig REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    pkg_check_modules(VCS_LAUNCHER_LIBS REQUIRED IMPORTED_TARGET
        x11 xcursor xrandr wayland-client wayland-cursor xkbcommon freetype2 fontconfig dbus-1)
    # the protocols' XML: the system's, or a newer wayland-protocols unpacked somewhere (a release built on an older
    # distribution: CI's Ubuntu 22.04 has 1.25, without fractional-scale and cursor-shape)
    set(VCS_WAYLAND_PROTOCOLS_DIR "" CACHE PATH "wayland-protocols' XML folder (empty: the system's)")
    if(NOT VCS_WAYLAND_PROTOCOLS_DIR)
        pkg_check_modules(VCS_WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
        pkg_get_variable(VCS_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    endif()
    find_program(VCS_WAYLAND_SCANNER wayland-scanner REQUIRED)
    find_path(VCS_VULKAN_INCLUDE vulkan/vulkan_core.h HINTS "$ENV{VULKAN_SDK}/include")
    if(NOT VCS_VULKAN_INCLUDE)
        message(FATAL_ERROR "the Vulkan headers were not found: install libvulkan-dev (Debian / Ubuntu), "
                            "vulkan-headers (Fedora / Arch) - docs/LINUX.md")
    endif()
    find_program(VCS_GLSLANG NAMES glslangValidator glslang HINTS "$ENV{VULKAN_SDK}/bin")
    if(NOT VCS_GLSLANG)
        message(FATAL_ERROR "glslangValidator not found (the launcher's shader): install glslang-tools (Debian / "
                            "Ubuntu), glslang (Fedora / Arch) - docs/LINUX.md")
    endif()

    # the one ui.hlsl, as SPIR-V for Vulkan 1.1 (tools/hlsl_spirv.py: the D3D registers' bindings)
    foreach(stage vs ps)
        if(stage STREQUAL "vs")
            set(_glsl_stage vert)
        else()
            set(_glsl_stage frag)
        endif()
        add_custom_command(
            OUTPUT "${VCS_LAUNCHER_SHADERS}/ui_${stage}.spv.h"
            COMMAND "${Python3_EXECUTABLE}" -I "${VCS_ROOT}/tools/hlsl_spirv.py" --glslang "${VCS_GLSLANG}"
                    --target-env vulkan1.1 --stage ${_glsl_stage} --entry ${stage}_main --name g_ui_${stage}_spv
                    --out "${VCS_LAUNCHER_SHADERS}/ui_${stage}.spv.h" "${VCS_UI_HLSL}"
            DEPENDS "${VCS_UI_HLSL}" "${VCS_ROOT}/tools/hlsl_spirv.py"
            COMMENT "glslang ui.hlsl ${stage}_main"
            VERBATIM)
    endforeach()

    # the X11 window icon, drawn by make_icon.py (Wayland's comes from the .desktop entry: the app id)
    add_custom_command(
        OUTPUT "${VCS_LAUNCHER_SHADERS}/launcher_icon.h"
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/make_icon.py" --header "${VCS_LAUNCHER_SHADERS}/launcher_icon.h"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_icon.py"
        COMMENT "make_icon.py --header"
        VERBATIM)

    # the Wayland protocols the launcher speaks, generated from the system's XML (stable first, else where older
    # wayland-protocols releases keep them)
    set(VCS_PROTOCOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
    file(MAKE_DIRECTORY "${VCS_PROTOCOL_DIR}")
    set(VCS_PROTOCOL_SOURCES)
    foreach(protocol
            "xdg-shell:stable/xdg-shell/xdg-shell.xml"
            "viewporter:stable/viewporter/viewporter.xml"
            "xdg-decoration-unstable-v1:unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
            "fractional-scale-v1:staging/fractional-scale/fractional-scale-v1.xml"
            "xdg-activation-v1:staging/xdg-activation/xdg-activation-v1.xml"
            "cursor-shape-v1:staging/cursor-shape/cursor-shape-v1.xml"
            "tablet-v2:stable/tablet/tablet-v2.xml|unstable/tablet/tablet-unstable-v2.xml" # cursor-shape refers to it
            "?xdg-toplevel-icon-v1:staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml") # ? optional (1.37+)
        set(_optional FALSE)
        if(protocol MATCHES "^\\?")
            set(_optional TRUE)
            string(SUBSTRING "${protocol}" 1 -1 protocol)
        endif()
        string(REPLACE ":" ";" _parts "${protocol}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _paths)
        string(REPLACE "|" ";" _paths "${_paths}")
        set(_xml "")
        foreach(_candidate ${_paths})
            if(NOT _xml AND EXISTS "${VCS_WAYLAND_PROTOCOLS_DIR}/${_candidate}")
                set(_xml "${VCS_WAYLAND_PROTOCOLS_DIR}/${_candidate}")
            endif()
        endforeach()
        if(NOT _xml AND _optional)
            message(STATUS "wayland-protocols without ${_name}: left out")
            continue()
        endif()
        if(NOT _xml)
            message(FATAL_ERROR "${VCS_WAYLAND_PROTOCOLS_DIR} has no ${_name} (wayland-protocols 1.32 or newer needed)")
        endif()
        if(_name STREQUAL "xdg-toplevel-icon-v1")
            set(VCS_HAVE_TOPLEVEL_ICON ON)
        endif()
        add_custom_command(
            OUTPUT "${VCS_PROTOCOL_DIR}/${_name}-client-protocol.h" "${VCS_PROTOCOL_DIR}/${_name}-protocol.c"
            COMMAND "${VCS_WAYLAND_SCANNER}" client-header "${_xml}" "${VCS_PROTOCOL_DIR}/${_name}-client-protocol.h"
            COMMAND "${VCS_WAYLAND_SCANNER}" private-code "${_xml}" "${VCS_PROTOCOL_DIR}/${_name}-protocol.c"
            DEPENDS "${_xml}"
            COMMENT "wayland-scanner ${_name}"
            VERBATIM)
        list(APPEND VCS_PROTOCOL_SOURCES "${VCS_PROTOCOL_DIR}/${_name}-protocol.c" "${VCS_PROTOCOL_DIR}/${_name}-client-protocol.h")
    endforeach()

    add_executable(VCSLauncher
        ${VCS_LAUNCHER_COMMON}
        platform/platform_linux.cpp
        platform/linux/x11.cpp
        platform/linux/wayland.cpp
        platform/linux/dbus.cpp
        render/font_freetype.cpp
        render/vulkan/renderer_vulkan.cpp
        model/process_posix.cpp
        model/system_linux.cpp
        "${VCS_LAUNCHER_SHADERS}/ui_vs.spv.h"
        "${VCS_LAUNCHER_SHADERS}/ui_ps.spv.h"
        "${VCS_LAUNCHER_SHADERS}/launcher_icon.h"
        ${VCS_PROTOCOL_SOURCES}
    )
    target_include_directories(VCSLauncher PRIVATE "${VCS_PROTOCOL_DIR}" "${VCS_VULKAN_INCLUDE}")
    if(VCS_HAVE_TOPLEVEL_ICON)
        target_compile_definitions(VCSLauncher PRIVATE VCS_HAVE_TOPLEVEL_ICON=1)
    endif()
    target_link_libraries(VCSLauncher PRIVATE PkgConfig::VCS_LAUNCHER_LIBS ${CMAKE_DL_LIBS})
    target_compile_options(VCSLauncher PRIVATE -Wall -Wextra -Wno-missing-field-initializers -fvisibility=hidden)
    # small and self-contained: stripped; the C++ runtime linked in, so the binary needs only glibc's version of the
    # system it was built on (tools/release.py checks it: 2.35, Ubuntu 22.04) - not that system's libstdc++
    target_link_options(VCSLauncher PRIVATE $<$<CONFIG:Release>:-s> -Wl,--as-needed -static-libstdc++ -static-libgcc)
    set_property(TARGET VCSLauncher PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# the version (VERSION in the repository root, the one number of a release) in the exe's version information
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VCS_ROOT}/VERSION")
file(STRINGS "${VCS_ROOT}/VERSION" VCS_VERSION LIMIT_COUNT 1)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _vcs_version_numbers "${VCS_VERSION}")
if(NOT _vcs_version_numbers)
    message(FATAL_ERROR "VERSION must start with major.minor.patch (it holds '${VCS_VERSION}')")
endif()
target_compile_definitions(VCSLauncher PRIVATE VCS_VERSION_MAJOR=${CMAKE_MATCH_1} VCS_VERSION_MINOR=${CMAKE_MATCH_2}
                           VCS_VERSION_PATCH=${CMAKE_MATCH_3})
# the text from a header: a quoted define on the command line loses its quotes in llvm-windres (llvm-mingw)
file(CONFIGURE OUTPUT "${VCS_LAUNCHER_SHADERS}/launcher_version.h" CONTENT "#define VCS_VERSION_TEXT \"${VCS_VERSION}\"
")

target_include_directories(VCSLauncher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${VCS_ROOT}/src" "${VCS_LAUNCHER_SHADERS}")
# next to tools/ and the source, where a release has it (the launcher finds the repository from its own folder)
set_target_properties(VCSLauncher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${VCS_ROOT}")
if(MSVC)
    # one self-contained exe: the static C++ runtime, nothing to install
    set_property(TARGET VCSLauncher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(VCSLauncher PRIVATE /W3 /utf-8)
elseif(MINGW)
    # llvm-mingw: the same - libc++, libunwind and winpthread linked in, only the system's UCRT left (tools/release.py
    # checks the imports); wWinMain needs -municode
    target_compile_options(VCSLauncher PRIVATE -Wall -Wno-missing-field-initializers -Wno-unknown-pragmas)
    target_link_options(VCSLauncher PRIVATE -municode -static $<$<CONFIG:Release>:-s>)
endif()
