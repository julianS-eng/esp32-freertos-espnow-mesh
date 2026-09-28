# Platform-independent gateway logic (also compiled by host/CMakeLists.txt).
idf_component_register(
    SRCS "gw_core.c" "gw_fsm.c" "gw_json.c" "gw_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES mesh_protocol
)
