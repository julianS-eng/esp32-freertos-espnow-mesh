/*
 * SPDX-License-Identifier: MIT
 */
#include "mesh_seq.h"

#include <string.h>

#include "mesh_protocol.h"

void mesh_seq_tracker_reset(mesh_seq_tracker_t *t)
{
    if (t != NULL) {
        memset(t, 0, sizeof(*t));
    }
}

static void start_at(mesh_seq_tracker_t *t, uint16_t seq)
{
    t->initialized = true;
    t->highest = seq;
    t->window = 1u;
    t->received++;
}

mesh_seq_result_t mesh_seq_tracker_update(mesh_seq_tracker_t *t, uint16_t seq, uint32_t *gap_out)
{
    uint32_t gap = 0;
    mesh_seq_result_t res;

    if (!t->initialized) {
        start_at(t, seq);
        res = MESH_SEQ_NEW;
    } else {
        const int32_t diff = mesh_seq_diff(seq, t->highest);
        if (diff > 0) {
            gap = (uint32_t)diff - 1u;
            t->window = (diff < MESH_SEQ_WINDOW) ? ((t->window << diff) | 1u) : 1u;
            t->highest = seq;
            t->received++;
            t->lost += gap;
            res = MESH_SEQ_NEW;
        } else if (diff == 0) {
            t->duplicates++;
            res = MESH_SEQ_DUPLICATE;
        } else if (-diff < MESH_SEQ_WINDOW) {
            const uint64_t bit = (uint64_t)1u << (uint32_t)(-diff);
            if (t->window & bit) {
                t->duplicates++;
                res = MESH_SEQ_DUPLICATE;
            } else {
                t->window |= bit;
                t->received++;
                t->reordered++;
                if (t->lost > 0) {
                    t->lost--;
                }
                res = MESH_SEQ_LATE;
            }
        } else {
            /* Far behind the window: the only sane explanation for a
             * stop-and-wait sender is a restart without a JOIN reaching us. */
            t->resets++;
            start_at(t, seq);
            res = MESH_SEQ_RESET;
        }
    }
    if (gap_out != NULL) {
        *gap_out = gap;
    }
    return res;
}

uint32_t mesh_seq_loss_ppm(const mesh_seq_tracker_t *t)
{
    const uint64_t expected = (uint64_t)t->received + t->lost;
    if (expected == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)t->lost * 1000000u) / expected);
}
