/*
 * SPDX-License-Identifier: MIT
 *
 * node_tasks.c - FreeRTOS tasks of the sensor node.
 *
 *  task        prio core  role
 *  ----------- ---- ----  ---------------------------------------------------
 *  supervisor   10   1    liveness budgets per task, heap watch, TWDT feed
 *  tx            8   0    join, stop-and-wait delivery with retries, CONFIG
 *  sensor        6   1    periodic sampling of every enabled backend
 *  heartbeat     4   1    health telemetry incl. stack high-water marks
 *  (Wi-Fi)      23   0    ESP-IDF; our rx/send callbacks run here
 *
 * Priorities and stack sizes are justified in docs/ARCHITECTURE.md and the
 * stack sizes were validated with uxTaskGetStackHighWaterMark (see the
 * "stack" log lines and the hwm field of every heartbeat).
 */
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "mesh_radio.h"
#include "mesh_rand.h"
#include "mesh_tx.h"
#include "node_app.h"
#include "sdkconfig.h"
#include "sensor_hal.h"

static const char *TAG = "node";

/* Stack sizes in bytes (ESP-IDF FreeRTOS uses bytes, not words). Sized from
 * uxTaskGetStackHighWaterMark measurements (QEMU, loopback radio; see
 * docs/ARCHITECTURE.md): measured peak x ~1.5, rounded up to 512 B, plus extra
 * head-room where a path could not be exercised without hardware (the
 * ESP-NOW driver inside esp_now_send(), the real I2C/ADC drivers). */
#define STACK_SENSOR 4096     /* sim peak ~1.0 KB; real drivers not yet measured */
#define STACK_TX 5120         /* peak ~2.9 KB + esp_now_send() internals */
#define STACK_HEARTBEAT 3584  /* peak ~2.1 KB (ESP_LOG with many varargs) */
#define STACK_SUPERVISOR 3072 /* peak ~0.9 KB; restart path (ESP_LOGE) not exercised */

#define PRIO_SUPERVISOR 10
#define PRIO_TX 8
#define PRIO_SENSOR 6
#define PRIO_HEARTBEAT 4

#define ACK_QUEUE_LEN 4
#define CTRL_QUEUE_LEN 4
#define SEND_CB_TIMEOUT_MS 50
#define TX_IDLE_WAKE_MS 1000
#define RADIO_RETRY_MS 5000

static node_ctx_t *s_n; /* for the Wi-Fi callbacks, which take no user argument */
static uint16_t s_seq;  /* owned by the tx task */
static SemaphoreHandle_t s_drop_mtx; /* makes "drop oldest + enqueue" atomic between producers */

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static TickType_t us_to_ticks(uint64_t us)
{
    if (us == 0) {
        return 0;
    }
    const uint64_t ticks = (us * configTICK_RATE_HZ + 999999u) / 1000000u; /* round up */
    return (TickType_t)(ticks > portMAX_DELAY - 1 ? portMAX_DELAY - 1 : ticks);
}

static void checkin(node_ctx_t *n, node_task_id_t id)
{
    xEventGroupSetBits(n->alive_eg, (EventBits_t)1 << id);
}

node_config_t node_cfg_snapshot(node_ctx_t *n)
{
    xSemaphoreTake(n->state_mtx, portMAX_DELAY);
    const node_config_t c = n->cfg;
    xSemaphoreGive(n->state_mtx);
    return c;
}

static void enqueue_drop_oldest(node_ctx_t *n, const tx_item_t *item, bool urgent)
{
    BaseType_t ok = urgent ? xQueueSendToFront(n->tx_q, item, 0) : xQueueSendToBack(n->tx_q, item, 0);
    if (ok != pdTRUE) {
        static tx_item_t dropped; /* only touched under s_drop_mtx */
        xSemaphoreTake(s_drop_mtx, portMAX_DELAY);
        (void)xQueueReceive(n->tx_q, &dropped, 0); /* discard the oldest reading */
        ok = urgent ? xQueueSendToFront(n->tx_q, item, 0) : xQueueSendToBack(n->tx_q, item, 0);
        xSemaphoreGive(s_drop_mtx);
        xSemaphoreTake(n->state_mtx, portMAX_DELAY);
        n->stats.queue_drops += (ok == pdTRUE) ? 1u : 2u;
        xSemaphoreGive(n->state_mtx);
    }
    xTaskNotifyGive(n->task[NODE_TASK_TX]);
}

/* ------------------------------------------------------------------------- */
/* Radio callbacks - run in the Wi-Fi task (priority 23). No blocking, no     */
/* logging, no allocation: decode, route to a queue, wake the tx task.        */
/* ------------------------------------------------------------------------- */

static void on_recv(const uint8_t src_mac[MESH_MAC_LEN], const uint8_t *data, int len, int8_t rssi,
                    bool broadcast)
{
    node_ctx_t *n = s_n;
    if (n == NULL) {
        return;
    }
    ctrl_evt_t evt; /* ~140 B on the Wi-Fi task stack */
    if (mesh_decode(data, (size_t)len, &evt.frame) != MESH_OK) {
        n->rx_invalid++;
        return;
    }
    if (evt.frame.hdr.node_id != MESH_GATEWAY_NODE_ID) {
        return; /* other nodes' JOIN broadcasts */
    }
    switch (evt.frame.hdr.type) {
    case MESH_MSG_ACK: {
        if (broadcast) {
            return;
        }
        const ack_evt_t ack = {.seq = evt.frame.u.ack.acked_seq,
                               .attempt = evt.frame.u.ack.acked_attempt,
                               .status = evt.frame.u.ack.status,
                               .rssi = rssi};
        if (xQueueSendToBack(n->ack_q, &ack, 0) != pdTRUE) {
            n->rx_dropped++;
        }
        break;
    }
    case MESH_MSG_JOIN_ACK:
    case MESH_MSG_CONFIG:
        memcpy(evt.mac, src_mac, MESH_MAC_LEN);
        evt.rssi = rssi;
        if (xQueueSendToBack(n->ctrl_q, &evt, 0) != pdTRUE) {
            n->rx_dropped++;
        } else if (n->task[NODE_TASK_TX] != NULL) {
            xTaskNotifyGive(n->task[NODE_TASK_TX]);
        }
        break;
    default:
        break;
    }
}

static void on_send(const uint8_t dst_mac[MESH_MAC_LEN], bool delivered)
{
    (void)dst_mac;
    if (s_n != NULL) {
        xEventGroupSetBits(s_n->link_eg, delivered ? LINK_SEND_OK : LINK_SEND_FAIL);
    }
}

/* ------------------------------------------------------------------------- */
/* TX task                                                                   */
/* ------------------------------------------------------------------------- */

/** Hand a frame to ESP-NOW and wait for the MAC-layer completion callback. */
static bool radio_send_wait(node_ctx_t *n, const uint8_t mac[MESH_MAC_LEN], const uint8_t *buf, size_t len)
{
    xEventGroupClearBits(n->link_eg, LINK_SEND_OK | LINK_SEND_FAIL);
    if (mesh_radio_send(mac, buf, len) != ESP_OK) {
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(n->link_eg, LINK_SEND_OK | LINK_SEND_FAIL, pdTRUE, pdFALSE,
                                                 pdMS_TO_TICKS(SEND_CB_TIMEOUT_MS));
    return (bits & LINK_SEND_OK) != 0;
}

typedef enum { DELIVER_OK, DELIVER_FAILED, DELIVER_REJECTED } deliver_result_t;

/** Stop-and-wait delivery of an encoded frame, driven by the pure mesh_tx FSM. */
static deliver_result_t deliver(node_ctx_t *n, mesh_tx_t *tx, uint8_t *buf, size_t len, uint16_t seq)
{
    xQueueReset(n->ack_q); /* discard late ACKs of previous frames */
    mesh_tx_action_t act = mesh_tx_start(tx, seq);
    for (;;) {
        if (act == MESH_TX_ACTION_SEND) {
            mesh_frame_set_attempt(buf, len, tx->attempt);
            if (tx->attempt > 1) {
                xSemaphoreTake(n->state_mtx, portMAX_DELAY);
                n->stats.tx_retries++;
                xSemaphoreGive(n->state_mtx);
            }
            /* Timestamp before handing the frame to the radio: the RTT then
             * covers both airtimes, and the ACK timeout starts at the send. */
            const uint64_t t_send = (uint64_t)esp_timer_get_time();
            const bool mac_ok = radio_send_wait(n, n->gw_mac, buf, len);
            mesh_tx_sent(tx, t_send);
            act = mac_ok ? MESH_TX_ACTION_WAIT : mesh_tx_on_radio_fail(tx, (uint64_t)esp_timer_get_time());
            continue;
        }
        if (act == MESH_TX_ACTION_DONE) {
            return tx->acked_status == MESH_ACK_REJECTED ? DELIVER_REJECTED : DELIVER_OK;
        }
        if (act == MESH_TX_ACTION_FAILED) {
            return DELIVER_FAILED;
        }
        /* WAIT: block on the ACK queue until the FSM's next deadline. */
        const uint64_t now = (uint64_t)esp_timer_get_time();
        const uint64_t deadline = mesh_tx_deadline(tx);
        ack_evt_t ack;
        if (xQueueReceive(n->ack_q, &ack, us_to_ticks(deadline > now ? deadline - now : 0)) == pdTRUE) {
            act = mesh_tx_on_ack(tx, ack.seq, ack.attempt, ack.status, (uint64_t)esp_timer_get_time());
            if (act == MESH_TX_ACTION_DONE) {
                xSemaphoreTake(n->state_mtx, portMAX_DELAY);
                n->stats.last_ack_rssi = ack.rssi;
                xSemaphoreGive(n->state_mtx);
            }
            if (act != MESH_TX_ACTION_WAIT) {
                continue;
            }
        }
        act = mesh_tx_poll(tx, (uint64_t)esp_timer_get_time());
    }
}

static uint8_t frame_flags(void)
{
    return MESH_FLAG_ACK_REQ | (mesh_radio_encryption_enabled() ? MESH_FLAG_ENCRYPTED : 0);
}

static bool do_join(node_ctx_t *n)
{
    const node_config_t cfg = node_cfg_snapshot(n);
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_JOIN, cfg.node_id, s_seq++, node_uptime_ms(), 0);
    f.u.join.fw_version = mesh_fw_version(NODE_FW_MAJOR, NODE_FW_MINOR, NODE_FW_PATCH);
    f.u.join.boot_count = n->boot_count;
    f.u.join.report_interval_ms = cfg.report_interval_ms;
    f.u.join.heartbeat_interval_ms = cfg.heartbeat_interval_ms;
    f.u.join.backend = sensor_hal_backend_mask();
    f.u.join.reset_reason = n->reset_reason;
    uint8_t buf[MESH_FRAME_MAX];
    size_t len;
    if (mesh_encode(&f, buf, sizeof(buf), &len) != MESH_OK) {
        return false;
    }
    (void)radio_send_wait(n, MESH_BROADCAST_MAC, buf, len);

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(CONFIG_NODE_JOIN_TIMEOUT_MS);
    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) {
            return false;
        }
        ctrl_evt_t evt;
        if (xQueueReceive(n->ctrl_q, &evt, timeout - elapsed) != pdTRUE) {
            return false;
        }
        if (evt.frame.hdr.type != MESH_MSG_JOIN_ACK) {
            continue; /* CONFIG before JOIN_ACK: ignore, the gateway will retry */
        }
        if (n->gateway_pinned && memcmp(evt.mac, n->gw_mac, MESH_MAC_LEN) != 0) {
            ESP_LOGW(TAG, "JOIN_ACK from unexpected gateway ignored");
            continue;
        }
        char macs[MESH_MAC_STR_LEN];
        mesh_format_mac(evt.mac, macs);
        if (evt.frame.u.join_ack.status != MESH_JOIN_ACCEPTED) {
            ESP_LOGW(TAG, "join rejected by %s (status %u)", macs, evt.frame.u.join_ack.status);
            return false;
        }
        memcpy(n->gw_mac, evt.mac, MESH_MAC_LEN);
        if (mesh_radio_add_peer(n->gw_mac, true) != ESP_OK) {
            return false;
        }
        xEventGroupSetBits(n->link_eg, LINK_JOINED);
        ESP_LOGI(TAG, "joined gateway %s (rssi %d dBm, offline timeout %" PRIu32 " ms)", macs, evt.rssi,
                 evt.frame.u.join_ack.offline_timeout_ms);
        return true;
    }
}

static void send_ack(node_ctx_t *n, const ctrl_evt_t *evt, uint8_t status)
{
    const node_config_t cfg = node_cfg_snapshot(n);
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_ACK, cfg.node_id, s_seq++, node_uptime_ms(), 0);
    f.u.ack.acked_seq = evt->frame.hdr.seq;
    f.u.ack.acked_type = evt->frame.hdr.type;
    f.u.ack.acked_attempt = evt->frame.hdr.attempt;
    f.u.ack.status = status;
    f.u.ack.rssi = evt->rssi;
    uint8_t buf[MESH_FRAME_MAX];
    size_t len;
    if (mesh_encode(&f, buf, sizeof(buf), &len) == MESH_OK) {
        (void)radio_send_wait(n, n->gw_mac, buf, len); /* ACKs are not acknowledged */
    }
}

static void handle_ctrl(node_ctx_t *n, const ctrl_evt_t *evt)
{
    if (memcmp(evt->mac, n->gw_mac, MESH_MAC_LEN) != 0 || evt->frame.hdr.type != MESH_MSG_CONFIG) {
        return; /* duplicate JOIN_ACK or foreign gateway */
    }
    const uint8_t key = evt->frame.u.config.key;
    const uint32_t value = evt->frame.u.config.value;
    xSemaphoreTake(n->state_mtx, portMAX_DELAY);
    const node_cfg_result_t res = node_config_apply(&n->cfg, key, value);
    const node_config_t snapshot = n->cfg;
    xSemaphoreGive(n->state_mtx);

    send_ack(n, evt, res == NODE_CFG_INVALID ? MESH_ACK_INVALID : MESH_ACK_OK);
    switch (res) {
    case NODE_CFG_APPLIED:
        ESP_LOGI(TAG, "config key %u = %" PRIu32 " applied", key, value);
        if (node_storage_save(&snapshot) != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist config");
        }
        /* Wake the periodic tasks so a new interval takes effect immediately. */
        xTaskNotifyGive(n->task[NODE_TASK_SENSOR]);
        xTaskNotifyGive(n->task[NODE_TASK_HEARTBEAT]);
        break;
    case NODE_CFG_REBOOT:
        ESP_LOGW(TAG, "reboot requested by gateway");
        vTaskDelay(pdMS_TO_TICKS(200)); /* let the ACK leave the radio */
        esp_restart();
        break;
    case NODE_CFG_INVALID:
        ESP_LOGW(TAG, "config key %u = %" PRIu32 " rejected", key, value);
        break;
    default:
        break;
    }
}

static size_t build_frame(node_ctx_t *n, const tx_item_t *item, uint16_t seq, uint8_t *buf)
{
    const node_config_t cfg = node_cfg_snapshot(n);
    mesh_frame_t f;
    if (item->kind == TX_ITEM_DATA) {
        mesh_frame_init(&f, MESH_MSG_DATA, cfg.node_id, seq, node_uptime_ms(), frame_flags());
        f.u.data.backend = item->u.reading.backend_mask;
        f.u.data.status = item->u.reading.status;
        xSemaphoreTake(n->state_mtx, portMAX_DELAY);
        f.u.data.last_rtt_us = n->stats.last_rtt_us;
        xSemaphoreGive(n->state_mtx);
        f.u.data.channel_count = item->u.reading.count;
        memcpy(f.u.data.channels, item->u.reading.ch, sizeof(mesh_channel_t) * item->u.reading.count);
    } else {
        mesh_frame_init(&f, MESH_MSG_HEARTBEAT, cfg.node_id, seq, node_uptime_ms(), frame_flags());
        f.u.hb = item->u.hb;
    }
    size_t len = 0;
    return mesh_encode(&f, buf, MESH_FRAME_MAX, &len) == MESH_OK ? len : 0;
}

static void record_result(node_ctx_t *n, deliver_result_t res, const mesh_tx_t *tx)
{
    xSemaphoreTake(n->state_mtx, portMAX_DELAY);
    if (res == DELIVER_FAILED) {
        n->stats.tx_failed++;
    } else {
        n->stats.tx_ok++;
        n->stats.last_rtt_us = tx->rtt_us;
        n->stats.rtt_sum_us += tx->rtt_us;
        n->stats.rtt_count++;
        if (tx->rtt_us > n->stats.rtt_max_us) {
            n->stats.rtt_max_us = tx->rtt_us;
        }
    }
    xSemaphoreGive(n->state_mtx);
}

static void tx_task(void *arg)
{
    node_ctx_t *n = (node_ctx_t *)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    const mesh_retry_policy_t pol = {
        .max_attempts = CONFIG_NODE_TX_MAX_ATTEMPTS,
        .ack_timeout_us = CONFIG_NODE_ACK_TIMEOUT_MS * 1000u,
        .backoff_base_us = CONFIG_NODE_BACKOFF_BASE_MS * 1000u,
        .backoff_max_us = CONFIG_NODE_BACKOFF_MAX_MS * 1000u,
        .jitter_us = CONFIG_NODE_BACKOFF_JITTER_MS * 1000u,
    };
    const mesh_retry_policy_t join_pol = {
        .max_attempts = 8,
        .ack_timeout_us = 1,
        .backoff_base_us = 500000u,
        .backoff_max_us = CONFIG_NODE_JOIN_BACKOFF_MAX_MS * 1000u,
        .jitter_us = 250000u,
    };
    mesh_tx_t tx;
    mesh_tx_init(&tx, &pol, esp_random());
    uint32_t join_rng = esp_random();
    uint8_t join_attempt = 1;
    uint32_t consecutive_failures = 0;
    TickType_t next_join = 0;
    TickType_t next_radio_retry = 0;
    static uint8_t buf[MESH_FRAME_MAX]; /* static: keeps 250 B off the task stack */

    for (;;) {
        checkin(n, NODE_TASK_TX);
        esp_task_wdt_reset();
        const TickType_t now = xTaskGetTickCount();

        if (!n->radio_ok) {
            if ((int32_t)(now - next_radio_retry) >= 0) {
                n->radio_ok = (mesh_radio_init(on_recv, on_send) == ESP_OK);
                next_radio_retry = now + pdMS_TO_TICKS(RADIO_RETRY_MS);
                ESP_LOGW(TAG, "radio init retry: %s", n->radio_ok ? "ok" : "failed");
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TX_IDLE_WAKE_MS));
            continue;
        }

        if (!(xEventGroupGetBits(n->link_eg) & LINK_JOINED)) {
            if ((int32_t)(now - next_join) < 0) {
                ulTaskNotifyTake(pdTRUE, (next_join - now) < pdMS_TO_TICKS(TX_IDLE_WAKE_MS)
                                             ? (next_join - now)
                                             : pdMS_TO_TICKS(TX_IDLE_WAKE_MS));
                continue;
            }
            if (do_join(n)) {
                join_attempt = 1;
                consecutive_failures = 0;
            } else {
                if (join_attempt < 16) {
                    join_attempt++;
                }
                next_join = xTaskGetTickCount() + us_to_ticks(mesh_retry_backoff_us(&join_pol, join_attempt, &join_rng));
            }
            continue;
        }

        ctrl_evt_t evt;
        while (xQueueReceive(n->ctrl_q, &evt, 0) == pdTRUE) {
            handle_ctrl(n, &evt);
        }

        tx_item_t item;
        if (xQueueReceive(n->tx_q, &item, 0) != pdTRUE) {
            /* Nothing queued: sleep until a producer or the rx callback notifies us. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TX_IDLE_WAKE_MS));
            continue;
        }
        const uint16_t seq = s_seq++;
        const size_t len = build_frame(n, &item, seq, buf);
        if (len == 0) {
            continue;
        }
        const deliver_result_t res = deliver(n, &tx, buf, len, seq);
        record_result(n, res, &tx);
        if (res == DELIVER_OK) {
            consecutive_failures = 0;
        } else if (res == DELIVER_REJECTED) {
            ESP_LOGW(TAG, "gateway does not know us any more; re-joining");
            xEventGroupClearBits(n->link_eg, LINK_JOINED);
        } else if (++consecutive_failures >= CONFIG_NODE_GATEWAY_LOST_FAILURES) {
            ESP_LOGW(TAG, "%" PRIu32 " consecutive undelivered frames: gateway lost, re-joining",
                     consecutive_failures);
            xEventGroupClearBits(n->link_eg, LINK_JOINED);
            next_join = xTaskGetTickCount();
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Sensor task                                                               */
/* ------------------------------------------------------------------------- */

/**
 * Sleep until @p *next, but wake early (and restart the schedule) when a
 * CONFIG change notifies the task. Returns true if the period elapsed.
 */
static bool wait_period(TickType_t *next, uint32_t period_ms)
{
    *next += pdMS_TO_TICKS(period_ms);
    const TickType_t now = xTaskGetTickCount();
    const int32_t remaining = (int32_t)(*next - now);
    if (remaining <= 0) {
        *next = now; /* overrun: skip missed slots rather than bursting */
        return true;
    }
    if (ulTaskNotifyTake(pdTRUE, (TickType_t)remaining) > 0) {
        *next = xTaskGetTickCount();
        return false;
    }
    return true;
}

static void sensor_task(void *arg)
{
    node_ctx_t *n = (node_ctx_t *)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        checkin(n, NODE_TASK_SENSOR);
        const node_config_t cfg = node_cfg_snapshot(n);
        if (!wait_period(&next, cfg.report_interval_ms)) {
            continue;
        }
        static tx_item_t item; /* static: sensor_reading_t is ~110 B */
        item.kind = TX_ITEM_DATA;
        (void)sensor_hal_read(node_uptime_ms(), &item.u.reading);
        enqueue_drop_oldest(n, &item, false);
    }
}

/* ------------------------------------------------------------------------- */
/* Heartbeat task                                                            */
/* ------------------------------------------------------------------------- */

static void heartbeat_task(void *arg)
{
    node_ctx_t *n = (node_ctx_t *)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        checkin(n, NODE_TASK_HEARTBEAT);
        const node_config_t cfg = node_cfg_snapshot(n);
        if (!wait_period(&next, cfg.heartbeat_interval_ms)) {
            continue;
        }
        static tx_item_t item;
        item.kind = TX_ITEM_HEARTBEAT;
        mesh_heartbeat_payload_t *hb = &item.u.hb;
        memset(hb, 0, sizeof(*hb));
        hb->uptime_s = node_uptime_ms() / 1000u;
        hb->free_heap = esp_get_free_heap_size();
        hb->min_free_heap = esp_get_minimum_free_heap_size();

        xSemaphoreTake(n->state_mtx, portMAX_DELAY);
        hb->tx_ok = n->stats.tx_ok;
        hb->tx_retries = n->stats.tx_retries;
        hb->tx_failed = n->stats.tx_failed;
        hb->queue_drops = n->stats.queue_drops;
        hb->rtt_avg_us = n->stats.rtt_count ? (uint32_t)(n->stats.rtt_sum_us / n->stats.rtt_count) : 0;
        hb->rtt_max_us = n->stats.rtt_max_us;
        hb->last_ack_rssi = n->stats.last_ack_rssi;
        n->stats.rtt_sum_us = 0;
        n->stats.rtt_count = 0;
        n->stats.rtt_max_us = 0;
        xSemaphoreGive(n->state_mtx);

        hb->task_count = NODE_TASK_COUNT;
        for (int i = 0; i < NODE_TASK_COUNT; i++) {
            const UBaseType_t hwm = n->task[i] ? uxTaskGetStackHighWaterMark(n->task[i]) : 0;
            hb->stack_hwm[i] = (uint16_t)(hwm > UINT16_MAX ? UINT16_MAX : hwm);
            /* Machine-readable line consumed by tools/ (and the QEMU stack report). */
            ESP_LOGI(TAG, "stack task=%s size=%" PRIu32 " min_free=%u", n->task_name[i], n->task_stack[i],
                     (unsigned)hwm);
        }
        ESP_LOGI(TAG, "hb: heap=%" PRIu32 " min=%" PRIu32 " ok=%" PRIu32 " retries=%" PRIu32 " failed=%" PRIu32
                      " drops=%" PRIu32 " rx_invalid=%" PRIu32,
                 hb->free_heap, hb->min_free_heap, hb->tx_ok, hb->tx_retries, hb->tx_failed, hb->queue_drops,
                 n->rx_invalid);
        enqueue_drop_oldest(n, &item, true);
    }
}

/* ------------------------------------------------------------------------- */
/* Supervisor (software watchdog)                                            */
/* ------------------------------------------------------------------------- */

static void supervisor_task(void *arg)
{
    node_ctx_t *n = (node_ctx_t *)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    TickType_t last_seen[NODE_TASK_COUNT];
    for (int i = 0; i < NODE_TASK_COUNT; i++) {
        last_seen[i] = xTaskGetTickCount();
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_NODE_SUPERVISOR_PERIOD_MS));
        const EventBits_t bits = xEventGroupClearBits(n->alive_eg, (1u << NODE_TASK_COUNT) - 1u);
        const TickType_t now = xTaskGetTickCount();
        const node_config_t cfg = node_cfg_snapshot(n);
        const uint32_t budget_ms[NODE_TASK_COUNT] = {
            [NODE_TASK_SENSOR] = 2u * cfg.report_interval_ms + 5000u,
            [NODE_TASK_TX] = CONFIG_NODE_TASK_STALL_MS,
            [NODE_TASK_HEARTBEAT] = 2u * cfg.heartbeat_interval_ms + 5000u,
            [NODE_TASK_SUPERVISOR] = UINT32_MAX,
        };
        for (int i = 0; i < NODE_TASK_COUNT; i++) {
            if (bits & (1u << i)) {
                last_seen[i] = now;
            } else if (i != NODE_TASK_SUPERVISOR &&
                       (uint32_t)((now - last_seen[i]) * portTICK_PERIOD_MS) > budget_ms[i]) {
                ESP_LOGE(TAG, "task '%s' silent for %" PRIu32 " ms (budget %" PRIu32 "): restarting",
                         n->task_name[i], (uint32_t)((now - last_seen[i]) * portTICK_PERIOD_MS), budget_ms[i]);
                vTaskDelay(pdMS_TO_TICKS(100)); /* flush the log */
                esp_restart();
            }
        }
        if (esp_get_free_heap_size() < CONFIG_NODE_LOW_HEAP_BYTES) {
            ESP_LOGW(TAG, "low heap: %" PRIu32 " bytes", esp_get_free_heap_size());
        }
        esp_task_wdt_reset();
    }
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

static void spawn(node_ctx_t *n, node_task_id_t id, TaskFunction_t fn, const char *name, uint32_t stack,
                  UBaseType_t prio, BaseType_t core)
{
    n->task_name[id] = name;
    n->task_stack[id] = stack;
    const BaseType_t ok = xTaskCreatePinnedToCore(fn, name, stack, n, prio, &n->task[id], core);
    configASSERT(ok == pdPASS);
}

esp_err_t node_tasks_start(node_ctx_t *n)
{
    n->tx_q = xQueueCreate(CONFIG_NODE_TX_QUEUE_LEN, sizeof(tx_item_t));
    n->ack_q = xQueueCreate(ACK_QUEUE_LEN, sizeof(ack_evt_t));
    n->ctrl_q = xQueueCreate(CTRL_QUEUE_LEN, sizeof(ctrl_evt_t));
    n->link_eg = xEventGroupCreate();
    n->alive_eg = xEventGroupCreate();
    n->state_mtx = xSemaphoreCreateMutex();
    s_drop_mtx = xSemaphoreCreateMutex();
    if (!n->tx_q || !n->ack_q || !n->ctrl_q || !n->link_eg || !n->alive_eg || !n->state_mtx || !s_drop_mtx) {
        return ESP_ERR_NO_MEM;
    }
    s_n = n;

    if (n->gateway_pinned) {
        /* Needed up-front with encryption: the JOIN_ACK arrives encrypted. */
        n->radio_ok = (mesh_radio_init(on_recv, on_send) == ESP_OK) && (mesh_radio_add_peer(n->gw_mac, true) == ESP_OK);
    } else {
        n->radio_ok = (mesh_radio_init(on_recv, on_send) == ESP_OK);
    }
    if (!n->radio_ok) {
        ESP_LOGE(TAG, "radio init failed; the tx task will keep retrying every %d ms", RADIO_RETRY_MS);
    }

    /* The tx task is created first: the others notify it. */
    spawn(n, NODE_TASK_TX, tx_task, "tx", STACK_TX, PRIO_TX, 0);
    spawn(n, NODE_TASK_SENSOR, sensor_task, "sensor", STACK_SENSOR, PRIO_SENSOR, 1);
    spawn(n, NODE_TASK_HEARTBEAT, heartbeat_task, "heartbeat", STACK_HEARTBEAT, PRIO_HEARTBEAT, 1);
    spawn(n, NODE_TASK_SUPERVISOR, supervisor_task, "supervisor", STACK_SUPERVISOR, PRIO_SUPERVISOR, 1);
    return ESP_OK;
}
