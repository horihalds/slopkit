# --- Windows trainer template (nested cross-build body) --------------------
#
# The body of the nested project whose top-level file is
# cmake/trainer-template/CMakeLists.txt. It builds the trainer runtime and the
# script engine into `slopkit-trainer.exe` under `SLOPKIT_TRAINER_OUTPUT_DIR`,
# linking the same fetched dependencies (cmake/Dependencies.cmake) and the same
# source list (cmake/TrainerSources.cmake) the host build uses.

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
# psapi backs the Windows target backend's module enumeration.
target_link_libraries(slopkit-trainer-template PRIVATE sol2::sol2 Zydis slopkit_lua ftxui::component psapi)
set_target_properties(slopkit-trainer-template PROPERTIES
    OUTPUT_NAME slopkit-trainer
    RUNTIME_OUTPUT_DIRECTORY "${SLOPKIT_TRAINER_OUTPUT_DIR}")
