# Install-step size pipeline: installs a target and moves its debug info out of
# the installed file into a sibling `.debug` sidecar.
#
#   slopkit_install_stripped(<target> <destination> [RUNTIME|LIBRARY])
#     Installs <target> into <destination> (default type: RUNTIME), then, when
#     stripping is enabled, splits the installed file with `objcopy`.
#
# With `SLOPKIT_STRIP_INSTALL` ON and `SLOPKIT_OBJCOPY_EXECUTABLE` available
# (both resolved in CMakeLists.txt) `objcopy --only-keep-debug` writes the debug
# info to a sidecar and `objcopy --strip-debug --add-gnu-debuglink` removes it
# from the file and points it at that sidecar. For a RUNTIME the sidecar is the
# sibling `<name>.debug`; for a LIBRARY it goes into the `.debug/` sub-directory
# of the destination, because the host scans the plugin directory for loadable
# files and would otherwise reject the sidecar as a plugin.
# The installed file is a few MB smaller while `gdb <installed>` still resolves
# source lines through the sidecar. Only `--strip-debug` is used, never
# `--strip-all`, so `.dynsym`/`.symtab` survive and a dlopened plugin keeps
# resolving the application's exported symbols. With the option OFF or `objcopy`
# missing, the target is installed verbatim, exactly as before.

function(slopkit_install_stripped target destination)
    cmake_parse_arguments(ARG "RUNTIME;LIBRARY" "" "" ${ARGN})

    if(ARG_LIBRARY)
        set(type LIBRARY)
        # The plugin directory is scanned for loadable files, so the sidecar must
        # not sit next to the library; gdb's `.debug` sub-directory convention
        # keeps it discoverable without the loader ever seeing it.
        set(debug_dir "${destination}/.debug")
    else()
        set(type RUNTIME)
        set(debug_dir "${destination}")
    endif()

    install(TARGETS ${target} ${type} DESTINATION ${destination})

    if(NOT SLOPKIT_STRIP_INSTALL OR NOT SLOPKIT_OBJCOPY_EXECUTABLE)
        return()
    endif()

    # `$ENV{DESTDIR}` and `${CMAKE_INSTALL_PREFIX}` stay install-time references
    # (escaped here), so a staged or `--prefix`-overridden install resolves them
    # correctly; the target and destination are baked in at configure time. The
    # debug file is passed as its own variable because CMake does not strip
    # quotes that sit in the middle of a token (`--add-gnu-debuglink="${dbg}"`
    # would reach objcopy with the quotes as part of the name).
    install(CODE "
        set(bin \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${destination}/$<TARGET_FILE_NAME:${target}>\")
        set(dbg \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${debug_dir}/$<TARGET_FILE_NAME:${target}>.debug\")
        get_filename_component(dbg_dir \"\${dbg}\" DIRECTORY)
        file(MAKE_DIRECTORY \"\${dbg_dir}\")
        execute_process(COMMAND \"${SLOPKIT_OBJCOPY_EXECUTABLE}\" --only-keep-debug \"\${bin}\" \"\${dbg}\" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND \"${SLOPKIT_OBJCOPY_EXECUTABLE}\" --strip-debug --add-gnu-debuglink=\${dbg} \"\${bin}\" COMMAND_ERROR_IS_FATAL ANY)")
endfunction(slopkit_install_stripped)
