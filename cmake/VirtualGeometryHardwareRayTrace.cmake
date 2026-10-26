if(APPLE AND ENGINE_VG_HARDWARE_RAYTRACE)
    set(VG_RT_SHADER ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/metal/virtualgeometry-raytrace.metal)
    set(VG_RT_METALLIB ${CMAKE_BINARY_DIR}/tests/assets/shaders/metal/virtualgeometry-raytrace.metallib)
    add_custom_command(OUTPUT ${VG_RT_METALLIB}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/tests/assets/shaders/metal
        COMMAND xcrun -sdk macosx metal -std=metal3.1 -c ${VG_RT_SHADER}
            -o ${CMAKE_BINARY_DIR}/tests/assets/shaders/metal/virtualgeometry-raytrace.air
        COMMAND xcrun -sdk macosx metallib
            ${CMAKE_BINARY_DIR}/tests/assets/shaders/metal/virtualgeometry-raytrace.air -o ${VG_RT_METALLIB}
        DEPENDS ${VG_RT_SHADER} VERBATIM)
    add_custom_target(VirtualGeometryHardwareRayTraceShaders DEPENDS ${VG_RT_METALLIB})
    add_library(vg_hardware_raytrace STATIC
        ${CMAKE_SOURCE_DIR}/src/runtime/virtualgeometry/raytracing/metal/VirtualGeometryMetalRayTraceBackend.mm)
    target_include_directories(vg_hardware_raytrace PUBLIC ${CMAKE_SOURCE_DIR}/src/runtime)
    target_compile_options(vg_hardware_raytrace PRIVATE -fobjc-arc)
    target_compile_features(vg_hardware_raytrace PUBLIC cxx_std_17)
    target_compile_definitions(vg_hardware_raytrace PUBLIC VG_RT_METALLIB_PATH="${VG_RT_METALLIB}")
    target_compile_definitions(vg_hardware_raytrace PUBLIC ENGINE_VG_HAS_METAL_RAYTRACE=1)
    target_link_libraries(vg_hardware_raytrace PRIVATE "-framework Foundation" "-framework Metal")
    add_dependencies(vg_hardware_raytrace VirtualGeometryHardwareRayTraceShaders)
    target_link_libraries(runtime PUBLIC vg_hardware_raytrace)
endif()
