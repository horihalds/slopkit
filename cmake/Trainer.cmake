# --- Windows trainer template (cross-compiled PE executable) ---------------
#
# `Generate Trainer…` copies an installed runtime template and appends the table's
# payload to it, so the Windows target needs the trainer as a Windows binary. The
# sources are the same ones the native `slopkit-trainer` target builds (see
# cmake/TrainerSources.cmake); only the target backend differs, and the fetched
# Lua, sol2 and Zydis libraries plus the shared presentation library are
# cross-compiled by the nested project (cmake/TrainerTemplate.cmake) this rule
# configures and builds with the mingw toolchain
# (cmake/toolchain-mingw64.cmake).
#
# The template is still one self-contained file with the payload appended, so it
# needs a statically linked mingw Qt 6 kit: a shared kit would require sidecar
# DLLs beside the trainer. Without such a kit the Windows target is reported
# unavailable, exactly as a missing compiler is.
#
# A missing mingw compiler or Qt kit never fails the configure: it warns, no
# template is produced, and `trainer::generate` then reports the windows target
# on its own while the Linux one still works.
#
# Expected to be included after `SLOPKIT_TRAINER_DIR`, `SLOPKIT_TRAINER_SOURCES`
# and `SLOPKIT_FETCHCONTENT_BASE_DIR` are defined (see CMakeLists.txt).

find_program(SLOPKIT_MINGW_X86_64_C_EXECUTABLE NAMES x86_64-w64-mingw32-gcc)
find_program(SLOPKIT_MINGW_X86_64_CXX_EXECUTABLE NAMES x86_64-w64-mingw32-g++)

set(SLOPKIT_MINGW_QT_DIR "" CACHE PATH
    "Statically linked mingw-w64 Qt 6 kit for the Windows trainer template")
set(SLOPKIT_MINGW_QT6_PREFIX "")

# The Qt 6 CMake package sits in `<prefix>/lib/cmake/Qt6`; a static kit has
# `libQt6Core.a` in the sibling `lib` directory. A shared kit is refused because
# it would break the template's one-file promise.
function(slopkit_probe_static_qt6 out_prefix)
    set(candidates "")
    if(SLOPKIT_MINGW_QT_DIR)
        list(APPEND candidates
            "${SLOPKIT_MINGW_QT_DIR}/lib/cmake/Qt6"
            "${SLOPKIT_MINGW_QT_DIR}/Qt6")
    else()
        list(APPEND candidates
            "/usr/x86_64-w64-mingw32/sys-root/mingw/lib/cmake/Qt6"
            "/usr/x86_64-w64-mingw32/lib/cmake/Qt6"
            "/usr/lib/x86_64-w64-mingw32/cmake/Qt6"
            "/opt/mingw64/lib/cmake/Qt6")
    endif()

    foreach(candidate IN LISTS candidates)
        if(EXISTS "${candidate}/Qt6Config.cmake")
            get_filename_component(prefix "${candidate}/../../.." ABSOLUTE)
            if(EXISTS "${prefix}/lib/libQt6Core.a")
                set(${out_prefix} "${prefix}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endforeach()
    set(${out_prefix} "" PARENT_SCOPE)
endfunction()

slopkit_probe_static_qt6(SLOPKIT_MINGW_QT6_PREFIX)

set(SLOPKIT_TRAINER_WINDOWS_TEMPLATE ${SLOPKIT_TRAINER_DIR}/slopkit-trainer.exe)
set(SLOPKIT_TRAINER_TEMPLATE_BUILD_DIR ${CMAKE_BINARY_DIR}/trainer-template)

file(GLOB_RECURSE SLOPKIT_TRAINER_WINDOWS_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/trainer/*.hpp
    ${CMAKE_SOURCE_DIR}/src/ui/*.hpp
    ${CMAKE_SOURCE_DIR}/src/core/*.hpp
    ${CMAKE_SOURCE_DIR}/src/script/*.hpp
    ${CMAKE_SOURCE_DIR}/src/reclass/*.hpp
    ${CMAKE_SOURCE_DIR}/src/disasm/*.hpp
    ${CMAKE_SOURCE_DIR}/src/scan/*.hpp
    ${CMAKE_SOURCE_DIR}/src/expr/*.hpp)

if(SLOPKIT_MINGW_X86_64_C_EXECUTABLE AND SLOPKIT_MINGW_X86_64_CXX_EXECUTABLE AND SLOPKIT_MINGW_QT6_PREFIX)
    # The nested configure is a real CMake project because the template links the
    # fetched Lua, sol2 and Zydis libraries and Qt; it reuses the host build's
    # downloads through SLOPKIT_SHARED_SOURCE_DIR.
    add_custom_command(OUTPUT ${SLOPKIT_TRAINER_WINDOWS_TEMPLATE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SLOPKIT_TRAINER_DIR}
        COMMAND ${CMAKE_COMMAND}
            -S ${CMAKE_SOURCE_DIR}/cmake/trainer-template
            -B ${SLOPKIT_TRAINER_TEMPLATE_BUILD_DIR}
            -G "${CMAKE_GENERATOR}"
            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/cmake/toolchain-mingw64.cmake
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_PREFIX_PATH=${SLOPKIT_MINGW_QT6_PREFIX}
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
elseif(NOT (SLOPKIT_MINGW_X86_64_C_EXECUTABLE AND SLOPKIT_MINGW_X86_64_CXX_EXECUTABLE))
    message(WARNING
        "x86_64-w64-mingw32-g++ was not found: the Windows trainer template will not be built, "
        "and Generate Trainer will report the windows target as unavailable")
else()
    message(WARNING
        "no statically linked mingw-w64 Qt 6 kit was found (set SLOPKIT_MINGW_QT_DIR to a kit whose "
        "Qt6Config.cmake has libQt6Core.a beside it): the Windows trainer template will not be built, "
        "and Generate Trainer will report the windows target as unavailable")
endif()
