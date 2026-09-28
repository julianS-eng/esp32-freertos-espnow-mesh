/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_seq.h - Receiver-side sequence tracking: duplicate suppression, loss
 * accounting and reboot detection, using a 64-entry sliding window (the same
 * idea as the IPsec anti-replay window of RFC 4303, adapted to 16-bit
 * serial-number arithmetic).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_SEQ_WINDOW 64

typedef enum {
    MESH_SEQ_NEW = 0,       /**< Newest frame so far; accept. */
    MESH_SEQ_LATE = 1,      /**< Older than newest but never seen (reordered); accept. */
    MESH_SEQ_DUPLICATE = 2, /**< Already seen (retransmission after a lost ACK); re-ACK, do not re-process. */
    MESH_SEQ_RESET = 3,     /**< Jump too far back: sender restarted its counter; tracker re-initialised, accept. */
} mesh_seq_result_t;

typedef struct {
    bool initialized;
    uint16_t highest;     /**< Newest sequence number accepted. */
    uint64_t window;      /**< Bit i set => (highest - i) was received. */
    uint32_t received;    /**< Unique frames accepted. */
    uint32_t duplicates;
    uint32_t lost;        /**< Current estimate; decremented when a late frame fills a gap. */
    uint32_t reordered;
    uint32_t resets;
} mesh_seq_tracker_t;

void mesh_seq_tracker_reset(mesh_seq_tracker_t *t);

/**
 * Account for a received sequence number.
 * @param gap_out  optional; number of frames newly counted as lost by this update.
 */
mesh_seq_result_t mesh_seq_tracker_update(mesh_seq_tracker_t *t, uint16_t seq, uint32_t *gap_out);

/** Loss ratio in parts-per-million: lost / (received + lost). 0 when nothing was expected. */
uint32_t mesh_seq_loss_ppm(const mesh_seq_tracker_t *t);

#ifdef __cplusplus
}
#endif
