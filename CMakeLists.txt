cmake_minimum_required(VERSION 3.20)
project(slopkit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build: without a build type CMake passes no -O flag at
# all, so every hot path (the scanner above all) runs unoptimized. An explicit
# -DCMAKE_BUILD_TYPE=... on the command line still wins, and multi-config
# generators pick their configuration at build time.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# GCC's -Wfree-nonheap-object (part of -Wall) mis-fires on libstdc++'s
# std::variant copy assignment once optimization kicks in (see GCC PR 106155);
# silence that known false positive so the optimized build stays warning-clean.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-free-nonheap-object)
endif()

# --- CPU tuning ------------------------------------------------------------
#
# Build for the host CPU's full instruction set so the compiler emits every
# modern instruction available (AVX2/AVX-512, BMI, FMA, ...), which matters
# most for the scanner's hot loops. `-DSLOPKIT_NATIVE=OFF` restores a portable
# build targeting the compiler's default baseline.
option(SLOPKIT_NATIVE "Build for the host CPU with -march/-mtune=native" ON)

# --- debug info ------------------------------------------------------------
#
# Default builds use RelWithDebInfo, so every artifact carries full DWARF and
# debug info dominates its on-disk size (~95%). Compressing the debug sections
# shrinks each one by roughly 60% while `readelf`/`gdb` read them transparently.
# zstd compresses best but needs a debugger whose BFD was built with zstd; zlib
# is the universal fallback, and a compiler that supports neither keeps today's
# uncompressed sections. `-DSLOPKIT_COMPRESS_DEBUG=OFF` restores them.
option(SLOPKIT_COMPRESS_DEBUG "Compress DWARF debug sections (-gz)" ON)

set(SLOPKIT_DEBUG_COMPRESS_FLAG "")
if(SLOPKIT_COMPRESS_DEBUG AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-gz=zstd" SLOPKIT_CXX_HAS_GZ_ZSTD)
    check_cxx_compiler_flag("-gz=zlib" SLOPKIT_CXX_HAS_GZ_ZLIB)
    if(SLOPKIT_CXX_HAS_GZ_ZSTD)
        set(SLOPKIT_DEBUG_COMPRESS_FLAG "-gz=zstd")
    elseif(SLOPKIT_CXX_HAS_GZ_ZLIB)
        set(SLOPKIT_DEBUG_COMPRESS_FLAG "-gz=zlib")
    endif()

    # A compiler accepts `-gz=zstd` even when the debugger's BFD was built
    # without zstd, and gdb then refuses to read the file at all. Keep zstd only
    # when the available debugger can actually read it, otherwise fall back to
    # zlib (or to no compression when even that is unavailable).
    if(SLOPKIT_DEBUG_COMPRESS_FLAG STREQUAL "-gz=zstd")
        find_program(SLOPKIT_GDB_EXECUTABLE NAMES gdb)
        if(SLOPKIT_GDB_EXECUTABLE)
            set(_slopkit_probe_src "${CMAKE_BINARY_DIR}/CMakeFiles/slopkit_debug_probe.cpp")
            set(_slopkit_probe_obj "${_slopkit_probe_src}.o")
            file(WRITE "${_slopkit_probe_src}" "int slopkit_debug_probe;\n")
            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" -g -gz=zstd -c "${_slopkit_probe_src}" -o "${_slopkit_probe_obj}"
                RESULT_VARIABLE _slopkit_probe_compiled
                OUTPUT_QUIET ERROR_QUIET)
            if(_slopkit_probe_compiled EQUAL 0)
                execute_process(
                    COMMAND "${SLOPKIT_GDB_EXECUTABLE}" -batch -ex "file ${_slopkit_probe_obj}" -ex quit
                    RESULT_VARIABLE _slopkit_probe_read
                    OUTPUT_QUIET ERROR_VARIABLE _slopkit_probe_err)
                if(_slopkit_probe_err MATCHES "zstd")
                    if(SLOPKIT_CXX_HAS_GZ_ZLIB)
                        set(SLOPKIT_DEBUG_COMPRESS_FLAG "-gz=zlib")
                    else()
                        set(SLOPKIT_DEBUG_COMPRESS_FLAG "")
                    endif()
                endif()
            endif()
            file(REMOVE "${_slopkit_probe_src}" "${_slopkit_probe_obj}")
        endif()
    endif()
endif()

# `slopkit_native` carries the flags every target shares. The compression flag
# is applied independently of `SLOPKIT_NATIVE`, so a portable build still gets
# compressed debug info.
add_library(slopkit_native INTERFACE)
if(SLOPKIT_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slopkit_native INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-march=native;-mtune=native>)
endif()
if(SLOPKIT_DEBUG_COMPRESS_FLAG)
    target_compile_options(slopkit_native INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:${SLOPKIT_DEBUG_COMPRESS_FLAG}>)
    # The linker defaults to uncompressed debug sections, so the flag must be on
    # the link line too or it would expand the compressed input sections again.
    target_link_options(slopkit_native INTERFACE ${SLOPKIT_DEBUG_COMPRESS_FLAG})
endif()

# --- install stripping -----------------------------------------------------
#
# `build/` keeps fully debuggable binaries; the install step shrinks them. When
# `SLOPKIT_STRIP_INSTALL` is ON and `objcopy` (which ships with binutils) is
# present, `slopkit_install_stripped` moves each installed file's debug info into
# a sibling `<name>.debug` sidecar and points the file at it, so the installed
# `slopkit` drops from ~28 MB to a few MB while `gdb` still resolves source lines
# through the sidecar. `-DSLOPKIT_STRIP_INSTALL=OFF` restores today's unstripped
# install.
option(SLOPKIT_STRIP_INSTALL "Strip installed binaries and install .debug sidecars" ON)
find_program(SLOPKIT_OBJCOPY_EXECUTABLE NAMES objcopy)
include(${CMAKE_SOURCE_DIR}/cmake/StripDebugInfo.cmake)

# --- install layout --------------------------------------------------------
#
# GNUInstallDirs drives every install destination and the relative plugin
# search path baked into the binary. Note `CMAKE_INSTALL_LIBDIR` is `lib64` on
# RPM-based distros, so it must never be assumed to be `lib`.

include(GNUInstallDirs)

# Plugins are installed under <prefix>/<libdir>/slopkit/plugins. From the
# executable in <prefix>/bin that is the relative path below; the loader and
# the install rule share it so `lib` vs `lib64` can never drift.
set(SLOPKIT_PLUGIN_RELATIVE_DIR "../${CMAKE_INSTALL_LIBDIR}/slopkit/plugins")

# --- dependencies ----------------------------------------------------------
#
# Qt6 Widgets is the UI toolkit, replacing the previous immediate-mode stack.
# Zydis still comes from the system packages (see below).

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Q_OBJECT classes live in headers next to their same-named .cpp, which is the
# layout AUTOMOC scans to generate the meta-object code.
set(CMAKE_AUTOMOC ON)

# --- system dependencies ---------------------------------------------------
#
# Zydis comes from the system repositories (e.g. Fedora `zydis-devel`) and ships
# a pkg-config file but no CMake package config.

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZYDIS REQUIRED IMPORTED_TARGET zydis)

# --- assets ----------------------------------------------------------------

include(${CMAKE_SOURCE_DIR}/cmake/EmbedFont.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/EmbedIcon.cmake)

# The icon PNG set and the embedded window icon are derived from
# assets/icons/icon.svg at configure time, so ImageMagick is a hard build-time
# dependency.
find_program(SLOPKIT_MAGICK_EXECUTABLE NAMES magick convert)
if(NOT SLOPKIT_MAGICK_EXECUTABLE)
    message(FATAL_ERROR
        "ImageMagick is required to generate the slopkit icon from "
        "assets/icons/icon.svg, but neither `magick` nor `convert` was found "
        "on PATH. "
        "Install it (e.g. Fedora: `sudo dnf install ImageMagick`; "
        "Debian/Ubuntu: `sudo apt install imagemagick`) and re-run configure.")
endif()

# The fonts are embedded at configure time; consumers get the generated include
# directory and the `slopkit::generated::*` symbols through this target.
add_library(slopkit_assets INTERFACE)
embed_font(slopkit_assets ${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf slopkit_notosans_regular)
embed_font(slopkit_assets ${CMAKE_SOURCE_DIR}/assets/fonts/NotoSansMono-Regular.ttf slopkit_notosansmono_regular)

# Hicolor PNG set for the desktop entry, embedded below as a Qt resource so
# `QIcon` gets every size crisp.
set(SLOPKIT_ICON_SIZES 16 24 32 48 64 128 256 512)
slopkit_icon_pngs(${CMAKE_SOURCE_DIR}/assets/icons/icon.svg SLOPKIT_ICON_PNGS SIZES ${SLOPKIT_ICON_SIZES})

# UI action glyphs: rendered with the same ImageMagick step and embedded (never
# installed) under :/icons/actions/<N>x<N>/<name>.png. The names mirror
# widgets::ActionIcon one-to-one; menu icons paint at 16 logical px, so the
# larger sizes only serve HiDPI/fractional scaling.
set(SLOPKIT_ACTION_ICON_SIZES 16 24 32 48 64)
set(SLOPKIT_ACTION_ICON_NAMES
    cancel checkmark chip diskette error file folder home
    info ok search settings target warning)
set(SLOPKIT_ACTION_ICON_PNGS "")
foreach(name IN LISTS SLOPKIT_ACTION_ICON_NAMES)
    slopkit_action_icon_pngs(${CMAKE_SOURCE_DIR}/assets/icons/${name}.svg pngs SIZES ${SLOPKIT_ACTION_ICON_SIZES})
    list(APPEND SLOPKIT_ACTION_ICON_PNGS ${pngs})
endforeach()

# Qt resources are registered per executable, so every binary that paints the
# slopkit icons calls this on itself instead of receiving them through a
# library.
function(slopkit_add_icon_resources target)
    qt_add_resources(${target} "slopkit_icons"
        PREFIX "/icons"
        BASE "${CMAKE_BINARY_DIR}/icons"
        FILES ${SLOPKIT_ICON_PNGS} ${SLOPKIT_ACTION_ICON_PNGS})
endfunction()

# --- shared UI library -----------------------------------------------------
#
# The reusable presentation pieces shared by slopkit and slopkit-sandbox:
# theme, fonts, settings and the widget components, plus the logger and the
# version string both binaries need. scan/process/plugin/table stay out, which
# is also what keeps the sandbox structurally incapable of target access.

set(SLOPKIT_UI_COMMON_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/log.cpp
    ${CMAKE_SOURCE_DIR}/src/core/version.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/fonts.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/settings.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/platform.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/components/elided_tooltip_delegate.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/components/input_box.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/components/message_box.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/components/widgets.cpp)

add_library(slopkit_ui_common STATIC ${SLOPKIT_UI_COMMON_SOURCES})
target_include_directories(slopkit_ui_common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(slopkit_ui_common PRIVATE -Wall -Wextra)
set_target_properties(slopkit_ui_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Only fonts.cpp needs the embedded faces, so the asset target stays private
# and its generated include directory does not leak to the consumers.
target_link_libraries(slopkit_ui_common
    PRIVATE slopkit_assets
    PUBLIC slopkit_native Qt6::Widgets)

# --- application -----------------------------------------------------------

# Plugin sources are excluded from the application binary: every plugin defines
# `slopkit_plugin_entry`, so linking two of them into one binary would collide.
# Platform code is shared with the plugins through the slopkit_platform target.
file(GLOB_RECURSE SLOPKIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER SLOPKIT_SOURCES EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/plugins/.*")
list(FILTER SLOPKIT_SOURCES EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/platform/.*")
# The sandbox is its own executable, never part of the app binary.
list(FILTER SLOPKIT_SOURCES EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/sandbox/.*")
# The shared UI library compiles these, so neither the app nor the tests may.
list(REMOVE_ITEM SLOPKIT_SOURCES ${SLOPKIT_UI_COMMON_SOURCES})

add_executable(slopkit ${SLOPKIT_SOURCES})
target_include_directories(slopkit PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(slopkit PRIVATE SLOPKIT_PLUGIN_RELATIVE_DIR="${SLOPKIT_PLUGIN_RELATIVE_DIR}")
target_compile_options(slopkit PRIVATE -Wall -Wextra)
target_link_libraries(slopkit PRIVATE
    slopkit_ui_common
    slopkit_assets
    slopkit_native
    Qt6::Widgets
    PkgConfig::ZYDIS
    ${CMAKE_DL_LIBS})

# Every generated icon size is embedded as :/icons/<N>x<N>/apps/slopkit.png,
# the action glyphs as :/icons/actions/<N>x<N>/<name>.png.
slopkit_add_icon_resources(slopkit)

# --- sandbox (practice target) ---------------------------------------------
#
# A second UI binary: the practice target slopkit launches from its Help menu.
# It links only the shared presentation library, never scan/process/plugin, so
# it is structurally incapable of doing target access.

set(SLOPKIT_SANDBOX_SOURCES
    ${CMAKE_SOURCE_DIR}/src/sandbox/main.cpp
    ${CMAKE_SOURCE_DIR}/src/sandbox/sandbox_values.cpp
    ${CMAKE_SOURCE_DIR}/src/sandbox/sandbox_window.cpp)

add_executable(slopkit-sandbox ${SLOPKIT_SANDBOX_SOURCES})
target_include_directories(slopkit-sandbox PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(slopkit-sandbox PRIVATE -Wall -Wextra)
target_link_libraries(slopkit-sandbox PRIVATE
    slopkit_ui_common
    slopkit_native
    Qt6::Widgets)
slopkit_add_icon_resources(slopkit-sandbox)

# --- platform library ------------------------------------------------------

# Linux helpers shared by the plugins and exercised directly by the tests.
file(GLOB_RECURSE SLOPKIT_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/platform/*.cpp)
add_library(slopkit_platform STATIC ${SLOPKIT_PLATFORM_SOURCES})
target_include_directories(slopkit_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(slopkit_platform PRIVATE -Wall -Wextra -fvisibility=hidden)
set_target_properties(slopkit_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(slopkit_platform PRIVATE slopkit_native)

# The Process List classifies applications through the platform desktop-entry
# index, so the application links the shared platform library too.
target_link_libraries(slopkit PRIVATE slopkit_platform)

# The bundled plugins link the static platform library statically, so its
# diagnostics reference `slopkit::log`; exporting the application's symbols lets
# a dlopen'ed plugin resolve them to this process's single logger.
set_target_properties(slopkit PROPERTIES ENABLE_EXPORTS ON)

# --- plugin support library ------------------------------------------------

# The ABI plumbing both bundled plugins share: the session set, the string
# arena, the error mapping, the vtable and all 21 entry functions. It is a static
# library so each plugin links its own copy; the loader dlopen's plugins with
# RTLD_LOCAL, so the per-plugin state (host services, string arena, session
# registry) stays private to each library instance and two plugins never share
# it. `entry<Profile>()` is the one-liner a plugin's entry point expands to.
file(GLOB_RECURSE SLOPKIT_PLUGIN_SUPPORT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/plugins/support/*.cpp)
add_library(slopkit_plugin_support STATIC ${SLOPKIT_PLUGIN_SUPPORT_SOURCES})
target_include_directories(slopkit_plugin_support PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(slopkit_plugin_support PRIVATE -Wall -Wextra -fvisibility=hidden)
set_target_properties(slopkit_plugin_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(slopkit_plugin_support PUBLIC slopkit_platform)

# --- plugins ---------------------------------------------------------------

set(SLOPKIT_PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/plugins)

# Builds a plugin as a shared library with hidden visibility and places it next
# to the executable, where the host discovers it in `${exe_dir}/plugins`.
function(add_slopkit_plugin target)
    add_library(${target} SHARED ${ARGN})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${target} PRIVATE -Wall -Wextra -fvisibility=hidden)
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${SLOPKIT_PLUGIN_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${SLOPKIT_PLUGIN_OUTPUT_DIR})
    target_link_libraries(${target} PRIVATE slopkit_native)
endfunction()

add_slopkit_plugin(slopkit-linux_proc ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/linux_proc/linux_proc_plugin.cpp)
target_link_libraries(slopkit-linux_proc PRIVATE slopkit_plugin_support)

add_slopkit_plugin(slopkit-wine_proton ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/wine_proton/wine_proton_plugin.cpp)
target_link_libraries(slopkit-wine_proton PRIVATE slopkit_plugin_support)

# --- tests -----------------------------------------------------------------

find_package(Catch2 REQUIRED)
include(CTest)

# Fixture plugins for the loader tests: one uses an incompatible ABI major, one
# exports no entry symbol. They are kept out of the application plugin
# directory so they never show up in a normal run.
set(SLOPKIT_TEST_PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test_plugins)

function(add_slopkit_test_plugin target)
    add_library(${target} SHARED ${ARGN})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${target} PRIVATE -Wall -Wextra -fvisibility=hidden)
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${SLOPKIT_TEST_PLUGIN_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${SLOPKIT_TEST_PLUGIN_OUTPUT_DIR})
    target_link_libraries(${target} PRIVATE slopkit_native)
endfunction()

add_slopkit_test_plugin(slopkit_test_bad_abi ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bad_abi_plugin.cpp)
add_slopkit_test_plugin(slopkit_test_no_entry ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/no_entry_plugin.cpp)
add_slopkit_test_plugin(slopkit_test_missing_regions ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/missing_regions_plugin.cpp)
add_slopkit_test_plugin(slopkit_test_old_abi_minor ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/old_abi_minor_plugin.cpp)

# The logging fixture is a valid plugin, so it lives in its own directory: the
# loader tests above require the broken-plugin directory to yield no plugins.
set(SLOPKIT_TEST_LOGGING_PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/logging_test_plugins)
add_slopkit_test_plugin(slopkit_test_logging ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/logging_plugin.cpp)
set_target_properties(slopkit_test_logging PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${SLOPKIT_TEST_LOGGING_PLUGIN_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${SLOPKIT_TEST_LOGGING_PLUGIN_OUTPUT_DIR})

# --- formatting ------------------------------------------------------------

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE SLOPKIT_FORMAT_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/src/*.hpp
        ${CMAKE_SOURCE_DIR}/src/*.cpp
        ${CMAKE_SOURCE_DIR}/tests/*.hpp
        ${CMAKE_SOURCE_DIR}/tests/*.cpp)
    add_test(NAME clang-format-check
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${SLOPKIT_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # A pathological hang must fail fast instead of blocking the whole run.
    set_tests_properties(clang-format-check PROPERTIES TIMEOUT 60)
endif()

# --- documentation ---------------------------------------------------------

# `tools/docs-check.sh` verifies that relative doc links and anchor citations
# resolve and that the markdown stays clean; see the script for the exact rules.
# It needs neither clang-format nor a compiler, only bash and git.
add_test(NAME docs-links-check
    COMMAND ${CMAKE_SOURCE_DIR}/tools/docs-check.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(docs-links-check PROPERTIES TIMEOUT 60)

# Application modules (everything but the entry point) are compiled into the
# test binary so tests can exercise them.
set(SLOPKIT_TEST_SUPPORT_SOURCES ${SLOPKIT_SOURCES})
list(FILTER SLOPKIT_TEST_SUPPORT_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

# The sandbox's non-entry-point sources are compiled into the test binary too,
# so its value layout, its window and the real-engine self-scan are exercised
# in-process; `main.cpp` stays out because the tests own the entry point.
set(SLOPKIT_TEST_SANDBOX_SOURCES
    ${CMAKE_SOURCE_DIR}/src/sandbox/sandbox_values.cpp
    ${CMAKE_SOURCE_DIR}/src/sandbox/sandbox_window.cpp)

# Every test source is picked up, but never the fixture plugin sources: each of
# them defines `slopkit_plugin_entry`, so they are built as their own libraries.
file(GLOB_RECURSE SLOPKIT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
list(FILTER SLOPKIT_TEST_SOURCES EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/tests/fixtures/.*")
add_executable(slopkit_tests ${SLOPKIT_TEST_SOURCES} ${SLOPKIT_TEST_SUPPORT_SOURCES} ${SLOPKIT_TEST_SANDBOX_SOURCES})
# The tests load the bundled plugins the same way the app does, so they need the
# same exported symbols for the platform library's diagnostics.
set_target_properties(slopkit_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(slopkit_tests slopkit-linux_proc slopkit-wine_proton slopkit_test_bad_abi slopkit_test_no_entry slopkit_test_missing_regions slopkit_test_old_abi_minor slopkit_test_logging)
target_include_directories(slopkit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(slopkit_tests PRIVATE
    SLOPKIT_PLUGIN_DIR="${SLOPKIT_PLUGIN_OUTPUT_DIR}"
    SLOPKIT_PLUGIN_RELATIVE_DIR="${SLOPKIT_PLUGIN_RELATIVE_DIR}"
    SLOPKIT_TEST_PLUGIN_DIR="${SLOPKIT_TEST_PLUGIN_OUTPUT_DIR}"
    SLOPKIT_TEST_LOGGING_PLUGIN_DIR="${SLOPKIT_TEST_LOGGING_PLUGIN_OUTPUT_DIR}"
    SLOPKIT_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
    SLOPKIT_TMP_DIR="${CMAKE_SOURCE_DIR}/tmp")
target_link_libraries(slopkit_tests PRIVATE
    Catch2::Catch2
    slopkit_ui_common
    slopkit_platform
    slopkit_assets
    slopkit_native
    Qt6::Widgets
    PkgConfig::ZYDIS
    ${CMAKE_DL_LIBS})
slopkit_add_icon_resources(slopkit_tests)
target_compile_options(slopkit_tests PRIVATE -Wall -Wextra)

include(Catch)
# Widget tests run headless through the offscreen QPA platform. A pathological
# hang must fail within a minute (the slowest legitimate case is ~2s) instead of
# blocking the whole suite.
catch_discover_tests(slopkit_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

# --- install ---------------------------------------------------------------

# The desktop entry bakes the absolute installed binary path; `tools/install.sh`
# therefore passes the prefix at configure time.
configure_file(${CMAKE_SOURCE_DIR}/assets/slopkit.desktop.in
    ${CMAKE_BINARY_DIR}/slopkit.desktop @ONLY)

# `slopkit_install_stripped` drops each installed file's debug info into a
# sibling `.debug` sidecar; see cmake/StripDebugInfo.cmake and
# `SLOPKIT_STRIP_INSTALL`.
slopkit_install_stripped(slopkit ${CMAKE_INSTALL_BINDIR} RUNTIME)

# The practice target lands beside slopkit so the Help action resolves it as a
# sibling binary.
slopkit_install_stripped(slopkit-sandbox ${CMAKE_INSTALL_BINDIR} RUNTIME)

# Plugins go under the library dir, where the installed binary looks for them
# (see SLOPKIT_PLUGIN_RELATIVE_DIR above).
slopkit_install_stripped(slopkit-linux_proc ${CMAKE_INSTALL_LIBDIR}/slopkit/plugins LIBRARY)
slopkit_install_stripped(slopkit-wine_proton ${CMAKE_INSTALL_LIBDIR}/slopkit/plugins LIBRARY)

install(FILES ${CMAKE_BINARY_DIR}/slopkit.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

# The `.skt` MIME type is registered through shared-mime-info; `tools/install.sh`
# refreshes the database afterwards.
install(FILES ${CMAKE_SOURCE_DIR}/assets/application-x-slopkit-table.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/mime/packages)

foreach(size IN LISTS SLOPKIT_ICON_SIZES)
    install(FILES ${CMAKE_BINARY_DIR}/icons/${size}x${size}/apps/slopkit.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${size}x${size}/apps)
endforeach()

# The `.skt` MIME type reuses the application icon: desktops resolve the icon
# name `application-x-slopkit-table` through the same hicolor tree.
foreach(size IN LISTS SLOPKIT_ICON_SIZES)
    install(FILES ${CMAKE_BINARY_DIR}/icons/${size}x${size}/apps/slopkit.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${size}x${size}/mimetypes
        RENAME application-x-slopkit-table.png)
endforeach()

# The vector is installed as well, so SVG-aware desktops get a crisp icon.
install(FILES ${CMAKE_SOURCE_DIR}/assets/icons/icon.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
    RENAME slopkit.svg)

install(FILES ${CMAKE_SOURCE_DIR}/assets/icons/icon.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/mimetypes
    RENAME application-x-slopkit-table.svg)

install(FILES ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/assets/fonts/OFL.txt
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

# The rest of the documentation ships next to the README, so the relative
# `docs/...` links in the installed README resolve.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs
    FILES_MATCHING PATTERN "*.md")
