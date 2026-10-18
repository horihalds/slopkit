# --- Windows trainer template (nested cross-build) -------------------------
#
# Configured and built by cmake/Trainer.cmake with cmake/toolchain-mingw64.cmake.
# It is a separate CMake project because the template is a PE binary that must
# link the same fetched Lua, sol2 and Zydis revisions the host build uses, and a
# bare compiler command cannot consume the FetchContent targets. Its window is
# drawn with the Win32 API, so no Qt kit is involved. Both builds share the host
# build's download directory, so each repository is checked out once.
#
# The project body lives in cmake/TrainerTemplate.cmake; this file carries only
# the two commands CMake requires literally in the top-level file.

cmake_minimum_required(VERSION 3.20)
project(slopkit-trainer-template LANGUAGES C CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../TrainerTemplate.cmake)
