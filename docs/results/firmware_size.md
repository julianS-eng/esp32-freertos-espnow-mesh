<!-- Produced by `idf.py size` (ESP-IDF v5.4, target esp32s3, default sdkconfig.defaults of each project). -->
| Firmware | App image | App partition use | Flash code (.text) | Flash data (.rodata) | DIRAM used | DIRAM .bss | IRAM used |
|---|---:|---:|---:|---:|---:|---:|---:|
| sensor_node (simulated sensors) | 649 936 B | 15 % of 4 MiB | 449 108 B | 91 444 B | 109 996 B (32.2 %) | 16 968 B | 16 383 B (99.99 %) |
| gateway | 684 416 B | 16 % of 4 MiB | 476 028 B | 95 932 B | 123 300 B (36.1 %) | 27 200 B | 16 383 B (99.99 %) |

`IRAM used` is the dedicated 16 KiB IRAM region reported separately by
`esp_idf_size`; it is always filled first and the remaining IRAM-resident code
continues in the shared DIRAM (`DIRAM .text`: 71 515 B node / 74 583 B
gateway), so a full IRAM line is not a problem by itself. Heap left for tasks
and queues is reported at runtime (`free_heap`/`min_heap` in heartbeats and
`gw_stats`). PSRAM (8 MB) is not counted here.
