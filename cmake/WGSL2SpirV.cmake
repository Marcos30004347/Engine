include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(wgsl2spirv)
    set(options)
    set(oneValueArgs NAME SRC DST)
    set(multiValueArgs ARGS DEFINES INCLUDES)

    cmake_parse_arguments(WGSL
        "${options}"
        "${oneValueArgs}"
        "${multiValueArgs}"
        ${ARGN}
    )

    if(NOT WGSL_NAME)
        message(FATAL_ERROR "wgsl2spirv: NAME is required")
    endif()

    if(NOT WGSL_SRC)
        message(FATAL_ERROR "wgsl2spirv: SRC is required")
    endif()

    if(NOT WGSL_DST)
        message(FATAL_ERROR "wgsl2spirv: DST is required")
    endif()

    get_filename_component(SRC_ABS "${WGSL_SRC}" ABSOLUTE)
    get_filename_component(DST_ABS "${WGSL_DST}" ABSOLUTE)
    get_filename_component(DST_DIR "${DST_ABS}" DIRECTORY)

    set(PP_WGSL "${DST_DIR}/${WGSL_NAME}.pp.wgsl")

    # ---- expand DEFINES / INCLUDES into CLI args ----
    set(PP_ARGS)

    foreach(def ${WGSL_DEFINES})
        list(APPEND PP_ARGS -D "${def}")
    endforeach()

    foreach(inc ${WGSL_INCLUDES})
        list(APPEND PP_ARGS -I "${inc}")
    endforeach()
    # ------------------------------------------------

    add_custom_command(
        OUTPUT "${PP_WGSL}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DST_DIR}"
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/wgsl_pp.py
            "${SRC_ABS}"
            -o "${PP_WGSL}"
            ${PP_ARGS}
        DEPENDS "${SRC_ABS}"
        COMMENT "Preprocessing WGSL: ${WGSL_NAME}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${DST_ABS}"
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/wgsl2spirv.py
            "${PP_WGSL}"
            -o "${DST_ABS}"
            ${WGSL_ARGS}
        DEPENDS "${PP_WGSL}"
        COMMENT "WGSL → SPIR-V: ${WGSL_NAME}"
        VERBATIM
    )

    add_custom_target(${WGSL_NAME}
        DEPENDS "${DST_ABS}"
    )
endfunction()
