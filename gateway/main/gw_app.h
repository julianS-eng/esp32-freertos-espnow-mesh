/*
 * SPDX-License-Identifier: MIT
 *
 * gw_app.h - Gateway application glue (ESP-IDF side).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_core.h"
#include "gw_fsm.h"
#include "mesh_radio.h"

#define GW_FW_MAJOR 1
#define GW_FW_MINOR 0
#define GW_FW_PATCH 0

/* gw_storage.c */
esp_err_t gw_storage_init(void);
/** Load the registry blob. Returns nodes restored, 0 if none stored, -1 if corrupt. */
int gw_storage_load(gw_registry_t *reg);
esp_err_t gw_storage_save(const uint8_t *blob, size_t len);

/* gw_tasks.c */
/** Create queues and start the output task (so boot records can be emitted). */
esp_err_t gw_tasks_init(void);
/** Start the RX, liveness, command and supervisor tasks (after the radio is up). */
esp_err_t gw_tasks_start(void);
/** Enqueue an output record (thread-safe, non-blocking). */
bool gw_emit(const gw_out_t *rec);
/** Emit a gw_state record and apply the transition. */
void gw_dispatch(gw_event_t ev);
gw_core_t *gw_core(void);
bool gw_core_lock(uint32_t timeout_ms);
void gw_core_unlock(void);
uint64_t gw_now_ms(void);
mesh_radio_recv_cb_t gw_radio_recv_cb(void);
mesh_radio_send_cb_t gw_radio_send_cb(void);
