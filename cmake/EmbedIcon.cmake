# Program icon pipeline: turns the single vector source into the installed
# hicolor PNG set (and into the Qt resources that carry the window icon).
#
#   slopkit_icon_pngs(<source> <out_var> [SIZES 16 24 ...])
#     Writes ${CMAKE_BINARY_DIR}/icons/<N>x<N>/apps/slopkit.png for every size
#     and returns the resulting file list in <out_var>.
#
# The function requires SLOPKIT_MAGICK_EXECUTABLE (resolved in CMakeLists.txt)
# and re-runs whenever <source> changes. The source is a self-contained square
# vector, so no crop is applied; `-background none` keeps its transparency.

function(slopkit_icon_pngs source out_var)
    cmake_parse_arguments(ARG "" "" "SIZES" ${ARGN})

    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "slopkit_icon_pngs: no such file: ${source}")
    endif()

    set(pngs "")
    foreach(size IN LISTS ARG_SIZES)
        set(png_dir "${CMAKE_BINARY_DIR}/icons/${size}x${size}/apps")
        set(png_path "${png_dir}/slopkit.png")
        file(MAKE_DIRECTORY "${png_dir}")
        execute_process(
            COMMAND "${SLOPKIT_MAGICK_EXECUTABLE}" -background none "${source}"
                    -resize "${size}x${size}"
                    -strip "${png_path}"
            COMMAND_ERROR_IS_FATAL ANY)
        list(APPEND pngs "${png_path}")
    endforeach()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    set(${out_var} "${pngs}" PARENT_SCOPE)
endfunction(slopkit_icon_pngs)

# UI action glyph pipeline: turns a small square vector into the multi-size PNG
# set that is embedded (never installed) for a menu or button icon.
#
#   slopkit_action_icon_pngs(<source> <out_var> [SIZES 16 24 ...])
#     Writes ${CMAKE_BINARY_DIR}/icons/actions/<N>x<N>/<basename>.png for every
#     size and returns the resulting file list in <out_var>.
#
# The `actions/` sub-tree keeps these glyphs out of the hicolor `apps/` tree, so
# the desktop-entry install and the window icon are untouched. Like
# slopkit_icon_pngs it requires SLOPKIT_MAGICK_EXECUTABLE and re-runs whenever
# <source> changes; the source is a self-contained square vector, so no crop is
# applied and `-background none` keeps its transparency.

function(slopkit_action_icon_pngs source out_var)
    cmake_parse_arguments(ARG "" "" "SIZES" ${ARGN})

    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "slopkit_action_icon_pngs: no such file: ${source}")
    endif()

    get_filename_component(name "${source}" NAME_WE)
    set(pngs "")
    foreach(size IN LISTS ARG_SIZES)
        set(png_dir "${CMAKE_BINARY_DIR}/icons/actions/${size}x${size}")
        set(png_path "${png_dir}/${name}.png")
        file(MAKE_DIRECTORY "${png_dir}")
        execute_process(
            COMMAND "${SLOPKIT_MAGICK_EXECUTABLE}" -background none "${source}"
                    -resize "${size}x${size}"
                    -strip "${png_path}"
            COMMAND_ERROR_IS_FATAL ANY)
        list(APPEND pngs "${png_path}")
    endforeach()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    set(${out_var} "${pngs}" PARENT_SCOPE)
endfunction(slopkit_action_icon_pngs)
