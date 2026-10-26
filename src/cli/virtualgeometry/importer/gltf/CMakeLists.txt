project(vgimport_gltf_cli LANGUAGES CXX)

add_executable(vgimport_gltf main.cpp)
target_link_libraries(vgimport_gltf PRIVATE cli)
engine_copy_sdl_runtime(vgimport_gltf)
