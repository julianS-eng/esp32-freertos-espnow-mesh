/*
 * SPDX-License-Identifier: MIT
 *
 * gw_tasks.c - FreeRTOS tasks of the gateway.
 *
 *  task        prio core  role
 *  ----------- ---- ----  ---------------------------------------------------
 *  supervisor   10   1    liveness budgets per task, heap watch, TWDT feed
 *  rx            9   0    validate frames, registry, send ACK/JOIN_ACK
 *  liveness      7   1    node timeouts, queue-pressure FSM, stats, NVS saves
 *  out           5   1    JSON Lines formatting + serial output
 *  cmd           3   1    serial command line (set/reboot/forget/stats)
 *  (Wi-Fi)      23   0    ESP-IDF; our rx/send callbacks run here
 *
 * Only the rx task sits on the ACK path, so the ACK latency does not depend
 * on how fast the serial port drains: formatting and printing are decoupled
 * through out_q (PSRAM-backed). When out_q is nearly full the gateway enters
 * DEGRADED and answers ACK(BUSY) instead of dropping readings silently.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_app.h"
#include "gw_json.h"
#include "mesh_radio.h"
#include "mesh_tx.h"
#include "sdkconfig.h"

static const char *TAG = "gw";

typedef enum { T_RX = 0, T_LIVENESS, T_OUT, T_CMD, T_SUPERVISOR, T_COUNT } gw_task_id_t;
_Static_assert(T_COUNT <= GW_MAX_TASKS, "gw_stats carries GW_MAX_TASKS HWMs");

/* Stack sizes in bytes, sized from uxTaskGetStackHighWaterMark measurements
 * (QEMU, loopback radio, 4 emulated nodes; see docs/ARCHITECTURE.md). */
static const struct {
    const char *name;
    uint32_t stack;
    UBaseType_t prio;
    BaseType_t core;
    uint32_t budget_ms; /* supervisor: max silence before restart */
} s_task_def[T_COUNT] = {
    /* peak ~1.2 KB; keeps room for esp_now_send()/esp_now_add_peer() internals */
    [T_RX] = {"rx", 4096, 9, 0, 5000},
    /* peak ~1.8 KB incl. NVS registry write */
    [T_LIVENESS] = {"liveness", 3072, 7, 1, 5000},
    /* peak ~1.9 KB (vsnprintf of a JSON line) */
    [T_OUT] = {"out", 3072, 5, 1, 5000},
    /* CONFIG send path measured by feeding commands to the QEMU console */
    [T_CMD] = {"cmd", 4096, 3, 1, 5000},
    /* peak ~0.9 KB; restart path (ESP_LOGE) not exercised */
    [T_SUPERVISOR] = {"supervisor", 3072, 10, 1, UINT32_MAX},
};

typedef struct {
    uint8_t mac[MESH_MAC_LEN];
    int8_t rssi;
    uint8_t len;
    uint8_t data[MESH_FRAME_MAX];
} rx_item_t;

typedef struct {
    uint16_t node_id;
    mesh_ack_payload_t ack;
} cfg_ack_t;

#define SEND_DONE BIT0
#define SEND_OK BIT1

static gw_core_t s_core;
static gw_fsm_t s_fsm;
static SemaphoreHandle_t s_core_mtx;  /* guards s_core and s_fsm */
static QueueHandle_t s_rx_q;          /* Wi-Fi task -> rx */
static QueueHandle_t s_out_q;         /* everyone -> out (PSRAM) */
static QueueHandle_t s_cfg_ack_q;     /* rx -> cmd */
static EventGroupHandle_t s_alive_eg; /* check-ins */
static EventGroupHandle_t s_send_eg;  /* send callback -> sender */
static SemaphoreHandle_t s_send_mtx;  /* one ESP-NOW transmission in flight at a time */
static TaskHandle_t s_task[T_COUNT];
static volatile bool s_persist_pending;

/* Counters written by a single context each. */
static volatile uint32_t s_rx_q_drops;  /* Wi-Fi task */
static volatile uint32_t s_out_q_drops; /* producers (approximate across tasks, informative) */
static uint32_t s_out_q_peak;           /* out task */

uint64_t gw_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

gw_core_t *gw_core(void)
{
    return &s_core;
}

bool gw_core_lock(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_core_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void gw_core_unlock(void)
{
    xSemaphoreGive(s_core_mtx);
}

bool gw_emit(const gw_out_t *rec)
{
    if (s_out_q == NULL || xQueueSendToBack(s_out_q, rec, 0) != pdTRUE) {
        s_out_q_drops++;
        return false;
    }
    return true;
}

static void emit_info(const char *msg)
{
    static gw_out_t rec; /* callers: cmd task and app_main only */
    memset(&rec, 0, sizeof(rec));
    rec.kind = GW_OUT_INFO;
    rec.ts_ms = gw_now_ms();
    strlcpy(rec.u.info, msg, sizeof(rec.u.info));
    gw_emit(&rec);
}

void gw_dispatch(gw_event_t ev)
{
    gw_state_t from;
    bool changed;
    if (s_core_mtx != NULL) {
        xSemaphoreTake(s_core_mtx, portMAX_DELAY);
    }
    changed = gw_fsm_dispatch(&s_fsm, ev, &from);
    const gw_state_t to = s_fsm.state;
    if (s_core_mtx != NULL) {
        xSemaphoreGive(s_core_mtx);
    }
    if (changed) {
        gw_out_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.kind = GW_OUT_GW_STATE;
        rec.ts_ms = gw_now_ms();
        rec.u.gw_state.from = (uint8_t)from;
        rec.u.gw_state.to = (uint8_t)to;
        rec.u.gw_state.event = (uint8_t)ev;
        gw_emit(&rec);
        ESP_LOGI(TAG, "state %s -> %s (%s)", gw_state_str(from), gw_state_str(to), gw_event_str(ev));
    }
}

static void checkin(gw_task_id_t id)
{
    xEventGroupSetBits(s_alive_eg, (EventBits_t)1 << id);
}

/* ------------------------------------------------------------------------- */
/* Radio callbacks (Wi-Fi task)                                              */
/* ------------------------------------------------------------------------- */

static void on_recv(const uint8_t src_mac[MESH_MAC_LEN], const uint8_t *data, int len, int8_t rssi,
                    bool broadcast)
{
    (void)broadcast;
    if (s_rx_q == NULL || len <= 0 || len > (int)MESH_FRAME_MAX) {
        return;
    }
    rx_item_t item;
    memcpy(item.mac, src_mac, MESH_MAC_LEN);
    item.rssi = rssi;
    item.len = (uint8_t)len;
    memcpy(item.data, data, (size_t)len);
    if (xQueueSendToBack(s_rx_q, &item, 0) != pdTRUE) {
        s_rx_q_drops++;
    }
}

static void on_send(const uint8_t dst_mac[MESH_MAC_LEN], bool delivered)
{
    (void)dst_mac;
    xEventGroupSetBits(s_send_eg, SEND_DONE | (delivered ? SEND_OK : 0));
}

/** Serialised transmit: send and wait for the MAC-layer completion (<= 50 ms). */
static bool radio_send_wait(const uint8_t mac[MESH_MAC_LEN], const uint8_t *buf, size_t len)
{
    xSemaphoreTake(s_send_mtx, portMAX_DELAY);
    xEventGroupClearBits(s_send_eg, SEND_DONE | SEND_OK);
    bool ok = false;
    if (mesh_radio_send(mac, buf, len) == ESP_OK) {
        const EventBits_t b = xEventGroupWaitBits(s_send_eg, SEND_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(50));
        ok = (b & SEND_OK) != 0;
    }
    xSemaphoreGive(s_send_mtx);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* RX task                                                                   */
/* ------------------------------------------------------------------------- */

static void rx_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    static rx_item_t item;       /* static: 256 B */
    static gw_rx_result_t res;   /* static: ~600 B */
    for (;;) {
        checkin(T_RX);
        esp_task_wdt_reset();
        if (xQueueReceive(s_rx_q, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        const bool can_forward = uxQueueSpacesAvailable(s_out_q) >= GW_RX_MAX_OUT;
        xSemaphoreTake(s_core_mtx, portMAX_DELAY);
        const bool accepting = gw_fsm_accepts_traffic(s_fsm.state);
        if (accepting) {
            gw_core_handle_frame(&s_core, item.mac, item.data, item.len, item.rssi, gw_now_ms(), can_forward, &res);
        }
        xSemaphoreGive(s_core_mtx);
        if (!accepting) {
            continue;
        }

        if (res.add_peer) {
            (void)mesh_radio_add_peer(res.reply_mac, true);
        }
        if (res.reply) {
            bool temp = false;
            if (res.reply_needs_temp_peer && !mesh_radio_has_peer(res.reply_mac)) {
                temp = (mesh_radio_add_peer(res.reply_mac, false) == ESP_OK);
            }
            (void)radio_send_wait(res.reply_mac, res.reply_buf, res.reply_len);
            if (temp) {
                (void)mesh_radio_del_peer(res.reply_mac);
            }
        }
        if (res.persist) {
            s_persist_pending = true; /* the flash write is done by the liveness task */
        }
        for (size_t i = 0; i < res.n_out; i++) {
            gw_emit(&res.out[i]);
        }
        if (res.ack_for_gateway) {
            const cfg_ack_t a = {.node_id = res.ack_from_node, .ack = res.ack};
            (void)xQueueSendToBack(s_cfg_ack_q, &a, 0);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Output task                                                               */
/* ------------------------------------------------------------------------- */

static void out_task(void *arg)
{
    (void)arg;
    static gw_out_t rec;
    static char line[GW_JSON_LINE_MAX];
    for (;;) {
        checkin(T_OUT);
        const UBaseType_t waiting = uxQueueMessagesWaiting(s_out_q);
        if (waiting > s_out_q_peak) {
            s_out_q_peak = waiting;
        }
        if (xQueueReceive(s_out_q, &rec, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        const int n = gw_json_format(&rec, line, sizeof(line));
        if (n > 0) {
            fwrite(line, 1, (size_t)n, stdout);
            fflush(stdout);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Liveness task                                                             */
/* ------------------------------------------------------------------------- */

static void fill_platform_stats(gw_stats_t *s)
{
    s->uptime_s = (uint32_t)(gw_now_ms() / 1000u);
    s->rx_queue_drops = s_rx_q_drops;
    s->out_queue_drops = s_out_q_drops;
    s->out_queue_peak = s_out_q_peak;
    s->free_heap = esp_get_free_heap_size();
    s->min_free_heap = esp_get_minimum_free_heap_size();
    s->free_psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s->task_count = T_COUNT;
    for (int i = 0; i < T_COUNT; i++) {
        s->task_names[i] = s_task_def[i].name;
        s->stack_hwm[i] = s_task[i] ? (uint16_t)uxTaskGetStackHighWaterMark(s_task[i]) : 0;
    }
}

static void liveness_task(void *arg)
{
    (void)arg;
    static gw_out_t recs[GW_REGISTRY_CAPACITY];
    static uint8_t blob[8 + 26 * GW_REGISTRY_CAPACITY];
    TickType_t last_stats = xTaskGetTickCount();
    const uint32_t out_len = CONFIG_GW_OUT_QUEUE_LEN;
    for (;;) {
        checkin(T_LIVENESS);
        vTaskDelay(pdMS_TO_TICKS(500));
        const uint64_t now = gw_now_ms();

        /* 1. Node liveness */
        xSemaphoreTake(s_core_mtx, portMAX_DELAY);
        size_t n = gw_core_tick(&s_core, now, recs, GW_REGISTRY_CAPACITY);
        xSemaphoreGive(s_core_mtx);
        for (size_t i = 0; i < n; i++) {
            gw_emit(&recs[i]);
        }

        /* 2. Queue pressure -> RUNNING/DEGRADED (hysteresis) */
        const uint32_t fill_pct = 100u * (uint32_t)uxQueueMessagesWaiting(s_out_q) / out_len;
        const uint32_t rx_fill_pct = 100u * (uint32_t)uxQueueMessagesWaiting(s_rx_q) / CONFIG_GW_RX_QUEUE_LEN;
        const uint32_t worst = fill_pct > rx_fill_pct ? fill_pct : rx_fill_pct;
        if (worst >= CONFIG_GW_QUEUE_PRESSURE_PCT && s_fsm.state == GW_ST_RUNNING) {
            gw_dispatch(GW_EV_QUEUE_PRESSURE);
        } else if (worst <= CONFIG_GW_QUEUE_RELIEF_PCT && s_fsm.state == GW_ST_DEGRADED) {
            gw_dispatch(GW_EV_QUEUE_RELIEVED);
        }

        /* 3. Deferred NVS write of the registry */
        if (s_persist_pending) {
            size_t len = 0;
            xSemaphoreTake(s_core_mtx, portMAX_DELAY);
            s_persist_pending = false;
            const bool ok = gw_registry_serialize(&s_core.reg, blob, sizeof(blob), &len);
            xSemaphoreGive(s_core_mtx);
            if (ok && gw_storage_save(blob, len) != ESP_OK) {
                ESP_LOGE(TAG, "registry save failed");
            }
        }

        /* 4. Periodic link summaries and gateway statistics */
        if ((xTaskGetTickCount() - last_stats) >= pdMS_TO_TICKS(CONFIG_GW_STATS_PERIOD_S * 1000u)) {
            last_stats = xTaskGetTickCount();
            xSemaphoreTake(s_core_mtx, portMAX_DELAY);
            n = gw_core_link_records(&s_core, now, recs, GW_REGISTRY_CAPACITY - 1);
            gw_out_t *st = &recs[n];
            memset(st, 0, sizeof(*st));
            st->kind = GW_OUT_GW_STATS;
            st->ts_ms = now;
            gw_core_fill_stats(&s_core, &st->u.stats);
            xSemaphoreGive(s_core_mtx);
            fill_platform_stats(&st->u.stats);
            for (size_t i = 0; i <= n; i++) {
                gw_emit(&recs[i]);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Command task                                                              */
/* ------------------------------------------------------------------------- */

static void send_config(uint16_t node_id, uint8_t key, uint32_t value)
{
    static uint8_t buf[MESH_FRAME_MAX];
    uint8_t mac[MESH_MAC_LEN];
    size_t len = 0;
    uint16_t seq = 0;
    xSemaphoreTake(s_core_mtx, portMAX_DELAY);
    const bool ok = gw_core_build_config(&s_core, node_id, key, value, gw_now_ms(), buf, &len, &seq, mac);
    xSemaphoreGive(s_core_mtx);
    if (!ok) {
        emit_info("unknown node");
        return;
    }
    const mesh_retry_policy_t pol = {.max_attempts = CONFIG_GW_CONFIG_ATTEMPTS,
                                     .ack_timeout_us = CONFIG_GW_CONFIG_ACK_TIMEOUT_MS * 1000u,
                                     .backoff_base_us = 50000,
                                     .backoff_max_us = 400000,
                                     .jitter_us = 20000};
    mesh_tx_t tx;
    mesh_tx_init(&tx, &pol, (uint32_t)esp_timer_get_time());
    xQueueReset(s_cfg_ack_q);
    mesh_tx_action_t act = mesh_tx_start(&tx, seq);
    while (act != MESH_TX_ACTION_DONE && act != MESH_TX_ACTION_FAILED) {
        if (act == MESH_TX_ACTION_SEND) {
            mesh_frame_set_attempt(buf, len, tx.attempt);
            const bool mac_ok = radio_send_wait(mac, buf, len);
            const uint64_t now = (uint64_t)esp_timer_get_time();
            mesh_tx_sent(&tx, now);
            act = mac_ok ? MESH_TX_ACTION_WAIT : mesh_tx_on_radio_fail(&tx, now);
            continue;
        }
        const uint64_t now = (uint64_t)esp_timer_get_time();
        const uint64_t dl = mesh_tx_deadline(&tx);
        cfg_ack_t a;
        const TickType_t wait = pdMS_TO_TICKS(dl > now ? (dl - now + 999) / 1000 : 0);
        if (xQueueReceive(s_cfg_ack_q, &a, wait) == pdTRUE && a.node_id == node_id) {
            act = mesh_tx_on_ack(&tx, a.ack.acked_seq, a.ack.acked_attempt, a.ack.status,
                                 (uint64_t)esp_timer_get_time());
            if (act != MESH_TX_ACTION_WAIT) {
                continue;
            }
        }
        act = mesh_tx_poll(&tx, (uint64_t)esp_timer_get_time());
        checkin(T_CMD);
    }
    gw_out_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.kind = GW_OUT_CONFIG;
    rec.ts_ms = gw_now_ms();
    rec.node_id = node_id;
    memcpy(rec.mac, mac, MESH_MAC_LEN);
    rec.u.config.key = key;
    rec.u.config.value = value;
    rec.u.config.status = (act == MESH_TX_ACTION_DONE) ? tx.acked_status : 0xFF;
    rec.u.config.attempts = tx.attempt;
    gw_emit(&rec);
}

static void emit_stats_now(void)
{
    static gw_out_t recs[GW_REGISTRY_CAPACITY];
    const uint64_t now = gw_now_ms();
    xSemaphoreTake(s_core_mtx, portMAX_DELAY);
    size_t n = gw_core_link_records(&s_core, now, recs, GW_REGISTRY_CAPACITY - 1);
    gw_out_t *st = &recs[n];
    memset(st, 0, sizeof(*st));
    st->kind = GW_OUT_GW_STATS;
    st->ts_ms = now;
    gw_core_fill_stats(&s_core, &st->u.stats);
    xSemaphoreGive(s_core_mtx);
    fill_platform_stats(&st->u.stats);
    for (size_t i = 0; i <= n; i++) {
        gw_emit(&recs[i]);
    }
}

static void handle_command(char *line)
{
    unsigned node = 0;
    unsigned long value = 0;
    char key[16] = {0};
    if (strcmp(line, "help") == 0) {
        emit_info("commands: stats | set <node> report_ms|hb_ms <value> | reboot <node> | forget <node>");
    } else if (strcmp(line, "stats") == 0 || strcmp(line, "nodes") == 0) {
        emit_stats_now();
    } else if (sscanf(line, "set %u %15s %lu", &node, key, &value) == 3) {
        if (strcmp(key, "report_ms") == 0) {
            send_config((uint16_t)node, MESH_CFG_REPORT_INTERVAL_MS, (uint32_t)value);
        } else if (strcmp(key, "hb_ms") == 0) {
            send_config((uint16_t)node, MESH_CFG_HEARTBEAT_INTERVAL_MS, (uint32_t)value);
        } else {
            emit_info("unknown key (report_ms | hb_ms)");
        }
    } else if (sscanf(line, "reboot %u", &node) == 1) {
        send_config((uint16_t)node, MESH_CFG_REBOOT, 0);
    } else if (sscanf(line, "forget %u", &node) == 1) {
        uint8_t mac[MESH_MAC_LEN];
        xSemaphoreTake(s_core_mtx, portMAX_DELAY);
        const bool ok = gw_registry_forget(&s_core.reg, (uint16_t)node, mac);
        xSemaphoreGive(s_core_mtx);
        if (ok) {
            (void)mesh_radio_del_peer(mac);
            s_persist_pending = true;
        }
        emit_info(ok ? "node forgotten" : "unknown node");
    } else if (line[0] != '\0') {
        emit_info("unknown command, try 'help'");
    }
}

/** Read one byte from the console with a timeout; returns 1 on success, 0 on timeout. */
static int console_getc(char *c, TickType_t timeout)
{
#if CONFIG_ESP_CONSOLE_UART
    return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, c, 1, timeout) == 1 ? 1 : 0;
#else
    /* USB-Serial-JTAG / USB-CDC consoles: command input not implemented (docs: Future work). */
    (void)c;
    vTaskDelay(timeout);
    return 0;
#endif
}

static void cmd_task(void *arg)
{
    (void)arg;
#if CONFIG_ESP_CONSOLE_UART
    /* Interrupt-driven RX only; output keeps using the VFS console path. The
     * driver read has a timeout, so this task can check in with the supervisor
     * while nobody is typing (a blocking read() would starve its budget). */
    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM)) {
        ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 512, 0, 0, NULL, 0));
    }
#endif
    static char line[96];
    size_t pos = 0;
    for (;;) {
        checkin(T_CMD);
        char c;
        if (console_getc(&c, pdMS_TO_TICKS(1000)) == 0) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            line[pos] = '\0';
            handle_command(line);
            pos = 0;
        } else if (pos < sizeof(line) - 1) {
            line[pos++] = c;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Supervisor                                                                */
/* ------------------------------------------------------------------------- */

static void supervisor_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    TickType_t last_seen[T_COUNT];
    for (int i = 0; i < T_COUNT; i++) {
        last_seen[i] = xTaskGetTickCount();
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const EventBits_t bits = xEventGroupClearBits(s_alive_eg, (1u << T_COUNT) - 1u);
        const TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < T_COUNT; i++) {
            if (s_task[i] == NULL || (bits & (1u << i))) {
                last_seen[i] = now;
                continue;
            }
            const uint32_t silent = (uint32_t)((now - last_seen[i]) * portTICK_PERIOD_MS);
            if (silent > s_task_def[i].budget_ms) {
                ESP_LOGE(TAG, "task '%s' silent for %" PRIu32 " ms: restarting", s_task_def[i].name, silent);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        }
        esp_task_wdt_reset();
    }
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

static void spawn(gw_task_id_t id, TaskFunction_t fn)
{
    const BaseType_t ok = xTaskCreatePinnedToCore(fn, s_task_def[id].name, s_task_def[id].stack, NULL,
                                                  s_task_def[id].prio, &s_task[id], s_task_def[id].core);
    configASSERT(ok == pdPASS);
}

esp_err_t gw_tasks_init(void)
{
    gw_fsm_init(&s_fsm);
    const gw_liveness_policy_t pol = {.suspect_misses = CONFIG_GW_SUSPECT_MISSES,
                                      .offline_misses = CONFIG_GW_OFFLINE_MISSES,
                                      .grace_ms = CONFIG_GW_LIVENESS_GRACE_MS};
    gw_core_init(&s_core, CONFIG_GW_MAX_NODES, &pol, mesh_radio_channel());

    s_core_mtx = xSemaphoreCreateMutex();
    s_send_mtx = xSemaphoreCreateMutex();
    s_alive_eg = xEventGroupCreate();
    s_send_eg = xEventGroupCreate();
    s_rx_q = xQueueCreate(CONFIG_GW_RX_QUEUE_LEN, sizeof(rx_item_t));
    s_cfg_ack_q = xQueueCreate(4, sizeof(cfg_ack_t));
    /* The output queue is the largest buffer (~CONFIG_GW_OUT_QUEUE_LEN x 140 B):
     * put it in PSRAM when present. It is only touched from task context, so
     * cache-disabled periods (flash writes) are not an issue. */
    s_out_q = xQueueCreateWithCaps(CONFIG_GW_OUT_QUEUE_LEN, sizeof(gw_out_t), MALLOC_CAP_SPIRAM);
    if (s_out_q == NULL) {
        ESP_LOGW(TAG, "no PSRAM: output queue in internal RAM");
        s_out_q = xQueueCreate(CONFIG_GW_OUT_QUEUE_LEN, sizeof(gw_out_t));
    }
    if (!s_core_mtx || !s_send_mtx || !s_alive_eg || !s_send_eg || !s_rx_q || !s_cfg_ack_q || !s_out_q) {
        return ESP_ERR_NO_MEM;
    }
    spawn(T_OUT, out_task);
    return ESP_OK;
}

esp_err_t gw_tasks_start(void)
{
    spawn(T_RX, rx_task);
    spawn(T_LIVENESS, liveness_task);
    spawn(T_CMD, cmd_task);
    spawn(T_SUPERVISOR, supervisor_task);
    return ESP_OK;
}

mesh_radio_recv_cb_t gw_radio_recv_cb(void)
{
    return on_recv;
}

mesh_radio_send_cb_t gw_radio_send_cb(void)
{
    return on_send;
}
