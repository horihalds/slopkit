# --- Wine resident agent (cross-compiled PE artifacts) ---------------------
#
# The agent DLL and its injector are Windows PE binaries that run *inside* the
# target (a Wine process), so they are cross-compiled with mingw-w64 — one pair
# per bitness — and placed next to the plugins, where the wine-proton plugin
# looks for them at runtime. A missing toolchain falls back to a prebuilt pair
# under `SLOPKIT_AGENT_PREBUILT_DIR/<bitness>`; with neither available the
# `slopkit-agent` target fails with an actionable message instead of silently
# leaving the plugin without an agent.
#
# A third artifact, `slopkit-probe-<bitness>.exe`, is a trivial console target
# the wine-gated end-to-end test drives. It is built into `agent_probe/` and is
# never installed.
#
# Expected to be included after `SLOPKIT_PLUGIN_OUTPUT_DIR` is defined.

set(SLOPKIT_AGENT_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/plugins/wine_proton/agent)
set(SLOPKIT_AGENT_PROTOCOL ${SLOPKIT_AGENT_SOURCE_DIR}/slopkit_agent_protocol.h)
set(SLOPKIT_AGENT_PREBUILT_DIR ${SLOPKIT_AGENT_SOURCE_DIR}/prebuilt CACHE PATH
    "Directory holding a prebuilt slopkit-agent.dll/slopkit-injector.exe per bitness")
set(SLOPKIT_AGENT_PROBE_DIR ${CMAKE_BINARY_DIR}/agent_probe)

find_program(SLOPKIT_MINGW_X86_64_EXECUTABLE NAMES x86_64-w64-mingw32-gcc)
find_program(SLOPKIT_MINGW_I686_EXECUTABLE NAMES i686-w64-mingw32-gcc)

# The CRT-free build: -nostdlib keeps each artifact down to its kernel32
# imports, -lgcc adds only the compiler helpers (stack probes, 64-bit division)
# and -fno-builtin keeps the memset/memcpy the sources define themselves.
set(SLOPKIT_AGENT_COMPILE_FLAGS -std=c11 -O2 -Wall -Wextra -Werror -fno-builtin -nostdlib)

set(SLOPKIT_AGENT_ARTIFACTS "")
set(SLOPKIT_AGENT_PROBES "")

foreach(bitness x86_64 i686)
    if(bitness STREQUAL "x86_64")
        set(_agent_compiler ${SLOPKIT_MINGW_X86_64_EXECUTABLE})
        set(_agent_underscore "")
    else()
        set(_agent_compiler ${SLOPKIT_MINGW_I686_EXECUTABLE})
        set(_agent_underscore "_")
    endif()
    # stdcall would decorate these entry points with @12; each is pinned to a
    # plain symbol with an asm label in its source, hence the leading underscore
    # the i686 linker then expects.
    set(_agent_dll_entry ${_agent_underscore}slopkit_agent_entry)
    set(_agent_injector_entry ${_agent_underscore}slopkit_injector_entry)
    set(_agent_probe_entry ${_agent_underscore}slopkit_probe_entry)

    set(_agent_dll ${SLOPKIT_PLUGIN_OUTPUT_DIR}/slopkit-agent-${bitness}.dll)
    set(_agent_injector ${SLOPKIT_PLUGIN_OUTPUT_DIR}/slopkit-injector-${bitness}.exe)
    set(_agent_probe ${SLOPKIT_AGENT_PROBE_DIR}/slopkit-probe-${bitness}.exe)
    list(APPEND SLOPKIT_AGENT_ARTIFACTS ${_agent_dll} ${_agent_injector})

    if(_agent_compiler)
        add_custom_command(OUTPUT ${_agent_dll}
            COMMAND ${_agent_compiler} ${SLOPKIT_AGENT_COMPILE_FLAGS} -shared -o ${_agent_dll}
                ${SLOPKIT_AGENT_SOURCE_DIR}/slopkit_agent.c
                ${SLOPKIT_AGENT_SOURCE_DIR}/slopkit_debug.c
                -lkernel32 -lgcc -Wl,--entry,${_agent_dll_entry} -Wl,--subsystem,windows
            DEPENDS ${SLOPKIT_AGENT_SOURCE_DIR}/slopkit_agent.c
                ${SLOPKIT_AGENT_SOURCE_DIR}/slopkit_debug.c
                ${SLOPKIT_AGENT_SOURCE_DIR}/slopkit_debug.h
                ${SLOPKIT_AGENT_PROTOCOL}
            COMMENT "Cross-compiling the ${bitness} resident agent DLL"
            VERBATIM)
        add_custom_command(OUTPUT ${_agent_injector}
            COMMAND ${_agent_compiler} ${SLOPKIT_AGENT_COMPILE_FLAGS} -o ${_agent_injector}
                ${SLOPKIT_AGENT_SOURCE_DIR}/slopkit_injector.c
                -lkernel32 -lgcc -Wl,--entry,${_agent_injector_entry} -Wl,--subsystem,console
            DEPENDS ${SLOPKIT_AGENT_SOURCE_DIR}/slopkit_injector.c ${SLOPKIT_AGENT_PROTOCOL}
            COMMENT "Cross-compiling the ${bitness} agent injector"
            VERBATIM)
        add_custom_command(OUTPUT ${_agent_probe}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SLOPKIT_AGENT_PROBE_DIR}
            COMMAND ${_agent_compiler} ${SLOPKIT_AGENT_COMPILE_FLAGS} -o ${_agent_probe}
                ${SLOPKIT_AGENT_SOURCE_DIR}/slopkit_probe.c
                -lkernel32 -lgcc -Wl,--entry,${_agent_probe_entry} -Wl,--subsystem,console
            DEPENDS ${SLOPKIT_AGENT_SOURCE_DIR}/slopkit_probe.c
            COMMENT "Cross-compiling the ${bitness} end-to-end test probe"
            VERBATIM)
        list(APPEND SLOPKIT_AGENT_PROBES ${_agent_probe})
    else()
        set(_agent_prebuilt_dll ${SLOPKIT_AGENT_PREBUILT_DIR}/${bitness}/slopkit-agent.dll)
        set(_agent_prebuilt_injector ${SLOPKIT_AGENT_PREBUILT_DIR}/${bitness}/slopkit-injector.exe)
        if(EXISTS ${_agent_prebuilt_dll} AND EXISTS ${_agent_prebuilt_injector})
            add_custom_command(OUTPUT ${_agent_dll} ${_agent_injector}
                COMMAND ${CMAKE_COMMAND} -E copy ${_agent_prebuilt_dll} ${_agent_dll}
                COMMAND ${CMAKE_COMMAND} -E copy ${_agent_prebuilt_injector} ${_agent_injector}
                DEPENDS ${_agent_prebuilt_dll} ${_agent_prebuilt_injector}
                COMMENT "Using the prebuilt ${bitness} agent (no mingw-w64 toolchain)"
                VERBATIM)
        else()
            add_custom_command(OUTPUT ${_agent_dll} ${_agent_injector}
                COMMAND ${CMAKE_COMMAND} -E echo
                    "error: no ${bitness}-w64-mingw32-gcc and no prebuilt agent in ${SLOPKIT_AGENT_PREBUILT_DIR}/${bitness}"
                COMMAND ${CMAKE_COMMAND} -E echo
                    "error: install the mingw-w64 ${bitness} toolchain, or place slopkit-agent.dll and slopkit-injector.exe there"
                COMMAND ${CMAKE_COMMAND} -E false
                COMMENT "The ${bitness} resident agent cannot be built"
                VERBATIM)
        endif()
    endif()
endforeach()

# One umbrella target so the whole agent can be built on its own; it is part of
# the default build, so `tools/verify.sh` and `tools/install.sh` cover it.
add_custom_target(slopkit-agent DEPENDS ${SLOPKIT_AGENT_ARTIFACTS})

# The test probes are not part of the default build: `slopkit_tests` depends on
# this target, and it stays empty (and therefore harmless) when no mingw-w64
# toolchain is present, in which case the wine-gated test skips itself.
add_custom_target(slopkit-agent-probe DEPENDS ${SLOPKIT_AGENT_PROBES})
