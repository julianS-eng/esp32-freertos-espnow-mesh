idf_component_register(
    SRCS "main.c" "node_tasks.c" "node_storage.c"
    INCLUDE_DIRS "."
    REQUIRES mesh_protocol mesh_radio sensor_hal node_core
    PRIV_REQUIRES nvs_flash esp_timer esp_system log
)
