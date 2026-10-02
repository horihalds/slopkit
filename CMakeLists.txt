cmake_minimum_required(VERSION 3.20)
project(slopkit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
# The only third-party source that is fetched and pinned to a commit hash is
# Dear ImGui. Zydis, GLFW and OpenGL come from the system packages (see below).

include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG f1cc2ae15e53a861a874c3034aae6798fde194ab)

FetchContent_MakeAvailable(imgui)

# Dear ImGui ships no CMakeLists.txt, so FetchContent only populates its
# sources. Expose the backend-agnostic core as a library target.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)

# --- system dependencies ---------------------------------------------------
#
# These come from the system repositories (e.g. Fedora `zydis-devel` and
# `glfw-devel`) and ship pkg-config files but no CMake package configs.

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZYDIS REQUIRED IMPORTED_TARGET zydis)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
pkg_check_modules(OPENGL REQUIRED IMPORTED_TARGET gl)

# GLFW + OpenGL3 backends, compiled against the system GLFW.
add_library(imgui_backends STATIC
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_link_libraries(imgui_backends PUBLIC imgui PkgConfig::GLFW)

# --- assets ----------------------------------------------------------------

include(${CMAKE_SOURCE_DIR}/cmake/EmbedFont.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/EmbedIcon.cmake)

# The icon PNG set and the embedded window icon are derived from data/icon.svg
# at configure time, so ImageMagick is a hard build-time dependency.
find_program(SLOPKIT_MAGICK_EXECUTABLE NAMES magick convert)
if(NOT SLOPKIT_MAGICK_EXECUTABLE)
    message(FATAL_ERROR
        "ImageMagick is required to generate the slopkit icon from data/icon.svg, "
        "but neither `magick` nor `convert` was found on PATH. "
        "Install it (e.g. Fedora: `sudo dnf install ImageMagick`; "
        "Debian/Ubuntu: `sudo apt install imagemagick`) and re-run configure.")
endif()

# The fonts are embedded at configure time; consumers get the generated include
# directory and the `slopkit::generated::*` symbols through this target.
add_library(slopkit_assets INTERFACE)
embed_font(slopkit_assets ${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf slopkit_notosans_regular)
embed_font(slopkit_assets ${CMAKE_SOURCE_DIR}/assets/fonts/NotoSansMono-Regular.ttf slopkit_notosansmono_regular)

# Hicolor PNG set for the desktop entry, plus the 64x64 RGBA blob used as the
# X11 window icon. Both share the `slopkit_assets` include directory.
set(SLOPKIT_ICON_SIZES 16 24 32 48 64 128 256 512)
slopkit_icon_pngs(${CMAKE_SOURCE_DIR}/data/icon.svg SLOPKIT_ICON_PNGS SIZES ${SLOPKIT_ICON_SIZES})
embed_window_icon(slopkit_assets ${CMAKE_SOURCE_DIR}/data/icon.svg 64 slopkit_icon_rgba)

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
    imgui_backends
    PkgConfig::OPENGL
    PkgConfig::ZYDIS
    ${CMAKE_DL_LIBS})

# --- platform library ------------------------------------------------------

# Linux helpers shared by the plugins and exercised directly by the tests.
file(GLOB_RECURSE SLOPKIT_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/platform/*.cpp)
add_library(slopkit_platform STATIC ${SLOPKIT_PLATFORM_SOURCES})
target_include_directories(slopkit_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(slopkit_platform PRIVATE -Wall -Wextra -fvisibility=hidden)
set_target_properties(slopkit_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The Process List classifies applications through the platform desktop-entry
# index, so the application links the shared platform library too.
target_link_libraries(slopkit PRIVATE slopkit_platform)

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
endfunction()

add_slopkit_test_plugin(slopkit_test_bad_abi ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/bad_abi_plugin.cpp)
add_slopkit_test_plugin(slopkit_test_no_entry ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/no_entry_plugin.cpp)
add_slopkit_test_plugin(slopkit_test_missing_regions ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/fixtures/missing_regions_plugin.cpp)

# --- formatting ------------------------------------------------------------

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE SLOPKIT_FORMAT_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/src/*.hpp
        ${CMAKE_SOURCE_DIR}/src/*.cpp)
    add_test(NAME clang-format-check
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${SLOPKIT_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# Application modules (everything but the entry point) are compiled into the
# test binary so tests can exercise them.
set(SLOPKIT_TEST_SUPPORT_SOURCES ${SLOPKIT_SOURCES})
list(FILTER SLOPKIT_TEST_SUPPORT_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

file(GLOB SLOPKIT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/tests/*.cpp)
add_executable(slopkit_tests ${SLOPKIT_TEST_SOURCES} ${SLOPKIT_TEST_SUPPORT_SOURCES})
add_dependencies(slopkit_tests slopkit-linux_proc slopkit-wine_proton slopkit_test_bad_abi slopkit_test_no_entry slopkit_test_missing_regions)
target_include_directories(slopkit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(slopkit_tests PRIVATE
    SLOPKIT_PLUGIN_DIR="${SLOPKIT_PLUGIN_OUTPUT_DIR}"
    SLOPKIT_PLUGIN_RELATIVE_DIR="${SLOPKIT_PLUGIN_RELATIVE_DIR}"
    SLOPKIT_TEST_PLUGIN_DIR="${SLOPKIT_TEST_PLUGIN_OUTPUT_DIR}")
target_link_libraries(slopkit_tests PRIVATE
    Catch2::Catch2
    slopkit_platform
    slopkit_assets
    imgui_backends
    PkgConfig::OPENGL
    PkgConfig::ZYDIS
    ${CMAKE_DL_LIBS})
target_compile_options(slopkit_tests PRIVATE -Wall -Wextra)

include(Catch)
catch_discover_tests(slopkit_tests)

# --- install ---------------------------------------------------------------

# The desktop entry bakes the absolute installed binary path; `install.sh`
# therefore passes the prefix at configure time.
configure_file(${CMAKE_SOURCE_DIR}/data/slopkit.desktop.in
    ${CMAKE_BINARY_DIR}/slopkit.desktop @ONLY)

install(TARGETS slopkit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Plugins go under the library dir, where the installed binary looks for them
# (see SLOPKIT_PLUGIN_RELATIVE_DIR above).
install(TARGETS slopkit-linux_proc slopkit-wine_proton
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/slopkit/plugins)

install(FILES ${CMAKE_BINARY_DIR}/slopkit.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

foreach(size IN LISTS SLOPKIT_ICON_SIZES)
    install(FILES ${CMAKE_BINARY_DIR}/icons/${size}x${size}/apps/slopkit.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${size}x${size}/apps)
endforeach()

# The vector is installed as well, so SVG-aware desktops get a crisp icon.
install(FILES ${CMAKE_SOURCE_DIR}/data/icon.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
    RENAME slopkit.svg)

install(FILES ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/assets/fonts/OFL.txt
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
