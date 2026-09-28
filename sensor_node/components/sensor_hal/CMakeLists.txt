# Sensor abstraction layer. sensor_math.c and sensor_sim.c are pure C and are
# also compiled by host/CMakeLists.txt; the hardware drivers compile to empty
# translation units unless enabled in menuconfig.
idf_component_register(
    SRCS "sensor_hal.c" "sensor_math.c" "sensor_sim.c"
         "sensor_mq2.c" "sensor_motor_as5600.c" "sensor_mpu6050.c" "i2c_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES mesh_protocol esp_driver_i2c
    PRIV_REQUIRES esp_adc esp_driver_ledc esp_driver_gpio esp_timer nvs_flash log
)
