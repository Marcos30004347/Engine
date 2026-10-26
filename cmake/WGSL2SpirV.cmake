include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(wgsl2spirv)
    set(options)
    set(oneValueArgs NAME SRC DST)
    # EXECUTION_MODES: SPIR-V execution modes to declare on the fragment entry
    # point after compilation. WGSL cannot express conservative depth, so a
    # shader that writes frag_depth is stuck with late-Z unless the declaration
    # is patched in here.
    set(multiValueArgs ARGS DEFINES INCLUDES EXECUTION_MODES)

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
    set(PP_DEPFILE "${DST_DIR}/${WGSL_NAME}.d")

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
            --depfile "${PP_DEPFILE}"
            ${PP_ARGS}
        DEPFILE "${PP_DEPFILE}"
        DEPENDS "${SRC_ABS}" "${CMAKE_SOURCE_DIR}/scripts/wgsl_pp.py"
        COMMENT "Preprocessing WGSL: ${WGSL_NAME}"
        VERBATIM
    )

    set(EXECUTION_MODE_COMMANDS)
    set(EXECUTION_MODE_DEPENDS)
    if(WGSL_EXECUTION_MODES)
        foreach(mode ${WGSL_EXECUTION_MODES})
            list(APPEND EXECUTION_MODE_COMMANDS
                COMMAND
                    ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/scripts/spirv_add_execution_mode.py
                    "${DST_ABS}"
                    --mode "${mode}"
            )
        endforeach()
        set(EXECUTION_MODE_DEPENDS
            "${CMAKE_SOURCE_DIR}/scripts/spirv_add_execution_mode.py")
    endif()

    add_custom_command(
        OUTPUT "${DST_ABS}"
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/wgsl2spirv.py
            "${PP_WGSL}"
            -o "${DST_ABS}"
            ${WGSL_ARGS}
        ${EXECUTION_MODE_COMMANDS}
        DEPENDS "${PP_WGSL}" "${CMAKE_SOURCE_DIR}/scripts/wgsl2spirv.py"
                ${EXECUTION_MODE_DEPENDS}
        COMMENT "WGSL → SPIR-V: ${WGSL_NAME}"
        VERBATIM
    )

    add_custom_target(${WGSL_NAME}
        DEPENDS "${DST_ABS}"
    )
endfunction()
