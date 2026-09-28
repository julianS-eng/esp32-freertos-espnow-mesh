# Guía de aprendizaje: FreeRTOS + ESP-NOW en el ESP32-S3

Este documento explica **por qué** el proyecto está construido como está. Va de la
teoría a las decisiones concretas, cuenta qué alternativas se descartaron y
termina con 10 preguntas de entrevista técnica con sus respuestas. Todos los
números citados salen de ejecuciones reales del código de este repositorio
(ver `docs/results/`).

---

## Parte 1 · Teoría paso a paso

### 1.1 Qué es un RTOS y por qué el ESP32 lo necesita

Un programa embebido "clásico" es un *super-loop*: `while (1) { leer(); enviar(); dormir(); }`.
Funciona mientras todas las actividades tengan el mismo ritmo. En cuanto aparecen
ritmos distintos (muestrear un encoder a 1 kHz, esperar un ACK con 30 ms de
timeout, mandar un latido cada 5 s, vigilar que nada se cuelgue) el super-loop se
convierte en una máquina de estados gigante y frágil.

Un **RTOS** (sistema operativo de tiempo real) reparte la CPU entre **tareas**
independientes, cada una con su propia pila (*stack*) y su **prioridad**.
FreeRTOS, que es la base de ESP-IDF, usa un planificador **preemptivo por
prioridades**: siempre ejecuta la tarea lista de mayor prioridad; si una tarea
más prioritaria se desbloquea (llega un dato a su cola), expulsa inmediatamente
a la que corre. Con prioridades iguales reparte por turnos (*time slicing*) en
cada *tick*.

En el ESP32-S3 hay **dos núcleos** (SMP): FreeRTOS de ESP-IDF puede fijar tareas
a un núcleo (`xTaskCreatePinnedToCore`). La pila Wi-Fi corre en el núcleo 0 con
prioridad 23.

**Tiempo real no significa "rápido", significa "predecible"**: lo importante es
que la tarea del ACK siempre pueda responder dentro de su plazo, aunque otra
tarea esté formateando JSON.

### 1.2 Tareas, estados y pilas

Una tarea está *Running*, *Ready*, *Blocked* (esperando una cola, un semáforo o
un tiempo) o *Suspended*. Una tarea bien diseñada pasa casi todo el tiempo
**Blocked**: no consume CPU y el núcleo puede entrar en la tarea *idle*.

Cada tarea tiene una pila fija que se reserva al crearla. Si se desborda,
corrompe memoria ajena: es uno de los fallos más difíciles de depurar. Por eso:

* `uxTaskGetStackHighWaterMark()` devuelve el **mínimo de pila libre** que ha
  tenido la tarea desde que arrancó (en ESP-IDF, en **bytes**).
* Se dimensiona con datos: pico medido × ~1,5, redondeado, más margen donde el
  código no se pudo ejercitar. En este proyecto, por ejemplo, la tarea `tx` del
  nodo usó como máximo 2876 B en QEMU y tiene 5120 B, porque `esp_now_send()`
  real no se ejecutó en QEMU.
* Se activan redes de seguridad: canario de desbordamiento
  (`CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY`) y *watchpoint* al final de la pila.

Regla práctica: nada de buffers grandes en la pila (se declaran `static`), y
cuidado con `printf`/`ESP_LOG` con muchos argumentos: la tarea `heartbeat` es la
que más pila usa (2272 B) precisamente por sus líneas de log.

### 1.3 Comunicación entre tareas

| Primitiva | Semántica | Uso en el proyecto |
|---|---|---|
| **Cola** (`xQueue`) | Copia datos, FIFO, acotada, bloqueante | lecturas `sensor → tx`, ACKs `callback Wi-Fi → tx`, registros JSON `rx → out` |
| **Mutex** | Exclusión mutua **con herencia de prioridad** | configuración y estadísticas del nodo, registro del gateway, bus I²C |
| **Semáforo binario/contador** | Señal / recuento de eventos | (sustituido por notificaciones, ver abajo) |
| **Event group** | Varios *flags* de bits, varias tareas pueden esperar | `SEND_OK/SEND_FAIL/JOINED`, *check-ins* del supervisor |
| **Notificación directa a tarea** | Un contador/flag por tarea, lo más ligero | despertar a `tx` cuando hay trabajo; avisar a `sensor` de un cambio de intervalo |

**Inversión de prioridad**: si una tarea de baja prioridad tiene un recurso que
necesita una de alta, y una de prioridad media expulsa a la baja, la alta queda
esperando indefinidamente (el caso famoso de la Mars Pathfinder). El mutex de
FreeRTOS lo evita elevando temporalmente la prioridad de quien lo tiene. Por
eso las estadísticas del nodo (escritas por `tx`, prioridad 8, y leídas por
`heartbeat`, prioridad 4) se protegen con un **mutex** y no con un semáforo
binario.

**Callbacks del Wi-Fi**: `esp_now_register_recv_cb` ejecuta nuestra función
*dentro de la tarea Wi-Fi*. Ahí no se puede bloquear, ni hacer logs largos, ni
reservar memoria: se decodifica la trama (rápido), se copia a una cola con
timeout 0 y se despierta a la tarea correspondiente.

### 1.4 ESP-NOW

ESP-NOW es un protocolo propietario de Espressif que envía **tramas de acción
802.11 específicas del fabricante** directamente entre dispositivos, sin punto
de acceso ni IP:

* Carga útil ≤ **250 bytes** (ESP-NOW v1).
* Unicast con **ACK de capa MAC** (el *callback* de envío dice si el otro radio
  la recibió) y broadcast sin ACK.
* Todos los dispositivos deben estar en el **mismo canal**.
* Tabla de *peers* de hasta 20 entradas, de las que como máximo 17 cifradas.
* Cifrado opcional **CCMP** con una clave maestra (PMK) que cifra las claves
  locales por peer (LMK). El broadcast nunca va cifrado.

El ACK de la capa MAC solo dice "el radio lo recibió", no "la aplicación lo
procesó". Si la cola del gateway está llena, la trama se pierde igualmente.
Por eso el protocolo añade un **ACK de aplicación**.

### 1.5 Fiabilidad: ARQ stop-and-wait

**ARQ** (*Automatic Repeat reQuest*): el emisor manda una trama y espera un ACK;
si no llega en un plazo (timeout), retransmite. *Stop-and-wait* significa una
sola trama en vuelo.

Piezas clave:

1. **Número de secuencia** para que el receptor distinga una trama nueva de una
   retransmisión (si el ACK se perdió, el emisor reenvía algo ya procesado).
2. **Timeout** algo mayor que el RTT esperado (30 ms frente a ~2,6 ms medidos
   en simulación).
3. **Back-off exponencial con jitter** entre reintentos (20, 40, 80 ms + 0–10 ms
   aleatorios): si el canal está ocupado o hay interferencia, insistir de
   inmediato empeora las cosas, y el jitter evita que varios nodos se
   sincronicen y colisionen una y otra vez.
4. **Límite de intentos** (4): pasado ese punto la trama se abandona y se
   cuenta; mejor perder una lectura que bloquear las siguientes.
5. **Medición de RTT segura (algoritmo de Karn)**: si el ACK llega tras una
   retransmisión, ¿a qué envío corresponde? Aquí el ACK **devuelve el número de
   intento**, así que el RTT se mide siempre contra el envío correcto.

### 1.6 Aritmética de números de secuencia

Con 16 bits, tras 65535 viene 0. Comparar con `>` falla en el salto. La
solución (RFC 1982) es la distancia con signo: `(int16_t)(a − b)`. Si es
positiva, `a` es más nueva. Funciona mientras las dos no estén separadas más de
media vuelta (32768), algo imposible con un emisor stop-and-wait.

El receptor usa una **ventana deslizante de 64 bits** (la misma idea que el
anti-replay de IPsec, RFC 4303): recuerda cuáles de las últimas 64 secuencias ha
visto. Así clasifica cada llegada como nueva (y cuenta el hueco como pérdida),
duplicada, tardía (rellena un hueco) o reinicio.

### 1.7 CRC

Un **CRC** es el resto de dividir el mensaje (visto como polinomio en GF(2))
entre un polinomio generador. CRC-16/CCITT-FALSE (0x1021, valor inicial 0xFFFF)
detecta todos los errores de 1, 2 y 3 bits en tramas de este tamaño (distancia
de Hamming 4) y todas las ráfagas de hasta 16 bits. La versión con tabla procesa
un byte por iteración a cambio de 512 bytes de flash; los tests la comparan con
la versión bit a bit sobre 2000 buffers aleatorios.

¿Por qué un CRC si 802.11 ya tiene FCS? Porque el FCS protege el aire, no el
camino completo: copias entre buffers, colas, o una trama de otro firmware que
por casualidad empieza con el mismo byte mágico.

### 1.8 Detección de nodos caídos

El nodo manda un **latido** cada `hb` ms. El gateway guarda la última vez que
oyó al nodo (cualquier trama válida cuenta) y, cada 500 ms, compara el silencio
con dos umbrales derivados del intervalo que el propio nodo anunció en su JOIN:
`suspect = 2·hb + 1 s` y `offline = 4·hb + 1 s`. El estado intermedio *suspect*
evita falsas alarmas por un par de tramas perdidas. En la simulación de
referencia el nodo 3 pasó a *suspect* a los 11,5 s de silencio y a *offline* a
los 21,5 s, exactamente según esos umbrales con hb = 5 s.

### 1.9 NVS

NVS (*Non-Volatile Storage*) es un almacén clave-valor sobre flash con
*wear-leveling* y escrituras atómicas por entrada. Se guarda: la configuración
del nodo (como un *blob* versionado; si el esquema no coincide o un valor está
fuera de rango, se usan los valores por defecto), un contador de arranques, la
R0 calibrada del MQ-2 y, en el gateway, el registro de nodos con CRC. Escribir en
flash deshabilita la caché unos milisegundos, así que el gateway lo hace desde la
tarea `liveness`, nunca desde la tarea que responde ACKs.

### 1.10 Estrategia de pruebas en embebido

1. **Núcleo puro**: la lógica no incluye cabeceras de ESP-IDF, así que se
   prueba en el PC con sanitizers (ASan encuentra accesos fuera de rango, UBSan
   comportamiento indefinido; UBSan detectó en este proyecto un test que metía
   `0xFF` en un `bool`).
2. **Simulación**: el mismo código C se enlaza con un simulador de eventos
   discretos con tiempo virtual y un modelo de canal. Detectó dos defectos reales
   (un JOIN que ocultaba pérdidas y una transición de estado que no se emitía).
3. **Emulación**: QEMU ejecuta el binario real del ESP32-S3; como no emula el
   RF, un backend de radio "loopback" sustituye solo esa capa. Detectó una tarea
   bloqueada leyendo `stdin` (el supervisor reinició el chip, como debía).
4. **Hardware**: lo que queda pendiente (ver README).

---

## Parte 2 · Decisiones de diseño y alternativas descartadas

| Decisión | Alternativas descartadas | Motivo |
|---|---|---|
| **Estrella de un salto** nodo → gateway | ESP-WIFI-MESH; *flooding* propio | Un salto basta para el alcance objetivo, simplifica el protocolo y la depuración. Multi-salto queda como trabajo futuro. |
| **ACK de aplicación + stop-and-wait** | Confiar solo en el ACK MAC; ventana deslizante | El ACK MAC no informa de duplicados ni de saturación del gateway; a 1 Hz el enlace está ocioso > 99 % del tiempo, una ventana solo añadiría complejidad. |
| **Codec campo a campo** en little-endian | `memcpy` del struct empaquetado | `memcpy` depende de la *endianness* y puede hacer lecturas desalineadas; los structs empaquetados se mantienen como documentación verificada con `_Static_assert` y con un test que compara su imagen de memoria con el codec. |
| **CRC-16/CCITT** | CRC-32; sin CRC | 2 bytes de sobrecarga, distancia de Hamming 4 para ≤ 250 B; sin CRC se pierde la integridad extremo a extremo. |
| **Cola con descarte del más antiguo** | Bloquear al productor; descartar el más nuevo | En monitorización vale más el dato reciente; bloquear el muestreo arruinaría la periodicidad. |
| **Notificaciones a tarea** para despertar `tx` | *Queue set* | Con descarte del más antiguo, el productor saca un elemento sin consumir su entrada en el set: el set crecería hasta disparar un `configASSERT`. |
| **Supervisor propio + TWDT** | Solo el Task Watchdog | Los intervalos son configurables hasta 1 h; el TWDT tiene un único timeout (5 s). El supervisor usa presupuestos por tarea y registra *qué* tarea se colgó. |
| **Umbrales de vivacidad derivados del latido anunciado** | Timeout global fijo | Nodos con distintos intervalos se juzgarían mal con un umbral único. |
| **Reiniciar el seguimiento de secuencia solo si cambia `boot_count`** | Reiniciar en cada JOIN (primera versión) | La simulación mostró que así se ocultaban las pérdidas de un nodo con interferencias que se re-unía varias veces. |
| **RTT medido desde antes de entregar la trama al radio** | Desde el *callback* de envío (primera versión) | La primera versión excluía el tiempo en el aire de la propia trama. |
| **Contrapresión con ACK(BUSY)** cuando el puerto serie no da abasto | Descartar en silencio | El nodo reintenta la misma secuencia más tarde; el dato no se pierde sin que nadie lo sepa, y el estado DEGRADED queda registrado. |
| **Salida JSON Lines** | CSV; binario (CBOR/protobuf) | Autodescriptiva, extensible, tolerante a líneas de log intercaladas, legible con cualquier herramienta. El coste (≈ 486 B por lectura de 17 canales) se compensa con 921 600 baudios. |
| **JSON sin coma flotante** | `printf("%f")` | Salida idéntica byte a byte en el host y en el Xtensa, lo que permite comparar simulador y firmware. |
| **Cifrado con MAC del gateway preconfigurada** | Descubrimiento del gateway + cambio a cifrado después | El nodo debe registrar al gateway como peer cifrado antes de poder descifrar el JOIN_ACK; cualquier otra secuencia tiene condiciones de carrera. |
| **Radio "loopback" para QEMU** | No probar el firmware sin hardware; FreeRTOS POSIX en el PC | Con loopback se ejecuta el binario Xtensa real (pilas medidas realistas); el port POSIX mediría pilas de x86-64. |
| **Simulador en C con el código del firmware** | Simulador en Python | En Python se estaría validando una reimplementación, no el código que se flashea. |

---

## Parte 3 · 10 preguntas de entrevista técnica

**1. ¿Qué diferencia hay entre un mutex y un semáforo binario en FreeRTOS, y cuándo usarías cada uno?**

El mutex tiene *propietario* y **herencia de prioridad**: si una tarea de alta
prioridad espera un mutex tomado por una de baja, esta hereda temporalmente la
prioridad alta y no puede ser expulsada por tareas de prioridad media (evita la
inversión de prioridad). Solo quien lo toma debe liberarlo. El semáforo binario
no tiene propietario ni herencia: sirve para **señalizar** (una ISR o una tarea
avisa a otra). Mutex para proteger recursos compartidos (aquí: estadísticas,
registro, bus I²C); semáforo o, mejor, notificación directa para señalizar.

**2. ¿Por qué no se puede llamar a `vTaskDelay` o hacer un `printf` largo dentro del callback de recepción de ESP-NOW?**

Porque el callback se ejecuta en la **tarea Wi-Fi** (prioridad 23). Bloquearla
retrasa todo el stack de radio (más tramas, ACKs MAC, beacons) y puede provocar
pérdidas o disparar el watchdog. El callback debe hacer trabajo acotado:
decodificar, copiar a una cola con timeout 0 y despertar a la tarea que procesa.
Si la cola está llena, se cuenta la pérdida y se sale.

**3. ¿Cómo dimensionaste las pilas de las tareas?**

Midiendo. Cada latido lleva el *high-water mark* de cada tarea
(`uxTaskGetStackHighWaterMark`), y el firmware se ejecutó en QEMU con el radio
loopback para obtener picos reales en Xtensa: por ejemplo `tx` usó 2876 B y
`heartbeat` 2272 B. Regla: pico × ~1,5 redondeado a 512 B, más margen extra en
rutas no ejercitadas (el interior de `esp_now_send()`, los drivers reales).
Además, canario de desbordamiento y *watchpoint* al final de la pila. Las
mediciones varían hasta unos 200 bytes entre ejecuciones, por eso se mantiene ≥ 1 KB
de margen.

**4. Si ESP-NOW ya tiene ACK, ¿para qué un ACK de aplicación?**

El ACK MAC solo confirma que el radio del peer recibió la trama. No dice si la
aplicación la procesó (la cola podía estar llena), no distingue un duplicado y
no permite contrapresión. El ACK de aplicación lleva un estado (OK, DUPLICATE,
BUSY, REJECTED, INVALID), el número de intento (RTT correcto según Karn) y el
RSSI con que el gateway oyó la trama.

**5. ¿Qué pasa si se pierde el ACK de una trama que el gateway sí recibió?**

El nodo agota el timeout y retransmite la misma secuencia con `attempt + 1`. El
gateway la encuentra en su ventana de 64 secuencias: la clasifica como
**duplicada**, **no** la reenvía al host, pero **sí** responde con
ACK(DUPLICATE), que el nodo trata como éxito. En la simulación de referencia
ocurrió 130 veces. Consecuencia interesante: si se pierden los 4 ACKs, el nodo
cuenta la trama como abandonada aunque el gateway la tenga (le pasó 4 veces al
nodo 1), por eso la pérdida "verdadera" es la que mide el gateway.

**6. ¿Cómo detecta el gateway que un nodo se ha caído sin falsas alarmas?**

Con latidos y dos umbrales derivados del intervalo que el nodo anuncia: tras
2 latidos perdidos + 1 s pasa a SUSPECT y tras 4 + 1 s a OFFLINE. Cualquier
trama válida lo devuelve a ONLINE. El estado intermedio absorbe pérdidas
puntuales, y derivar los umbrales del propio nodo permite intervalos distintos
por nodo. Cada transición se emite como evento JSON con el tiempo de silencio.

**7. ¿Cómo comparas números de secuencia de 16 bits que dan la vuelta?**

Con aritmética de números de serie (RFC 1982): `diff = (int16_t)(a − b)`. Si
`diff > 0`, `a` es posterior, también cuando `a = 0` y `b = 65535`. Es válido
mientras la distancia real sea menor que 32768, lo que un emisor stop-and-wait
garantiza. Los tests recorren tres vueltas completas del contador.

**8. ¿Cómo evitaste que un puerto serie lento hiciera perder datos en silencio?**

Separando el camino del ACK del de salida: la tarea `rx` solo valida, contabiliza
y responde; formatear e imprimir lo hace la tarea `out` desde una cola (en PSRAM).
Si la cola no tiene sitio para los registros de una trama, el gateway responde
**ACK(BUSY)** sin contabilizarla: el nodo espera y reintenta la misma secuencia.
Con la cola al 75 % el gateway pasa a DEGRADED (y vuelve al 25 %, con
histéresis). En la simulación de sobrecarga (12 nodos a 5 Hz, 115 200 baudios)
respondió 14 173 BUSY y las comprobaciones cruzadas del dashboard detectaron las
121 líneas de log que no cupieron.

**9. ¿Qué garantiza y qué no garantiza el cifrado de ESP-NOW en este diseño?**

Garantiza confidencialidad e integridad (CCMP) del tráfico unicast entre nodo y
gateway, e impide inyectar CONFIG falsos sin la LMK. No cifra el broadcast (el
JOIN viaja en claro), no ofrece intercambio de claves (PMK/LMK precompartidas; las
de ejemplo son públicas) y la protección anti-replay se limita a la ventana de
64 tramas. Además exige conocer la MAC del gateway de antemano. El comportamiento
extremo a extremo está pendiente de validar en hardware.

**10. ¿Cómo validaste un firmware de radio sin tener hardware?**

En capas: (1) toda la lógica en C puro con 100 tests Unity bajo ASan/UBSan;
(2) un simulador de eventos discretos que enlaza ese mismo código con un modelo
de canal con pérdidas en ráfaga (Gilbert–Elliott), reproducible por semilla, cuyo
resultado se contrasta con 30 comprobaciones de consistencia; (3) el binario real
del ESP32-S3 ejecutado en QEMU con un radio loopback, con aserciones en CI sobre
joins, tráfico, transiciones y ausencia de *panics*; (4) compilación de todas las
variantes con la imagen oficial de ESP-IDF. Y documentando explícitamente lo que
**no** se ha validado: pérdidas y RSSI reales, cifrado real, drivers de sensores.
