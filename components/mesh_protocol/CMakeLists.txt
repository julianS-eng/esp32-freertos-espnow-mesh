# Shared protocol component. Pure C99: no ESP-IDF dependency, so the same
# sources are also built by host/CMakeLists.txt for tests and simulation.
idf_component_register(
    SRCS "mesh_crc.c" "mesh_protocol.c" "mesh_seq.c" "mesh_tx.c"
    INCLUDE_DIRS "include"
)
