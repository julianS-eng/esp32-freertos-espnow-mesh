/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_tx.h - Sender-side stop-and-wait reliability state machine.
 *
 * The state machine is pure: it never sleeps, sends or reads a clock. The
 * caller feeds it events (frame sent, ACK received, radio-level failure) with
 * a monotonic timestamp in microseconds and asks it what to do next. The
 * firmware drives it from a FreeRTOS task using esp_timer_get_time(); the host
 * simulator drives it with virtual time. Both therefore run identical logic.
 *
 *            start()                 ack(seq,attempt)
 *   IDLE ------------> WAIT_ACK -------------------------> DONE
 *                        |   ^
 *     timeout or radio   |   | backoff expired -> ACTION_SEND (attempt++)
 *     failure, attempts  v   |
 *     left            BACKOFF
 *                        |
 *     attempts exhausted +-----------------------------> FAILED
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_TX_MAX_ATTEMPTS_LIMIT 8

typedef struct {
    uint8_t max_attempts;     /**< Total transmissions including the first (1..8). */
    uint32_t ack_timeout_us;  /**< Wait for an ACK after each transmission. */
    uint32_t backoff_base_us; /**< Delay before retry n is base * 2^(n-1), capped, plus jitter. */
    uint32_t backoff_max_us;
    uint32_t jitter_us;       /**< Uniform random extra delay in [0, jitter_us). */
} mesh_retry_policy_t;

typedef enum {
    MESH_TX_IDLE = 0,
    MESH_TX_WAIT_ACK,
    MESH_TX_BACKOFF,
    MESH_TX_DONE,
    MESH_TX_FAILED,
} mesh_tx_state_t;

typedef enum {
    MESH_TX_ACTION_WAIT = 0, /**< Nothing to do before mesh_tx_deadline(). */
    MESH_TX_ACTION_SEND,     /**< (Re)transmit now with attempt = mesh_tx_t.attempt, then call mesh_tx_sent(). */
    MESH_TX_ACTION_DONE,     /**< Delivered; RTT available in mesh_tx_t.rtt_us. */
    MESH_TX_ACTION_FAILED,   /**< Gave up after max_attempts. */
} mesh_tx_action_t;

typedef struct {
    mesh_retry_policy_t policy;
    mesh_tx_state_t state;
    uint16_t seq;
    uint8_t attempt;         /**< Attempt number of the latest (or next) transmission. */
    uint8_t acked_status;    /**< mesh_ack_status_t of the accepting ACK. */
    uint64_t sent_at_us[MESH_TX_MAX_ATTEMPTS_LIMIT + 1];
    uint64_t deadline_us;
    uint32_t rtt_us;         /**< Valid in DONE: send(acked attempt) -> ACK. */
    uint32_t rng;
} mesh_tx_t;

/** Default policy: 4 attempts, 30 ms ACK timeout, 20/40/80 ms backoff (+<10 ms jitter). */
mesh_retry_policy_t mesh_retry_policy_default(void);

/** Clamp a policy to sane limits (attempts 1..8, non-zero timeout). */
mesh_retry_policy_t mesh_retry_policy_sanitize(mesh_retry_policy_t p);

/** Delay to wait before attempt @p next_attempt (>= 2). Pure apart from advancing @p rng. */
uint32_t mesh_retry_backoff_us(const mesh_retry_policy_t *p, uint8_t next_attempt, uint32_t *rng);

void mesh_tx_init(mesh_tx_t *tx, const mesh_retry_policy_t *policy, uint32_t seed);

/** Begin delivering frame @p seq. Returns ACTION_SEND for attempt 1. */
mesh_tx_action_t mesh_tx_start(mesh_tx_t *tx, uint16_t seq);

/** Record that attempt tx->attempt was handed to the radio at @p now_us. */
void mesh_tx_sent(mesh_tx_t *tx, uint64_t now_us);

/**
 * An ACK arrived. Stale ACKs (other seq, or while idle) are ignored and return
 * ACTION_WAIT. MESH_ACK_BUSY is treated like a timeout (retry after backoff).
 */
mesh_tx_action_t mesh_tx_on_ack(mesh_tx_t *tx, uint16_t acked_seq, uint8_t acked_attempt, uint8_t status,
                                uint64_t now_us);

/** The radio reported a MAC-layer failure for the latest attempt; skip the ACK wait. */
mesh_tx_action_t mesh_tx_on_radio_fail(mesh_tx_t *tx, uint64_t now_us);

/** Advance timers. Call whenever the wait for an event returns (timeout or not). */
mesh_tx_action_t mesh_tx_poll(mesh_tx_t *tx, uint64_t now_us);

/** Absolute time of the next timer expiry (valid in WAIT_ACK and BACKOFF). */
static inline uint64_t mesh_tx_deadline(const mesh_tx_t *tx)
{
    return tx->deadline_us;
}

bool mesh_tx_busy(const mesh_tx_t *tx);

const char *mesh_tx_state_str(mesh_tx_state_t s);

#ifdef __cplusplus
}
#endif
