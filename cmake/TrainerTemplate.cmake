# --- Windows trainer template (nested cross-build body) --------------------
#
# The body of the nested project whose top-level file is
# cmake/trainer-template/CMakeLists.txt. It builds the trainer runtime, the
# script engine and the native Win32 window into `slopkit-trainer.exe` under
# `SLOPKIT_TRAINER_OUTPUT_DIR`, linking the same fetched dependencies
# (cmake/Dependencies.cmake) and the same source lists
# (cmake/TrainerSources.cmake) the host build uses. No Qt is involved: the
# template stays a single self-contained file with the payload appended, so its
# window is drawn directly with the Win32 API.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/Dependencies.cmake)

set(SLOPKIT_SOURCE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
include(${CMAKE_CURRENT_LIST_DIR}/TrainerSources.cmake)

add_executable(slopkit-trainer-template ${SLOPKIT_TRAINER_WINDOWS_SOURCES})
target_include_directories(slopkit-trainer-template PRIVATE ${SLOPKIT_SOURCE_ROOT}/src)
target_compile_options(slopkit-trainer-template PRIVATE -Wall -Wextra)
# psapi backs the Windows target backend's module enumeration; comctl32, user32
# and gdi32 back the ListView, the menus and the custom-drawn check column.
target_link_libraries(slopkit-trainer-template PRIVATE
    sol2::sol2
    Zydis
    slopkit_lua
    psapi
    comctl32
    gdi32
    user32)
set_target_properties(slopkit-trainer-template PROPERTIES
    OUTPUT_NAME slopkit-trainer
    RUNTIME_OUTPUT_DIRECTORY "${SLOPKIT_TRAINER_OUTPUT_DIR}")
