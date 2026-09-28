/*
 * SPDX-License-Identifier: MIT
 */
#include "mesh_tx.h"

#include <string.h>

#include "mesh_protocol.h"
#include "mesh_rand.h"

mesh_retry_policy_t mesh_retry_policy_default(void)
{
    const mesh_retry_policy_t p = {
        .max_attempts = 4,
        .ack_timeout_us = 30000,
        .backoff_base_us = 20000,
        .backoff_max_us = 200000,
        .jitter_us = 10000,
    };
    return p;
}

mesh_retry_policy_t mesh_retry_policy_sanitize(mesh_retry_policy_t p)
{
    if (p.max_attempts < 1) {
        p.max_attempts = 1;
    }
    if (p.max_attempts > MESH_TX_MAX_ATTEMPTS_LIMIT) {
        p.max_attempts = MESH_TX_MAX_ATTEMPTS_LIMIT;
    }
    if (p.ack_timeout_us == 0) {
        p.ack_timeout_us = 1000;
    }
    if (p.backoff_max_us < p.backoff_base_us) {
        p.backoff_max_us = p.backoff_base_us;
    }
    return p;
}

uint32_t mesh_retry_backoff_us(const mesh_retry_policy_t *p, uint8_t next_attempt, uint32_t *rng)
{
    uint32_t shift = (next_attempt >= 2) ? (uint32_t)(next_attempt - 2) : 0u;
    if (shift > 16) {
        shift = 16;
    }
    uint64_t d = (uint64_t)p->backoff_base_us << shift;
    if (d > p->backoff_max_us) {
        d = p->backoff_max_us;
    }
    return (uint32_t)d + mesh_rand_below(rng, p->jitter_us);
}

void mesh_tx_init(mesh_tx_t *tx, const mesh_retry_policy_t *policy, uint32_t seed)
{
    memset(tx, 0, sizeof(*tx));
    tx->policy = mesh_retry_policy_sanitize(policy != NULL ? *policy : mesh_retry_policy_default());
    tx->rng = seed;
    tx->state = MESH_TX_IDLE;
}

bool mesh_tx_busy(const mesh_tx_t *tx)
{
    return tx->state == MESH_TX_WAIT_ACK || tx->state == MESH_TX_BACKOFF;
}

mesh_tx_action_t mesh_tx_start(mesh_tx_t *tx, uint16_t seq)
{
    tx->seq = seq;
    tx->attempt = 1;
    tx->rtt_us = 0;
    tx->acked_status = 0;
    tx->deadline_us = 0;
    memset(tx->sent_at_us, 0, sizeof(tx->sent_at_us));
    tx->state = MESH_TX_WAIT_ACK; /* becomes meaningful once mesh_tx_sent() arms the timer */
    return MESH_TX_ACTION_SEND;
}

void mesh_tx_sent(mesh_tx_t *tx, uint64_t now_us)
{
    if (tx->attempt <= MESH_TX_MAX_ATTEMPTS_LIMIT) {
        tx->sent_at_us[tx->attempt] = now_us;
    }
    tx->state = MESH_TX_WAIT_ACK;
    tx->deadline_us = now_us + tx->policy.ack_timeout_us;
}

static mesh_tx_action_t schedule_retry_or_fail(mesh_tx_t *tx, uint64_t now_us)
{
    if (tx->attempt >= tx->policy.max_attempts) {
        tx->state = MESH_TX_FAILED;
        return MESH_TX_ACTION_FAILED;
    }
    tx->state = MESH_TX_BACKOFF;
    tx->deadline_us = now_us + mesh_retry_backoff_us(&tx->policy, (uint8_t)(tx->attempt + 1), &tx->rng);
    return MESH_TX_ACTION_WAIT;
}

mesh_tx_action_t mesh_tx_on_ack(mesh_tx_t *tx, uint16_t acked_seq, uint8_t acked_attempt, uint8_t status,
                                uint64_t now_us)
{
    if (!mesh_tx_busy(tx) || acked_seq != tx->seq) {
        return MESH_TX_ACTION_WAIT; /* stale or foreign ACK */
    }
    if (status == MESH_ACK_BUSY) {
        if (tx->state == MESH_TX_WAIT_ACK) {
            return schedule_retry_or_fail(tx, now_us);
        }
        return MESH_TX_ACTION_WAIT;
    }
    /* OK, DUPLICATE, REJECTED and INVALID all terminate delivery of this seq;
     * the caller inspects acked_status to decide on follow-up (e.g. re-join). */
    tx->acked_status = status;
    uint8_t a = acked_attempt;
    if (a == 0 || a > tx->attempt || a > MESH_TX_MAX_ATTEMPTS_LIMIT || tx->sent_at_us[a] == 0) {
        a = tx->attempt; /* defensive: fall back to latest attempt */
    }
    const uint64_t sent = tx->sent_at_us[a];
    tx->rtt_us = (now_us > sent) ? (uint32_t)(now_us - sent) : 0u;
    tx->state = MESH_TX_DONE;
    return MESH_TX_ACTION_DONE;
}

mesh_tx_action_t mesh_tx_on_radio_fail(mesh_tx_t *tx, uint64_t now_us)
{
    if (tx->state != MESH_TX_WAIT_ACK) {
        return MESH_TX_ACTION_WAIT;
    }
    return schedule_retry_or_fail(tx, now_us);
}

mesh_tx_action_t mesh_tx_poll(mesh_tx_t *tx, uint64_t now_us)
{
    switch (tx->state) {
    case MESH_TX_WAIT_ACK:
        if (now_us >= tx->deadline_us) {
            return schedule_retry_or_fail(tx, now_us);
        }
        return MESH_TX_ACTION_WAIT;
    case MESH_TX_BACKOFF:
        if (now_us >= tx->deadline_us) {
            tx->attempt++;
            return MESH_TX_ACTION_SEND;
        }
        return MESH_TX_ACTION_WAIT;
    case MESH_TX_DONE:
        return MESH_TX_ACTION_DONE;
    case MESH_TX_FAILED:
        return MESH_TX_ACTION_FAILED;
    case MESH_TX_IDLE:
    default:
        return MESH_TX_ACTION_WAIT;
    }
}

const char *mesh_tx_state_str(mesh_tx_state_t s)
{
    switch (s) {
    case MESH_TX_IDLE: return "idle";
    case MESH_TX_WAIT_ACK: return "wait_ack";
    case MESH_TX_BACKOFF: return "backoff";
    case MESH_TX_DONE: return "done";
    case MESH_TX_FAILED: return "failed";
    default: return "?";
    }
}
