"""Host-side tooling for the ESP32-S3 FreeRTOS + ESP-NOW sensor mesh.

The package parses the gateway's JSON Lines stream (live from a serial port or
from a file produced by the firmware or by the host simulator), computes
activity, packet-loss and latency metrics, renders a live terminal dashboard
and generates the figures and tables used in the documentation.
"""

__all__ = ["__version__"]

__version__ = "1.0.0"
