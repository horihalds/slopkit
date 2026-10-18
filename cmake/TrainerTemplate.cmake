# --- Windows trainer template (nested cross-build body) --------------------
#
# The body of the nested project whose top-level file is
# cmake/trainer-template/CMakeLists.txt. It builds the trainer runtime, the
# script engine and the trainer's Qt window into `slopkit-trainer.exe` under
# `SLOPKIT_TRAINER_OUTPUT_DIR`, linking the same fetched dependencies
# (cmake/Dependencies.cmake), the same source lists (cmake/TrainerSources.cmake,
# cmake/UiCommonSources.cmake) and the same presentation library the host build
# uses.
#
# `CMAKE_PREFIX_PATH` must point at a statically linked mingw Qt 6 kit;
# cmake/Trainer.cmake only configures this project when it found one, because
# the template stays a single self-contained file with the payload appended.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
set(CMAKE_AUTOMOC ON)

include(${CMAKE_CURRENT_LIST_DIR}/Dependencies.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/EmbedFont.cmake)

set(SLOPKIT_SOURCE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
include(${CMAKE_CURRENT_LIST_DIR}/TrainerSources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/UiCommonSources.cmake)

# The trainer window reads the palette and the fonts through the shared
# presentation library, exactly like the app and the sandbox do. The two faces
# are embedded per build tree, so the generated headers live here.
add_library(slopkit_assets INTERFACE)
embed_font(slopkit_assets ${SLOPKIT_SOURCE_ROOT}/assets/fonts/NotoSans-Regular.ttf slopkit_notosans_regular)
embed_font(slopkit_assets ${SLOPKIT_SOURCE_ROOT}/assets/fonts/NotoSansMono-Regular.ttf slopkit_notosansmono_regular)

add_library(slopkit_ui_common STATIC ${SLOPKIT_UI_COMMON_SOURCES})
target_include_directories(slopkit_ui_common PUBLIC ${SLOPKIT_SOURCE_ROOT}/src)
target_compile_options(slopkit_ui_common PRIVATE -Wall -Wextra)
target_link_libraries(slopkit_ui_common
    PRIVATE slopkit_assets
    PUBLIC Qt6::Widgets)

add_executable(slopkit-trainer-template ${SLOPKIT_TRAINER_WINDOWS_SOURCES})
target_include_directories(slopkit-trainer-template PRIVATE ${SLOPKIT_SOURCE_ROOT}/src)
target_compile_options(slopkit-trainer-template PRIVATE -Wall -Wextra)
# psapi backs the Windows target backend's module enumeration.
target_link_libraries(slopkit-trainer-template PRIVATE
    slopkit_ui_common
    sol2::sol2
    Zydis
    slopkit_lua
    psapi)
set_target_properties(slopkit-trainer-template PROPERTIES
    OUTPUT_NAME slopkit-trainer
    RUNTIME_OUTPUT_DIRECTORY "${SLOPKIT_TRAINER_OUTPUT_DIR}")
