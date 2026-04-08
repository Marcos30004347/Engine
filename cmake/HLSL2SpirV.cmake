include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(hlsl2spirv)
    set(options)
    set(oneValueArgs NAME SRC DST)
    set(multiValueArgs ARGS)

    cmake_parse_arguments(HLSL
        "${options}"
        "${oneValueArgs}"
        "${multiValueArgs}"
        ${ARGN}
    )

    if(NOT HLSL_NAME)
        message(FATAL_ERROR "hlsl2spirv: NAME is required")
    endif()

    if(NOT HLSL_SRC)
        message(FATAL_ERROR "hlsl2spirv: SRC is required")
    endif()

    if(NOT HLSL_DST)
        message(FATAL_ERROR "hlsl2spirv: DST is required")
    endif()

    get_filename_component(SRC_ABS "${HLSL_SRC}" ABSOLUTE)
    get_filename_component(DST_ABS "${HLSL_DST}" ABSOLUTE)
    get_filename_component(DST_DIR "${DST_ABS}" DIRECTORY)

    add_custom_command(
        OUTPUT "${DST_ABS}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DST_DIR}"
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/hlsl2spirv.py
            "${SRC_ABS}"
            -o "${DST_ABS}"
            ${HLSL_ARGS}
        DEPENDS "${SRC_ABS}"
        COMMENT "HLSL → SPIR-V: ${SRC_ABS}"
        VERBATIM
    )

    add_custom_target(${HLSL_NAME}
        DEPENDS "${DST_ABS}"
    )
endfunction()
