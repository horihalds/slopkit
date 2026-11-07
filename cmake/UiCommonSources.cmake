# --- shared presentation sources -------------------------------------------
#
# The sources of `slopkit_ui_common`, the presentation library the application,
# the sandbox and the trainer window all build on: palette, fonts, settings,
# platform helpers and the reusable components. Kept in one file so the host
# build and the nested Windows cross-build compile exactly the same set.
#
# `SLOPKIT_SOURCE_ROOT` must be set before this file is included. The embedding
# of the font binaries is the includer's job through `cmake/EmbedFont.cmake`,
# because the generated headers are per-build-tree.

set(SLOPKIT_UI_COMMON_SOURCES
    ${SLOPKIT_SOURCE_ROOT}/src/core/log.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/core/version.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/theme.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/datatype_palette.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/fonts.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/settings.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/platform.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/text.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/components/elided_tooltip_delegate.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/components/indent_delegate.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/components/validating_delegate.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/components/input_box.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/components/message_box.cpp
    ${SLOPKIT_SOURCE_ROOT}/src/ui/components/widgets.cpp)
