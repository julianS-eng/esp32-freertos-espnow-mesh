idf_component_register(
    SRCS "main.c" "gw_tasks.c" "gw_storage.c"
    INCLUDE_DIRS "."
    REQUIRES gw_core mesh_protocol mesh_radio
    PRIV_REQUIRES nvs_flash esp_timer esp_system esp_driver_uart vfs log
)
