add_library(cli INTERFACE)
target_link_libraries(cli INTERFACE editor)

add_subdirectory(virtualgeometry/importer/obj)
add_subdirectory(virtualgeometry/importer/gltf)
add_subdirectory(virtualtexture/importer)
