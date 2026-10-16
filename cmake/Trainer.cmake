# --- Windows trainer template (cross-compiled PE executable) ---------------
#
# `Generate Trainer…` copies an installed runtime template and appends the table's
# payload to it, so the Windows target needs the trainer as a Windows binary. The
# sources are the same ones the Linux `slopkit-trainer` target builds; only the
# target backend differs (`src/trainer/runtime/target_windows.cpp`), and the C++
# runtime is linked statically so the result runs on a machine with nothing but
# the operating system installed.
#
# A missing `x86_64-w64-mingw32-g++` never fails the configure: it warns, no
# template is produced, and `trainer::generate` then reports the windows target on
# its own while the Linux one still works.
#
# Expected to be included after `SLOPKIT_TRAINER_DIR` and `SLOPKIT_TRAINER_SOURCES`
# are defined (see CMakeLists.txt).

find_program(SLOPKIT_MINGW_X86_64_CXX_EXECUTABLE NAMES x86_64-w64-mingw32-g++)

set(SLOPKIT_TRAINER_WINDOWS_SOURCES ${SLOPKIT_TRAINER_SOURCES})
list(REMOVE_ITEM SLOPKIT_TRAINER_WINDOWS_SOURCES
    ${CMAKE_SOURCE_DIR}/src/trainer/runtime/target_linux.cpp)
list(APPEND SLOPKIT_TRAINER_WINDOWS_SOURCES
    ${CMAKE_SOURCE_DIR}/src/trainer/runtime/target_windows.cpp)

set(SLOPKIT_TRAINER_WINDOWS_HEADERS
    ${CMAKE_SOURCE_DIR}/src/trainer/payload.hpp
    ${CMAKE_SOURCE_DIR}/src/trainer/runtime/target.hpp
    ${CMAKE_SOURCE_DIR}/src/trainer/runtime/payload_reader.hpp
    ${CMAKE_SOURCE_DIR}/src/trainer/runtime/resolve.hpp
    ${CMAKE_SOURCE_DIR}/src/trainer/runtime/console.hpp)

set(SLOPKIT_TRAINER_WINDOWS_TEMPLATE ${SLOPKIT_TRAINER_DIR}/slopkit-trainer.exe)

# Static libstdc++/libgcc and the console subsystem: no mingw DLL may be needed on
# the target machine, and the trainer is a console program by definition.
set(SLOPKIT_TRAINER_WINDOWS_FLAGS
    -std=c++23 -O2 -Wall -Wextra
    -static -static-libgcc -static-libstdc++
    -Wl,--subsystem,console)

if(SLOPKIT_MINGW_X86_64_CXX_EXECUTABLE)
    add_custom_command(OUTPUT ${SLOPKIT_TRAINER_WINDOWS_TEMPLATE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SLOPKIT_TRAINER_DIR}
        COMMAND ${SLOPKIT_MINGW_X86_64_CXX_EXECUTABLE}
            ${SLOPKIT_TRAINER_WINDOWS_FLAGS}
            -I${CMAKE_SOURCE_DIR}/src
            -o ${SLOPKIT_TRAINER_WINDOWS_TEMPLATE}
            ${SLOPKIT_TRAINER_WINDOWS_SOURCES}
            -lpsapi
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
