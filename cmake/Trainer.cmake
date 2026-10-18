# --- Windows trainer template (cross-compiled PE executable) ---------------
#
# `Generate Trainer…` copies an installed runtime template and appends the table's
# payload to it, so the Windows target needs the trainer as a Windows binary. The
# sources are the same ones the native `slopkit-trainer` target builds (see
# cmake/TrainerSources.cmake); the target backend and the window differ, and the
# fetched Lua, sol2 and Zydis libraries are cross-compiled by the nested project
# (cmake/TrainerTemplate.cmake) this rule configures and builds with the mingw
# toolchain (cmake/toolchain-mingw64.cmake).
#
# The template's window is drawn with the Win32 API, so the cross-build needs no
# Qt kit and the template stays one self-contained file with the payload
# appended, with no sidecar DLLs.
#
# A missing mingw compiler never fails the configure: it warns, no template is
# produced, and `trainer::generate` then reports the windows target on its own
# while the Linux one still works.
#
# Expected to be included after `SLOPKIT_TRAINER_DIR`, `SLOPKIT_TRAINER_SOURCES`
# and `SLOPKIT_FETCHCONTENT_BASE_DIR` are defined (see CMakeLists.txt).

find_program(SLOPKIT_MINGW_X86_64_C_EXECUTABLE NAMES x86_64-w64-mingw32-gcc)
find_program(SLOPKIT_MINGW_X86_64_CXX_EXECUTABLE NAMES x86_64-w64-mingw32-g++)

set(SLOPKIT_TRAINER_WINDOWS_TEMPLATE ${SLOPKIT_TRAINER_DIR}/slopkit-trainer.exe)
set(SLOPKIT_TRAINER_TEMPLATE_BUILD_DIR ${CMAKE_BINARY_DIR}/trainer-template)

file(GLOB_RECURSE SLOPKIT_TRAINER_WINDOWS_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/trainer/*.hpp
    ${CMAKE_SOURCE_DIR}/src/core/*.hpp
    ${CMAKE_SOURCE_DIR}/src/script/*.hpp
    ${CMAKE_SOURCE_DIR}/src/reclass/*.hpp
    ${CMAKE_SOURCE_DIR}/src/disasm/*.hpp
    ${CMAKE_SOURCE_DIR}/src/scan/*.hpp
    ${CMAKE_SOURCE_DIR}/src/expr/*.hpp)

if(SLOPKIT_MINGW_X86_64_C_EXECUTABLE AND SLOPKIT_MINGW_X86_64_CXX_EXECUTABLE)
    # The nested configure is a real CMake project because the template links the
    # fetched Lua, sol2 and Zydis libraries; it reuses the host build's downloads
    # through SLOPKIT_SHARED_SOURCE_DIR.
    add_custom_command(OUTPUT ${SLOPKIT_TRAINER_WINDOWS_TEMPLATE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SLOPKIT_TRAINER_DIR}
        COMMAND ${CMAKE_COMMAND}
            -S ${CMAKE_SOURCE_DIR}/cmake/trainer-template
            -B ${SLOPKIT_TRAINER_TEMPLATE_BUILD_DIR}
            -G "${CMAKE_GENERATOR}"
            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/cmake/toolchain-mingw64.cmake
            -DCMAKE_BUILD_TYPE=Release
            -DSLOPKIT_SHARED_SOURCE_DIR=${SLOPKIT_FETCHCONTENT_BASE_DIR}
            -DSLOPKIT_TRAINER_OUTPUT_DIR=${SLOPKIT_TRAINER_DIR}
        COMMAND ${CMAKE_COMMAND} --build ${SLOPKIT_TRAINER_TEMPLATE_BUILD_DIR} --parallel
        DEPENDS ${SLOPKIT_TRAINER_WINDOWS_SOURCES} ${SLOPKIT_TRAINER_WINDOWS_HEADERS}
        COMMENT "Cross-compiling the Windows trainer template"
        VERBATIM)

    # Part of the default build, so `tools/verify.sh` and `tools/install.sh` cover
    # it exactly like the Linux template.
    add_custom_target(slopkit-trainer-windows ALL DEPENDS ${SLOPKIT_TRAINER_WINDOWS_TEMPLATE})

    # Nothing strips a PE with the host objcopy, and the executable is already
    # statically linked, so it is installed as it is.
    install(FILES ${SLOPKIT_TRAINER_WINDOWS_TEMPLATE}
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/slopkit/trainer
        OPTIONAL)
else()
    message(WARNING
        "x86_64-w64-mingw32-g++ was not found: the Windows trainer template will not be built, "
        "and Generate Trainer will report the windows target as unavailable")
endif()
