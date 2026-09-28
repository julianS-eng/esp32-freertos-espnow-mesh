/*
 * SPDX-License-Identifier: MIT
 *
 * gw_fsm.h - Gateway lifecycle state machine (pure, table driven).
 *
 *   BOOT --boot_done--> LOAD_REGISTRY --registry_loaded / registry_corrupt--> RADIO_INIT
 *   RADIO_INIT --radio_ready--> RUNNING      RADIO_INIT --radio_failed--> FAULT
 *   FAULT --retry_timer--> RADIO_INIT        FAULT --retries_exhausted--> (restart)
 *   RUNNING --queue_pressure--> DEGRADED     DEGRADED --queue_relieved--> RUNNING
 *
 * In DEGRADED the gateway answers DATA with ACK(BUSY) whenever it cannot
 * forward the reading, so nodes back off instead of silently losing data.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_ST_BOOT = 0,
    GW_ST_LOAD_REGISTRY,
    GW_ST_RADIO_INIT,
    GW_ST_RUNNING,
    GW_ST_DEGRADED,
    GW_ST_FAULT,
    GW_ST_COUNT_
} gw_state_t;

typedef enum {
    GW_EV_BOOT_DONE = 0,
    GW_EV_REGISTRY_LOADED,
    GW_EV_REGISTRY_CORRUPT,
    GW_EV_RADIO_READY,
    GW_EV_RADIO_FAILED,
    GW_EV_RETRY_TIMER,
    GW_EV_QUEUE_PRESSURE,
    GW_EV_QUEUE_RELIEVED,
    GW_EV_COUNT_
} gw_event_t;

typedef struct {
    gw_state_t state;
    unsigned transitions;
    unsigned rejected;
} gw_fsm_t;

void gw_fsm_init(gw_fsm_t *fsm);

/** Pure transition function. Returns false (and leaves *next = cur) for events not valid in @p cur. */
bool gw_fsm_next(gw_state_t cur, gw_event_t ev, gw_state_t *next);

/** Apply @p ev; returns true if the state changed. Invalid events are counted and ignored. */
bool gw_fsm_dispatch(gw_fsm_t *fsm, gw_event_t ev, gw_state_t *from);

/** True in states where frames are accepted from the radio. */
bool gw_fsm_accepts_traffic(gw_state_t s);

const char *gw_state_str(gw_state_t s);
const char *gw_event_str(gw_event_t ev);

#ifdef __cplusplus
}
#endif
