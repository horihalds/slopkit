# --- trainer sources -------------------------------------------------------
#
# The self-contained trainer is built from these lists three times: the native
# Linux template, the cross-compiled Windows template and the test binary. The
# script engine and every Qt-free translation unit it needs are shared verbatim
# with the application, so the trainer's Lua semantics cannot drift from the
# app's; the target backend and the units it needs are the only per-platform
# difference.
#
# `SLOPKIT_SOURCE_ROOT` must be set before this file is included.

# The native trainer window, its table model and the session thread: the third
# UI in the project, built from the same presentation library as the app and the
# sandbox. The Windows template replaces these with a native Win32 window so its
# cross-build needs no Qt at all.
set(SLOPKIT_TRAINER_QT_SOURCES
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/gui/rows_model.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/gui/session_worker.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/gui/window.cpp
    # AUTOMOC only sees a header it is given by name when it has no matching
    # source file, which `window_qt.hpp` does not.
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/gui/window_qt.hpp)

set(SLOPKIT_TRAINER_WIN32_SOURCES
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/gui/window_win32.cpp)

set(SLOPKIT_TRAINER_RUNTIME_SOURCES
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/caves.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/console.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/logging.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/payload_reader.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/resolve.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/rows.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/scripts.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/target_linux.cpp
    ${SLOPKIT_TRAINER_QT_SOURCES})

# The Qt-free platform units the Linux target backend builds on: the procfs
# parsers, the PE/ELF header readers and the Wine classification. The trainer's
# module list, region names and pointer width are the application's own as a
# result, so a Wine target is seen exactly as the app sees it. Kept out of the
# runtime list because the test binary takes these from `slopkit_platform`.
set(SLOPKIT_TRAINER_LINUX_SOURCES
    ${SLOPKIT_SOURCE_ROOT}/src/platform/linux/proc_text.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/platform/linux/module_entry.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/platform/linux/procfs.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/platform/linux/wine.cpp)

# The application's Lua engine and its Qt-free dependencies (the reclass class
# project, the Zydis-backed assembler and scan/pattern). Kept out of the runtime
# list because the test binary already compiles these files through the
# application source glob and would otherwise define them twice.
set(SLOPKIT_TRAINER_SCRIPT_SOURCES
    ${SLOPKIT_SOURCE_ROOT}/src/script/codec.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/script/engine.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/script/hook.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/script/structs.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/script/structs_view.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/script/symbols.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/disasm/assembler.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/scan/pattern.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/cpp_header.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/dissect.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/edit.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/enum.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/node.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/node_menu.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/project.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/serialize.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/value.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/nodes/container.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/nodes/node.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/reclass/nodes/number.cpp)

set(SLOPKIT_TRAINER_SOURCES
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/payload.cpp
    ${SLOPKIT_TRAINER_RUNTIME_SOURCES}
    ${SLOPKIT_TRAINER_LINUX_SOURCES}
    ${SLOPKIT_TRAINER_SCRIPT_SOURCES}
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/main.cpp
    # The shared codec and expression evaluator the app uses as well, so the two
    # cannot drift apart; log.cpp is inert until a sink is installed.
    ${SLOPKIT_SOURCE_ROOT}/src/core/log.cpp
    # The module classification the platform readers and the target backend both
    # use (`is_file_backed`, `main_module`).
    ${SLOPKIT_SOURCE_ROOT}/src/process/types.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/expr/expression.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/expr/resolver.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/scan/types.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/scan/value.cpp)

# The Windows template swaps the Linux target backend and the Qt window for the
# Windows backend and the native Win32 window, so the procfs, PE-header and Wine
# units go with them.
set(SLOPKIT_TRAINER_WINDOWS_SOURCES ${SLOPKIT_TRAINER_SOURCES})
list(REMOVE_ITEM SLOPKIT_TRAINER_WINDOWS_SOURCES
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/target_linux.cpp
    ${SLOPKIT_TRAINER_LINUX_SOURCES}
    ${SLOPKIT_TRAINER_QT_SOURCES})
list(APPEND SLOPKIT_TRAINER_WINDOWS_SOURCES
    ${SLOPKIT_SOURCE_ROOT}/src/trainer/runtime/target_windows.cpp
    ${SLOPKIT_TRAINER_WIN32_SOURCES})
