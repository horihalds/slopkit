# --- third-party dependencies ----------------------------------------------
#
# sol2 and Zydis are fetched from their upstream repositories at pinned
# revisions and Lua from the Lua team's official release tarball, which is pinned
# by its SHA-256, instead of coming from the system packages, so the application,
# the native trainer and the cross-compiled Windows template all link identical
# (and, for the trainer's target machine, static) copies and a script cannot
# behave differently between them.
#
# A nested configure (the mingw cross-build in cmake/TrainerTemplate.cmake) is
# handed the host build's download directory through `SLOPKIT_SHARED_SOURCE_DIR`,
# so each dependency is downloaded once. Only the source trees are shared: each
# build keeps its own binary directories, because object files compiled by one
# toolchain must never be reused by the other.

include(FetchContent)

if(NOT SLOPKIT_FETCHCONTENT_BASE_DIR)
    set(SLOPKIT_FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
        "Directory the fetched dependencies are downloaded into")
endif()
set(FETCHCONTENT_BASE_DIR "${SLOPKIT_FETCHCONTENT_BASE_DIR}" CACHE PATH
    "Directory the fetched dependencies are downloaded into" FORCE)

# The official Lua release tarball is extracted with its `lua-5.4.7/` top-level
# directory stripped, so its sources (and `lua.h`) live under the download
# directory's `src/`. The path is computed on every configure instead of being
# cached: a tree configured before the switch away from the old git mirror still
# holds a cache entry for the checkout layout, and it must not be able to win.
set(SLOPKIT_LUA_SOURCE_DIR "${SLOPKIT_FETCHCONTENT_BASE_DIR}/lua-src/src")

# Reuse shared source trees when they are already present; a fresh tree downloads
# its own copies below.
if(SLOPKIT_SHARED_SOURCE_DIR)
    if(EXISTS "${SLOPKIT_SHARED_SOURCE_DIR}/sol2-src/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_SOL2 "${SLOPKIT_SHARED_SOURCE_DIR}/sol2-src" CACHE PATH "" FORCE)
    endif()
    if(EXISTS "${SLOPKIT_SHARED_SOURCE_DIR}/zydis-src/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_ZYDIS "${SLOPKIT_SHARED_SOURCE_DIR}/zydis-src" CACHE PATH "" FORCE)
    endif()
    if(EXISTS "${SLOPKIT_SHARED_SOURCE_DIR}/lua-src/src/lua.h")
        set(SLOPKIT_LUA_SOURCE_DIR "${SLOPKIT_SHARED_SOURCE_DIR}/lua-src/src")
    endif()
endif()

# sol2: the header-only Lua binding library (pinned to 3.5.0).
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG 9190880c593dfb018ccf5cc9729ab87739709862)

# Zydis: the x86-64 decoder/encoder behind the listing and `assemble`. Zycore is
# its submodule. Tools, examples, tests and docs are never part of slopkit, and
# leaving them on would drag third-party diagnostics into the warning-free build.
FetchContent_Declare(zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG a2278f1d254e492f6a6b39f6cb5d1f5d515659dc
    GIT_SUBMODULES "dependencies/zycore")

set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(sol2 zydis)

# Lua: the VM sol2 binds to. The Lua team publishes no CMake build, so the
# official 5.4.7 release tarball is downloaded, pinned by its SHA-256, and its
# sources are compiled into a static library of our own. An already-downloaded
# tarball is compiled as it is, without a second download.
if(NOT EXISTS "${SLOPKIT_LUA_SOURCE_DIR}/lua.h")
    FetchContent_Populate(lua
        SOURCE_DIR "${SLOPKIT_FETCHCONTENT_BASE_DIR}/lua-src"
        BINARY_DIR "${SLOPKIT_FETCHCONTENT_BASE_DIR}/lua-build"
        URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
        URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30)
endif()

file(GLOB SLOPKIT_LUA_SOURCES CONFIGURE_DEPENDS "${SLOPKIT_LUA_SOURCE_DIR}/*.c")
# `lua.c` and `luac.c` are the interpreter's and the compiler's entry points,
# `onelua.c` is upstream's one-file amalgamation of every source above, and
# `ltests.c` belongs to Lua's own test suite.
list(FILTER SLOPKIT_LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")

add_library(slopkit_lua STATIC ${SLOPKIT_LUA_SOURCES})
target_include_directories(slopkit_lua PUBLIC "${SLOPKIT_LUA_SOURCE_DIR}")
set_target_properties(slopkit_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT WIN32)
    # Match a distro Lua: dynamic loading plus the POSIX filesystem and locale
    # bits. Windows needs no extra define; luaconf.h detects it.
    target_compile_definitions(slopkit_lua PRIVATE LUA_USE_LINUX)
    target_link_libraries(slopkit_lua PUBLIC ${CMAKE_DL_LIBS})
endif()

# The fetched libraries are third-party: marking the targets SYSTEM keeps their
# diagnostics out of the `-Wall -Wextra` warning-free build stage. Zydis is also
# compiled position-independently because the bundled plugins link it into
# shared objects.
set_target_properties(Zydis Zycore sol2 PROPERTIES SYSTEM TRUE)
set_target_properties(Zydis Zycore PROPERTIES POSITION_INDEPENDENT_CODE ON)
