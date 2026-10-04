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

add_library(slopkit_native INTERFACE)
if(SLOPKIT_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slopkit_native INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-march=native;-mtune=native>)
endif()

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

# --- application -----------------------------------------------------------

# Plugin sources are excluded from the application binary: every plugin defines
# `slopkit_plugin_entry`, so linking two of them into one binary would collide.
# Platform code is shared with the plugins through the slopkit_platform target.
file(GLOB_RECURSE SLOPKIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER SLOPKIT_SOURCES EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/tests/.*")
list(FILTER SLOPKIT_SOURCES EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/plugins/.*")
list(FILTER SLOPKIT_SOURCES EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/platform/.*")

add_executable(slopkit ${SLOPKIT_SOURCES})
target_include_directories(slopkit PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(slopkit PRIVATE SLOPKIT_PLUGIN_RELATIVE_DIR="${SLOPKIT_PLUGIN_RELATIVE_DIR}")
target_compile_options(slopkit PRIVATE -Wall -Wextra)
target_link_libraries(slopkit PRIVATE
    slopkit_assets
    slopkit_native
    Qt6::Widgets
    PkgConfig::ZYDIS
    ${CMAKE_DL_LIBS})

# Every generated icon size is embedded as :/icons/<N>x<N>/apps/slopkit.png,
# the action glyphs as :/icons/actions/<N>x<N>/<name>.png.
qt_add_resources(slopkit "slopkit_icons"
    PREFIX "/icons"
    BASE "${CMAKE_BINARY_DIR}/icons"
    FILES ${SLOPKIT_ICON_PNGS} ${SLOPKIT_ACTION_ICON_PNGS})

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
target_link_libraries(slopkit-linux_proc PRIVATE slopkit_platform)

add_slopkit_plugin(slopkit-wine_proton ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/wine_proton/wine_proton_plugin.cpp)
target_link_libraries(slopkit-wine_proton PRIVATE slopkit_platform)

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

add_slopkit_test_plugin(slopkit_test_bad_abi ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/bad_abi_plugin.cpp)
add_slopkit_test_plugin(slopkit_test_no_entry ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/no_entry_plugin.cpp)
add_slopkit_test_plugin(slopkit_test_missing_regions ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/missing_regions_plugin.cpp)
add_slopkit_test_plugin(slopkit_test_old_abi_minor ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/old_abi_minor_plugin.cpp)

# The logging fixture is a valid plugin, so it lives in its own directory: the
# loader tests above require the broken-plugin directory to yield no plugins.
set(SLOPKIT_TEST_LOGGING_PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/logging_test_plugins)
add_slopkit_test_plugin(slopkit_test_logging ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/logging_plugin.cpp)
set_target_properties(slopkit_test_logging PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${SLOPKIT_TEST_LOGGING_PLUGIN_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${SLOPKIT_TEST_LOGGING_PLUGIN_OUTPUT_DIR})

# --- formatting ------------------------------------------------------------

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE SLOPKIT_FORMAT_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/src/*.hpp
        ${CMAKE_SOURCE_DIR}/src/*.cpp)
    add_test(NAME clang-format-check
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${SLOPKIT_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # A pathological hang must fail fast instead of blocking the whole run.
    set_tests_properties(clang-format-check PROPERTIES TIMEOUT 60)
endif()

# Application modules (everything but the entry point) are compiled into the
# test binary so tests can exercise them.
set(SLOPKIT_TEST_SUPPORT_SOURCES ${SLOPKIT_SOURCES})
list(FILTER SLOPKIT_TEST_SUPPORT_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

file(GLOB SLOPKIT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/tests/*.cpp)
add_executable(slopkit_tests ${SLOPKIT_TEST_SOURCES} ${SLOPKIT_TEST_SUPPORT_SOURCES})
# The tests load the bundled plugins the same way the app does, so they need the
# same exported symbols for the platform library's diagnostics.
set_target_properties(slopkit_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(slopkit_tests slopkit-linux_proc slopkit-wine_proton slopkit_test_bad_abi slopkit_test_no_entry slopkit_test_missing_regions slopkit_test_old_abi_minor slopkit_test_logging)
target_include_directories(slopkit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(slopkit_tests PRIVATE
    SLOPKIT_PLUGIN_DIR="${SLOPKIT_PLUGIN_OUTPUT_DIR}"
    SLOPKIT_PLUGIN_RELATIVE_DIR="${SLOPKIT_PLUGIN_RELATIVE_DIR}"
    SLOPKIT_TEST_PLUGIN_DIR="${SLOPKIT_TEST_PLUGIN_OUTPUT_DIR}"
    SLOPKIT_TEST_LOGGING_PLUGIN_DIR="${SLOPKIT_TEST_LOGGING_PLUGIN_OUTPUT_DIR}"
    SLOPKIT_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
    SLOPKIT_TMP_DIR="${CMAKE_SOURCE_DIR}/tmp")
target_link_libraries(slopkit_tests PRIVATE
    Catch2::Catch2
    slopkit_platform
    slopkit_assets
    slopkit_native
    Qt6::Widgets
    PkgConfig::ZYDIS
    ${CMAKE_DL_LIBS})
qt_add_resources(slopkit_tests "slopkit_icons"
    PREFIX "/icons"
    BASE "${CMAKE_BINARY_DIR}/icons"
    FILES ${SLOPKIT_ICON_PNGS} ${SLOPKIT_ACTION_ICON_PNGS})
target_compile_options(slopkit_tests PRIVATE -Wall -Wextra)

include(Catch)
# Widget tests run headless through the offscreen QPA platform. A pathological
# hang must fail within a minute (the slowest legitimate case is ~2s) instead of
# blocking the whole suite.
catch_discover_tests(slopkit_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

# --- install ---------------------------------------------------------------

# The desktop entry bakes the absolute installed binary path; `install.sh`
# therefore passes the prefix at configure time.
configure_file(${CMAKE_SOURCE_DIR}/assets/slopkit.desktop.in
    ${CMAKE_BINARY_DIR}/slopkit.desktop @ONLY)

install(TARGETS slopkit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Plugins go under the library dir, where the installed binary looks for them
# (see SLOPKIT_PLUGIN_RELATIVE_DIR above).
install(TARGETS slopkit-linux_proc slopkit-wine_proton
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/slopkit/plugins)

install(FILES ${CMAKE_BINARY_DIR}/slopkit.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

# The `.skt` MIME type is registered through shared-mime-info; `install.sh`
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
