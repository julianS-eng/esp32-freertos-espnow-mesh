/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_protocol.h - Wire format shared by the sensor node and the gateway.
 *
 * This header is intentionally free of ESP-IDF dependencies so that the exact
 * same code is compiled into both firmwares *and* into the host test suite and
 * the host mesh simulator.
 *
 * Wire layout (all multi-byte integers are little-endian):
 *
 *   +--------+---------+------+-------+---------+-----+-----------+---------+-------------+---------+-------+
 *   | magic  | version | type | flags | node_id | seq | uptime_ms | attempt | payload_len | payload | crc16 |
 *   |  u8    |   u8    |  u8  |  u8   |  u16    | u16 |   u32     |   u8    |     u8      |  0..234 |  u16  |
 *   +--------+---------+------+-------+---------+-----+-----------+---------+-------------+---------+-------+
 *    \______________________ 14-byte header _______________________________________/
 *
 * The CRC is CRC-16/CCITT-FALSE computed over header + payload. A full frame
 * never exceeds ESP_NOW_MAX_DATA_LEN (250 bytes, ESP-NOW v1.0).
 *
 * The packed structs below document the layout and are verified with
 * static assertions. The codec (mesh_encode / mesh_decode) nevertheless
 * serialises field by field, which keeps it independent of host endianness and
 * never performs unaligned loads from radio buffers.
 *
 * See docs/PROTOCOL.md for the full specification.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

#define MESH_PROTO_MAGIC 0x4Du   /**< 'M' - first byte of every frame. */
#define MESH_PROTO_VERSION 1u    /**< Bumped on any incompatible change. */
#define MESH_FRAME_MAX 250u      /**< ESP_NOW_MAX_DATA_LEN for ESP-NOW v1. */
#define MESH_HEADER_SIZE 14u
#define MESH_TRAILER_SIZE 2u
#define MESH_FRAME_OVERHEAD (MESH_HEADER_SIZE + MESH_TRAILER_SIZE)
#define MESH_PAYLOAD_MAX (MESH_FRAME_MAX - MESH_FRAME_OVERHEAD) /* 234 */

#define MESH_MAX_CHANNELS 20u    /**< Max measurement channels in one DATA frame. */
#define MESH_HB_MAX_TASKS 6u     /**< Stack high-water marks carried in a heartbeat. */
#define MESH_GATEWAY_NODE_ID 0x0000u
#define MESH_BROADCAST_NODE_ID 0xFFFFu

/** Header flags. */
#define MESH_FLAG_ACK_REQ 0x01u   /**< Sender expects an application-level ACK. */
#define MESH_FLAG_ENCRYPTED 0x02u /**< Informational: frame was sent over an encrypted ESP-NOW peer. */

/** Message types. */
typedef enum {
    MESH_MSG_JOIN = 1,      /**< node -> gateway (broadcast): announce / re-announce after boot. */
    MESH_MSG_JOIN_ACK = 2,  /**< gateway -> node (unicast): accept or reject a join. */
    MESH_MSG_DATA = 3,      /**< node -> gateway: one sensor reading (N channels). */
    MESH_MSG_HEARTBEAT = 4, /**< node -> gateway: liveness + health telemetry. */
    MESH_MSG_ACK = 5,       /**< either direction: application-level acknowledgement. */
    MESH_MSG_CONFIG = 6,    /**< gateway -> node: change a persisted configuration key. */
} mesh_msg_type_t;

/** Physical quantities carried in DATA channels. Values are fixed-point x1000. */
typedef enum {
    MESH_Q_GAS_ADC_MV = 1,   /**< MQ-2 analogue output at the ADC pin, mV. */
    MESH_Q_GAS_RS_R0 = 2,    /**< MQ-2 sensing resistance ratio Rs/R0. */
    MESH_Q_GAS_PPM = 3,      /**< MQ-2 LPG-equivalent concentration, ppm. */
    MESH_Q_MOTOR_RPM = 4,    /**< Shaft speed from AS5600, rev/min. */
    MESH_Q_MOTOR_ANGLE = 5,  /**< Shaft angle from AS5600, degrees [0, 360). */
    MESH_Q_MOTOR_DUTY = 6,   /**< Commanded PWM duty cycle, percent. */
    MESH_Q_ENC_STATUS = 7,   /**< AS5600 STATUS register (MD/ML/MH bits). */
    MESH_Q_ENC_AGC = 8,      /**< AS5600 automatic gain control value. */
    MESH_Q_ACCEL_X = 9,      /**< MPU-6050 acceleration, g. */
    MESH_Q_ACCEL_Y = 10,
    MESH_Q_ACCEL_Z = 11,
    MESH_Q_GYRO_X = 12,      /**< MPU-6050 angular rate, deg/s (bias-compensated). */
    MESH_Q_GYRO_Y = 13,
    MESH_Q_GYRO_Z = 14,
    MESH_Q_IMU_TEMP = 15,    /**< MPU-6050 die temperature, degC. */
    MESH_Q_PITCH = 16,       /**< Tilt from accelerometer, degrees. */
    MESH_Q_ROLL = 17,
    MESH_Q_COUNT_ /* sentinel */
} mesh_quantity_t;

/** Bitmask of the sensor backends that produced a reading. */
#define MESH_BACKEND_SIM 0x01u
#define MESH_BACKEND_MQ2 0x02u
#define MESH_BACKEND_MOTOR 0x04u
#define MESH_BACKEND_MPU6050 0x08u

/** Reading status bits (mesh_data_payload_t.status). */
#define MESH_STATUS_WARMING_UP 0x01u  /**< MQ-2 heater not yet stable. */
#define MESH_STATUS_SENSOR_FAULT 0x02u /**< At least one backend failed this cycle. */
#define MESH_STATUS_CALIBRATING 0x04u

/** ACK status codes. */
typedef enum {
    MESH_ACK_OK = 0,
    MESH_ACK_DUPLICATE = 1, /**< Already processed (our previous ACK was lost). Sender treats as success. */
    MESH_ACK_BUSY = 2,      /**< Receiver queue full; sender should retry later. */
    MESH_ACK_REJECTED = 3,  /**< Unknown node / not joined; sender should re-join. */
    MESH_ACK_INVALID = 4,   /**< Semantically invalid request (e.g. CONFIG value out of range). */
} mesh_ack_status_t;

/** JOIN_ACK status codes. */
typedef enum {
    MESH_JOIN_ACCEPTED = 0,
    MESH_JOIN_REJECTED_FULL = 1,
    MESH_JOIN_REJECTED_VERSION = 2,
    MESH_JOIN_REJECTED_ID_CONFLICT = 3, /**< Node id already owned by another MAC. */
} mesh_join_status_t;

/** CONFIG keys. */
typedef enum {
    MESH_CFG_REPORT_INTERVAL_MS = 1,
    MESH_CFG_HEARTBEAT_INTERVAL_MS = 2,
    MESH_CFG_REBOOT = 3,
} mesh_config_key_t;

/* ------------------------------------------------------------------------- */
/* Packed wire structures                                                    */
/* ------------------------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#define MESH_PACKED __attribute__((packed))
#else
#error "A compiler supporting __attribute__((packed)) is required"
#endif

typedef struct MESH_PACKED {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint16_t node_id;   /**< Source node id (gateway = 0). */
    uint16_t seq;       /**< Per-sender sequence number, wraps at 65535. */
    uint32_t uptime_ms; /**< Sender uptime when the frame was built. */
    uint8_t attempt;    /**< 1 for the first transmission, incremented on every retry. */
    uint8_t payload_len;
} mesh_header_t;

typedef struct MESH_PACKED {
    uint8_t quantity;    /**< mesh_quantity_t */
    int32_t value_milli; /**< value x 1000 */
} mesh_channel_t;

typedef struct MESH_PACKED {
    uint8_t backend;      /**< MESH_BACKEND_* mask */
    uint8_t status;       /**< MESH_STATUS_* mask */
    uint32_t last_rtt_us; /**< Round-trip time of the previous acknowledged frame, 0 if none. */
    uint8_t channel_count;
    mesh_channel_t channels[MESH_MAX_CHANNELS];
} mesh_data_payload_t;
#define MESH_DATA_FIXED_SIZE 7u

typedef struct MESH_PACKED {
    uint32_t fw_version;        /**< (major << 16) | (minor << 8) | patch */
    uint32_t boot_count;        /**< Persisted in NVS; lets the gateway detect reboots. */
    uint32_t report_interval_ms;
    uint32_t heartbeat_interval_ms;
    uint8_t backend;            /**< MESH_BACKEND_* mask */
    uint8_t reset_reason;       /**< esp_reset_reason_t of the node */
} mesh_join_payload_t;

typedef struct MESH_PACKED {
    uint8_t status;             /**< mesh_join_status_t */
    uint8_t wifi_channel;
    uint32_t offline_timeout_ms; /**< After this silence the gateway declares the node offline. */
} mesh_join_ack_payload_t;

typedef struct MESH_PACKED {
    uint16_t acked_seq;
    uint8_t acked_type;
    uint8_t acked_attempt; /**< Echo of the attempt being acked (Karn-safe RTT). */
    uint8_t status;        /**< mesh_ack_status_t */
    int8_t rssi;           /**< RSSI (dBm) of the acked frame as seen by the receiver. */
} mesh_ack_payload_t;

typedef struct MESH_PACKED {
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t tx_ok;       /**< Frames acknowledged by the gateway. */
    uint32_t tx_retries;  /**< Retransmissions (attempt > 1). */
    uint32_t tx_failed;   /**< Frames abandoned after max attempts. */
    uint32_t queue_drops; /**< Readings dropped because the TX queue was full. */
    uint32_t rtt_avg_us;  /**< Mean RTT over the last heartbeat period. */
    uint32_t rtt_max_us;  /**< Max RTT over the last heartbeat period. */
    int8_t last_ack_rssi;
    uint8_t task_count;
    uint16_t stack_hwm[MESH_HB_MAX_TASKS]; /**< uxTaskGetStackHighWaterMark (bytes), fixed task order. */
} mesh_heartbeat_payload_t;

typedef struct MESH_PACKED {
    uint8_t key;    /**< mesh_config_key_t */
    uint32_t value;
} mesh_config_payload_t;

/* Layout checks: any accidental change to the wire format fails the build. */
_Static_assert(sizeof(mesh_header_t) == MESH_HEADER_SIZE, "header size");
_Static_assert(sizeof(mesh_channel_t) == 5, "channel size");
_Static_assert(sizeof(mesh_data_payload_t) == MESH_DATA_FIXED_SIZE + 5 * MESH_MAX_CHANNELS, "data size");
_Static_assert(sizeof(mesh_join_payload_t) == 18, "join size");
_Static_assert(sizeof(mesh_join_ack_payload_t) == 6, "join_ack size");
_Static_assert(sizeof(mesh_ack_payload_t) == 6, "ack size");
_Static_assert(sizeof(mesh_heartbeat_payload_t) == 38 + 2 * MESH_HB_MAX_TASKS, "heartbeat size");
_Static_assert(sizeof(mesh_config_payload_t) == 5, "config size");
_Static_assert(sizeof(mesh_data_payload_t) <= MESH_PAYLOAD_MAX, "data payload must fit ESP-NOW");
_Static_assert(offsetof(mesh_header_t, seq) == 6, "seq offset");
_Static_assert(offsetof(mesh_header_t, payload_len) == 13, "payload_len offset");

/* ------------------------------------------------------------------------- */
/* Decoded frame                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    mesh_header_t hdr;
    union {
        mesh_join_payload_t join;
        mesh_join_ack_payload_t join_ack;
        mesh_data_payload_t data;
        mesh_heartbeat_payload_t hb;
        mesh_ack_payload_t ack;
        mesh_config_payload_t config;
    } u;
} mesh_frame_t;

typedef enum {
    MESH_OK = 0,
    MESH_ERR_ARG = -1,
    MESH_ERR_TOO_SHORT = -2,
    MESH_ERR_MAGIC = -3,
    MESH_ERR_VERSION = -4,
    MESH_ERR_LENGTH = -5,
    MESH_ERR_CRC = -6,
    MESH_ERR_TYPE = -7,
    MESH_ERR_PAYLOAD = -8,
    MESH_ERR_NO_SPACE = -9,
} mesh_err_t;

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

/**
 * Initialise a frame header with magic, version, type and addressing.
 * Payload fields must be filled by the caller afterwards.
 */
void mesh_frame_init(mesh_frame_t *f, mesh_msg_type_t type, uint16_t node_id, uint16_t seq,
                     uint32_t uptime_ms, uint8_t flags);

/** Size in bytes of the serialised payload for @p f (depends on type and channel count). */
mesh_err_t mesh_payload_size(const mesh_frame_t *f, size_t *out_size);

/**
 * Serialise @p f into @p buf. hdr.magic, hdr.version and hdr.payload_len are
 * written by the encoder (the values in @p f are ignored); the CRC is appended.
 *
 * @return MESH_OK and the total frame length in @p out_len, or an error.
 */
mesh_err_t mesh_encode(const mesh_frame_t *f, uint8_t *buf, size_t cap, size_t *out_len);

/**
 * Validate and deserialise a received frame. Checks, in this order: minimum
 * length, magic, version, declared vs. actual length, CRC, message type and
 * type-specific payload size. On error @p out is left in an unspecified state.
 */
mesh_err_t mesh_decode(const uint8_t *buf, size_t len, mesh_frame_t *out);

/** Patch the attempt counter of an already-encoded frame and refresh its CRC. */
mesh_err_t mesh_frame_set_attempt(uint8_t *buf, size_t len, uint8_t attempt);

/** Append a channel to a DATA frame. Returns MESH_ERR_NO_SPACE when full. */
mesh_err_t mesh_data_add(mesh_frame_t *f, mesh_quantity_t q, int32_t value_milli);

const char *mesh_err_str(mesh_err_t err);
const char *mesh_msg_type_str(uint8_t type);
/** Stable snake_case key used in the gateway JSON output, or NULL if unknown. */
const char *mesh_quantity_key(uint8_t quantity);

/* ------------------------------------------------------------------------- */
/* Sequence-number arithmetic (RFC 1982 serial numbers, 16 bit)              */
/* ------------------------------------------------------------------------- */

/** Signed distance a - b in the 16-bit serial-number space. */
static inline int32_t mesh_seq_diff(uint16_t a, uint16_t b)
{
    return (int32_t)(int16_t)(uint16_t)(a - b);
}

/** True if @p a is strictly newer than @p b (handles wrap-around). */
static inline bool mesh_seq_newer(uint16_t a, uint16_t b)
{
    return mesh_seq_diff(a, b) > 0;
}

#ifdef __cplusplus
}
#endif
