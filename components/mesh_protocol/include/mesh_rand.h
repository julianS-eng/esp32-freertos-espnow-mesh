/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_rand.h - xorshift32 PRNG. Deterministic, seedable and allocation free.
 * Used for retry jitter and by the simulated sensor backend; it is NOT a
 * cryptographic generator (use esp_random() for anything security related).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Advance the state and return the next 32-bit value. A zero state is remapped. */
static inline uint32_t mesh_rand_next(uint32_t *state)
{
    uint32_t x = (*state != 0u) ? *state : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/** Uniform integer in [0, bound) (bound > 0). Slight modulo bias is acceptable for jitter. */
static inline uint32_t mesh_rand_below(uint32_t *state, uint32_t bound)
{
    return (bound == 0u) ? 0u : mesh_rand_next(state) % bound;
}

/** Uniform double in [0, 1). */
static inline double mesh_rand_unit(uint32_t *state)
{
    return (double)mesh_rand_next(state) / 4294967296.0;
}

#ifdef __cplusplus
}
#endif
