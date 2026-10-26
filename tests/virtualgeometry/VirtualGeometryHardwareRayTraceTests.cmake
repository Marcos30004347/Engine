if(TARGET vg_hardware_raytrace)
    add_executable(VirtualGeometryHardwareRayTraceGraphTests
        ${CMAKE_CURRENT_LIST_DIR}/VirtualGeometryHardwareRayTraceGraphTests.cpp)
    target_link_libraries(VirtualGeometryHardwareRayTraceGraphTests PRIVATE Engine)
    add_test(NAME VirtualGeometryHardwareRayTraceGraphTests COMMAND VirtualGeometryHardwareRayTraceGraphTests)
    set_tests_properties(VirtualGeometryHardwareRayTraceGraphTests PROPERTIES
        LABELS "virtualgeometry;raytracing;metal;gpu" SKIP_RETURN_CODE 77)
    add_executable(VirtualGeometryHardwareRayTraceTests
        ${CMAKE_CURRENT_LIST_DIR}/VirtualGeometryHardwareRayTraceTests.mm)
    target_compile_options(VirtualGeometryHardwareRayTraceTests PRIVATE -fobjc-arc)
    target_link_libraries(VirtualGeometryHardwareRayTraceTests PRIVATE vg_hardware_raytrace "-framework Foundation" "-framework Metal")
    add_test(NAME VirtualGeometryHardwareRayTraceTests COMMAND VirtualGeometryHardwareRayTraceTests)
    set_tests_properties(VirtualGeometryHardwareRayTraceTests PROPERTIES
        LABELS "virtualgeometry;raytracing;metal;gpu" SKIP_RETURN_CODE 77)
    add_executable(VirtualGeometryHardwareRayTraceBenchmark
        ${CMAKE_CURRENT_LIST_DIR}/VirtualGeometryHardwareRayTraceBenchmark.mm)
    target_compile_options(VirtualGeometryHardwareRayTraceBenchmark PRIVATE -fobjc-arc)
    target_link_libraries(VirtualGeometryHardwareRayTraceBenchmark PRIVATE vg_hardware_raytrace "-framework Foundation" "-framework Metal")
    add_test(NAME VirtualGeometryHardwareRayTraceBenchmarkSmoke COMMAND
        VirtualGeometryHardwareRayTraceBenchmark --suite full --instances 16 --rays 256 --warmup 1 --frames 3)
    set_tests_properties(VirtualGeometryHardwareRayTraceBenchmarkSmoke PROPERTIES
        LABELS "virtualgeometry;raytracing;metal;gpu;benchmark" SKIP_RETURN_CODE 77)
endif()
