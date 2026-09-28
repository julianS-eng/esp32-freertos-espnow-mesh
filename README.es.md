# ESP32-S3 FreeRTOS + ESP-NOW Sensor Mesh (resumen en español)

[![CI](https://github.com/julianS-eng/esp32-freertos-espnow-mesh/actions/workflows/ci.yml/badge.svg)](https://github.com/julianS-eng/esp32-freertos-espnow-mesh/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Python 3.11+](https://img.shields.io/badge/python-3.11%2B-blue.svg)](pyproject.toml)

La documentación completa está en inglés en el [README principal](README.md).
Para una explicación didáctica paso a paso, con decisiones de diseño y preguntas
de entrevista, lee [docs/LEARNING.md](docs/LEARNING.md) (en español).

## Qué es

Firmware en C para **ESP32-S3 N16R8** con **ESP-IDF** (`idf.py`, sin Arduino) que
forma una red de sensores inalámbrica:

* **Nodos sensores**: leen un sensor de gas **MQ-2**, un **motor R380 con encoder
  magnético AS5600** y una IMU **MPU-6050 (GY-521)**, o un simulador determinista,
  seleccionable en `menuconfig`. Envían las lecturas por **ESP-NOW** con ACK de
  aplicación, reintentos con back-off exponencial y jitter, y número de secuencia.
* **Gateway**: valida (CRC-16, versión, longitud), elimina duplicados, cuenta
  pérdidas con una ventana deslizante, detecta nodos caídos
  (online → suspect → offline), guarda el registro de nodos en NVS y emite **una
  línea JSON por evento** por el puerto serie. Acepta comandos (`set`, `reboot`,
  `forget`, `stats`) y aplica contrapresión (`ACK BUSY`) si el puerto serie se
  satura.
* **Herramienta Python** (`meshdash`): dashboard en vivo en la terminal, métricas
  de actividad, pérdida y latencia, figuras y GIF.

## Arquitectura en una frase

Toda la lógica que decide algo (protocolo, reintentos, duplicados, pérdidas,
vivacidad, máquina de estados del gateway, JSON) está en **C puro sin
dependencias de ESP-IDF**, y se compila tres veces: en el firmware, en 100 tests
Unity (con ASan/UBSan) y en un **simulador de eventos discretos**. Las tareas
FreeRTOS solo mueven datos entre colas.

| Nodo: tarea | Prioridad | Stack | Pico medido |
|---|---:|---:|---:|
| supervisor | 10 | 3072 B | 888 B |
| tx | 8 | 5120 B | 2888 B |
| sensor | 6 | 4096 B | 1028 B |
| heartbeat | 4 | 3584 B | 2204 B |

Los picos se midieron con `uxTaskGetStackHighWaterMark()` ejecutando el firmware
real en **QEMU** (con un radio "loopback" que sustituye solo el RF).

## Resultados (simulación de 6 nodos, 600 s, semilla 42)

| Métrica | Valor |
|---|---|
| Tramas únicas aceptadas | 4303 |
| Perdidas tras todos los reintentos | 22 (0,509 %) |
| Tramas que necesitaron reintento | 3,50 % |
| Duplicados suprimidos (ACK perdidos) | 130 |
| RTT p50 / p95 | 2,61 / 2,88 ms |
| Comprobaciones de consistencia dashboard vs. gateway | 30 / 30 correctas |

Los parámetros del canal de radio del simulador son **entradas elegidas**, no
mediciones: estos números validan la lógica del protocolo, no el alcance real.

## Compilar y flashear

```bash
cd gateway && idf.py set-target esp32s3 && idf.py build flash monitor -b 921600
cd sensor_node && idf.py set-target esp32s3 && idf.py menuconfig && idf.py build flash monitor
pip install -e ".[serial]" && meshdash live --port /dev/ttyUSB0
```

## Validado en simulación vs. pendiente de hardware

* ✅ Ambos firmwares compilan para esp32s3 con la imagen oficial `espressif/idf:v5.4`
  (incluida la variante con todos los drivers reales y cifrado).
* ✅ Ambos firmwares arrancan y funcionan en QEMU (join, reintentos, CONFIG por
  consola, transiciones online/suspect/offline, JSON).
* ✅ Lógica del protocolo validada con 100 tests C y 43 tests Python.
* ⏳ Pendiente con hardware: comportamiento real de la radio (pérdidas, RSSI,
  alcance), cifrado ESP-NOW extremo a extremo, lecturas reales de MQ-2 / AS5600 /
  MPU-6050, consumo de stack dentro de `esp_now_send()`, PSRAM octal.
* ❌ No implementado: ahorro de energía (deep sleep), multi-salto, OTA, puente MQTT.

Licencia: [MIT](LICENSE).
