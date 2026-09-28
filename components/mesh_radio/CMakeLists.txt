# ESP-IDF glue for ESP-NOW shared by both firmwares (not built on the host).
# The loopback backend is a QEMU/bench test double selected in menuconfig.
if(CONFIG_MESH_RADIO_ESPNOW)
    set(srcs "mesh_radio.c")
else()
    set(srcs "mesh_radio_loopback.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES mesh_protocol esp_wifi esp_event
    PRIV_REQUIRES log esp_timer
)
