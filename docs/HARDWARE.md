# Hardware and wiring

Target module: **ESP32-S3-WROOM-1 N16R8** (16 MB quad flash, 8 MB octal PSRAM),
e.g. on an ESP32-S3-DevKitC-1. All pins are configurable in `idf.py menuconfig`
→ *Sensor node: sensor backends*. Defaults avoid the pins that are unavailable on
N16R8 modules: GPIO26–32 (flash), GPIO33–37 (octal PSRAM), GPIO19/20 (USB),
and the strapping pins GPIO0/3/45/46.

> None of the wiring below has been validated on real hardware in this project
> yet – see the README section "Validated in simulation vs. pending hardware
> validation". Double-check voltages before powering anything.

## Default pin map

| Function | GPIO | Notes |
|---|---:|---|
| MQ-2 analogue out (via divider) | 4 | ADC1_CH3. Only ADC1 (GPIO1–10) is allowed: ADC2 is unusable while Wi-Fi is on. |
| I²C SDA | 8 | shared by AS5600 (0x36) and MPU-6050 (0x68), 400 kHz |
| I²C SCL | 9 | internal pull-ups enabled; the breakout boards also carry pull-ups |
| Motor PWM (H-bridge EN/IN1) | 5 | LEDC, 20 kHz, 10-bit |
| Motor direction (H-bridge IN2) | 6 | set to −1 in menuconfig if unused |

## MQ-2 gas sensor (FC-22 style module)

* Powered from **5 V** (the datasheet heater budget is ≤ 800 mW, i.e. up to
  ≈ 160 mA – do not take it from the 3.3 V regulator). Common ground with the
  ESP32-S3.
* `AO` swings up to ≈ 5 V; the ESP32-S3 ADC tolerates 3.3 V. Use a divider:
  10 kΩ from `AO` to GPIO4 and 20 kΩ from GPIO4 to GND (defaults in Kconfig:
  `SENSOR_MQ2_DIVIDER_TOP_OHM` / `_BOTTOM_OHM`; ratio 1.5).
* Set `SENSOR_MQ2_RL_OHM` to the load resistor on *your* module (often 1 kΩ,
  sometimes 4.7 kΩ or 10 kΩ – measure it). The Rs formula depends on it.
* Readings are flagged `warming_up` for `SENSOR_MQ2_WARMUP_S` (60 s default; the
  datasheet recommends 24–48 h of burn-in for stable absolute values).
* With `SENSOR_MQ2_AUTOCAL` the firmware averages 50 readings in clean air after
  warm-up, computes R0 = Rs / 9.83 and stores it in NVS (namespace `mq2`). To
  recalibrate, erase that key (`idf.py erase-flash` or an NVS tool).
* `gas_ppm` is an **LPG-equivalent indication** from a straight log-log line
  through two points of the datasheet curve (200 ppm → Rs/R0 = 1.6, 10 000 ppm →
  0.26). It is not a calibrated concentration, and the MQ-2 is cross-sensitive to
  smoke, H₂, alcohol, humidity and temperature.

## R380 DC motor (6–24 V) + AS5600 encoder

```
            +V_motor (6–24 V, own supply) ───────┐
                                                 │
ESP32-S3 GPIO5 (PWM) ──► EN/IN1 ┐           ┌────┴────┐
ESP32-S3 GPIO6 (DIR) ──► IN2    ├─ H-bridge ─┤  R380   │──(magnet on shaft end)
ESP32-S3 GND ─────────── GND    ┘  (e.g.     └─────────┘      ▲ 0.5–3 mm air gap
                                   DRV8871,                    │
                                   BTS7960)            AS5600 breakout (3.3 V, I²C 0x36)
```

* The motor **must not** be powered from the ESP32-S3 board. Use an H-bridge
  rated for the stall current of your R380 variant (several amps at 12 V) with
  flyback protection, and a common ground for the logic signals.
* PWM is 20 kHz (inaudible), 10-bit, with a linear soft-start over
  `SENSOR_MOTOR_RAMP_MS` (2 s) to limit inrush current.
* The AS5600 needs a **diametrically magnetised** magnet centred on the shaft.
  Its STATUS register is reported as `enc_status`: `0x20` = magnet detected,
  `0x10` = too weak, `0x08` = too strong; `enc_agc` should sit mid-range.
* Speed measurement: the angle is read once per RTOS tick (1 kHz) for
  `SENSOR_MOTOR_RPM_WINDOW_MS` (50 ms) and unwrapped. This is unambiguous only if
  the shaft turns less than half a revolution per sample, i.e. **< 30 000 rpm at
  the encoder**. A bare R380 at 12–24 V can exceed that under no load – mount
  the encoder after a gearbox, or use the AS5600 PWM/analogue output with a
  timer capture instead (future work).
* Control is open-loop (fixed duty). Closed-loop speed control (PI on rpm) is
  future work.

## GY-521 (MPU-6050)

* VCC 3.3 V (the breakout's regulator also accepts 5 V), GND, SDA → GPIO8,
  SCL → GPIO9, AD0 → GND for address 0x68 (`SENSOR_MPU6050_ADDR`).
* Configuration: ±4 g, ±500 °/s, DLPF 44 Hz, 100 Hz sample rate.
* The gyro bias is estimated at boot from 200 samples – keep the board still for
  ≈ 2 s after power-up, or set `SENSOR_MPU6050_GYRO_CAL_SAMPLES` to 0.
* Many "MPU-6050" breakouts carry clones (WHO_AM_I 0x70/0x72); the driver logs a
  warning and continues.

## Gateway

Any ESP32-S3 board; only the USB-UART console is used. The JSON stream runs at
**921 600 baud** – set the same rate in `meshdash live --baud` or your terminal.
Command input is supported on the UART console (the default); with the
USB-Serial-JTAG console the gateway still prints JSON but ignores commands.
