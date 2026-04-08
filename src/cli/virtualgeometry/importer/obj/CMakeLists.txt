project(vgimport_obj_cli LANGUAGES CXX)

add_executable(vgimport_obj main.cpp)
target_link_libraries(vgimport_obj PRIVATE cli)
engine_copy_sdl_runtime(vgimport_obj)
