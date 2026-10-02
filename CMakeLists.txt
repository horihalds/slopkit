cmake_minimum_required(VERSION 3.20)
project(slopkit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- application -----------------------------------------------------------

file(GLOB_RECURSE SLOPKIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER SLOPKIT_SOURCES EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/tests/.*")

add_executable(slopkit ${SLOPKIT_SOURCES})
target_include_directories(slopkit PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(slopkit PRIVATE -Wall -Wextra)

# --- tests -----------------------------------------------------------------

find_package(Catch2 REQUIRED)
include(CTest)

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
target_include_directories(slopkit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(slopkit_tests PRIVATE Catch2::Catch2)
target_compile_options(slopkit_tests PRIVATE -Wall -Wextra)

include(Catch)
catch_discover_tests(slopkit_tests)
