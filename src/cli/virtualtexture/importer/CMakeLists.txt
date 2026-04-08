project(vtmaterial_create_cli LANGUAGES CXX)

add_executable(vtmaterial_create main.cpp)
target_link_libraries(vtmaterial_create PRIVATE cli)
engine_copy_sdl_runtime(vtmaterial_create)
