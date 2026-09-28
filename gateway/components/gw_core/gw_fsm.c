/*
 * SPDX-License-Identifier: MIT
 */
#include "gw_fsm.h"

#include <stddef.h>

#define X GW_ST_COUNT_ /* "no transition" marker */

/* Rows: current state. Columns: event. */
static const gw_state_t s_table[GW_ST_COUNT_][GW_EV_COUNT_] = {
    /*                  BOOT_DONE           REG_LOADED        REG_CORRUPT       RADIO_READY  RADIO_FAILED RETRY_TIMER    Q_PRESSURE     Q_RELIEVED */
    [GW_ST_BOOT]          = {GW_ST_LOAD_REGISTRY, X,                X,                X,           X,           X,             X,             X},
    [GW_ST_LOAD_REGISTRY] = {X,                   GW_ST_RADIO_INIT, GW_ST_RADIO_INIT, X,           X,           X,             X,             X},
    [GW_ST_RADIO_INIT]    = {X,                   X,                X,                GW_ST_RUNNING, GW_ST_FAULT, X,           X,             X},
    [GW_ST_RUNNING]       = {X,                   X,                X,                X,           X,           X,             GW_ST_DEGRADED, X},
    [GW_ST_DEGRADED]      = {X,                   X,                X,                X,           X,           X,             X,             GW_ST_RUNNING},
    [GW_ST_FAULT]         = {X,                   X,                X,                X,           X,           GW_ST_RADIO_INIT, X,          X},
};

void gw_fsm_init(gw_fsm_t *fsm)
{
    fsm->state = GW_ST_BOOT;
    fsm->transitions = 0;
    fsm->rejected = 0;
}

bool gw_fsm_next(gw_state_t cur, gw_event_t ev, gw_state_t *next)
{
    *next = cur;
    if ((unsigned)cur >= GW_ST_COUNT_ || (unsigned)ev >= GW_EV_COUNT_) {
        return false;
    }
    const gw_state_t n = s_table[cur][ev];
    if (n == X) {
        return false;
    }
    *next = n;
    return true;
}

bool gw_fsm_dispatch(gw_fsm_t *fsm, gw_event_t ev, gw_state_t *from)
{
    gw_state_t next;
    if (from != NULL) {
        *from = fsm->state;
    }
    if (!gw_fsm_next(fsm->state, ev, &next)) {
        fsm->rejected++;
        return false;
    }
    fsm->state = next;
    fsm->transitions++;
    return true;
}

bool gw_fsm_accepts_traffic(gw_state_t s)
{
    return s == GW_ST_RUNNING || s == GW_ST_DEGRADED;
}

const char *gw_state_str(gw_state_t s)
{
    static const char *const names[GW_ST_COUNT_] = {
        [GW_ST_BOOT] = "boot",
        [GW_ST_LOAD_REGISTRY] = "load_registry",
        [GW_ST_RADIO_INIT] = "radio_init",
        [GW_ST_RUNNING] = "running",
        [GW_ST_DEGRADED] = "degraded",
        [GW_ST_FAULT] = "fault",
    };
    return ((unsigned)s < GW_ST_COUNT_) ? names[s] : "?";
}

const char *gw_event_str(gw_event_t ev)
{
    static const char *const names[GW_EV_COUNT_] = {
        [GW_EV_BOOT_DONE] = "boot_done",
        [GW_EV_REGISTRY_LOADED] = "registry_loaded",
        [GW_EV_REGISTRY_CORRUPT] = "registry_corrupt",
        [GW_EV_RADIO_READY] = "radio_ready",
        [GW_EV_RADIO_FAILED] = "radio_failed",
        [GW_EV_RETRY_TIMER] = "retry_timer",
        [GW_EV_QUEUE_PRESSURE] = "queue_pressure",
        [GW_EV_QUEUE_RELIEVED] = "queue_relieved",
    };
    return ((unsigned)ev < GW_EV_COUNT_) ? names[ev] : "?";
}
