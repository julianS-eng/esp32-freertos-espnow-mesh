# Pure node logic (also compiled by host/CMakeLists.txt for unit tests).
idf_component_register(
    SRCS "node_config.c"
    INCLUDE_DIRS "include"
    REQUIRES mesh_protocol
)
