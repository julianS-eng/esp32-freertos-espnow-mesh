/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_radio_loopback.c - Test double for mesh_radio.h (QEMU / bench runs).
 *
 * Never touches the RF hardware. A dedicated high-priority task stands in for
 * the Wi-Fi task: it invokes the application's send/receive callbacks
 * asynchronously, with ~1-2 ms latency and a configurable loss probability,
 * and plays the other side of the protocol:
 *
 *  - MESH_RADIO_LOOPBACK_GATEWAY: answers JOIN with JOIN_ACK and every
 *    ACK-requesting frame with ACK; every 30 s it sends a CONFIG frame that
 *    toggles the report interval, exercising the node's control path.
 *  - MESH_RADIO_LOOPBACK_NODES: N emulated nodes JOIN, send DATA every second
 *    and HEARTBEAT every 5 s, answer CONFIG with ACK; the last node goes
 *    silent between t = 40 s and t = 80 s to exercise offline detection.
 *
 * Everything the firmware does above the radio is real, which is what makes
 * the QEMU stack high-water marks meaningful.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mesh_protocol.h"
#include "mesh_radio.h"
#include "mesh_rand.h"
#include "sdkconfig.h"

static const char *TAG = "radio_loop";

const uint8_t MESH_BROADCAST_MAC[MESH_MAC_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define LOOP_PRIO 22 /* just below the real Wi-Fi task (23) */
#define LOOP_STACK 4096
#define MAX_PENDING 32
#define MAX_PEERS 20
#define LATENCY_US 1500u

typedef struct {
    bool used;
    int64_t due_us;
    bool is_send_done; /* true: send callback; false: deliver a frame to on_recv */
    bool delivered;
    uint8_t mac[MESH_MAC_LEN];
    uint8_t len;
    uint8_t data[MESH_FRAME_MAX];
} pending_t;

typedef struct {
    uint8_t mac[MESH_MAC_LEN];
    uint8_t len;
    uint8_t data[MESH_FRAME_MAX];
} tx_req_t;

static mesh_radio_recv_cb_t s_on_recv;
static mesh_radio_send_cb_t s_on_send;
static QueueHandle_t s_tx_q;
static SemaphoreHandle_t s_send_mtx; /* guards s_send_buf (keeps 257 B off the callers' stacks) */
static tx_req_t s_send_buf;
static TaskHandle_t s_task;
static pending_t s_pending[MAX_PENDING];
static uint8_t s_peers[MAX_PEERS][MESH_MAC_LEN];
static bool s_peer_used[MAX_PEERS];
static uint32_t s_rng = 0x5EED1234u;
static uint16_t s_remote_seq;

static bool lost(void)
{
    return mesh_rand_below(&s_rng, 100) < (uint32_t)CONFIG_MESH_LOOPBACK_LOSS_PCT;
}

static void schedule(bool is_send_done, bool delivered, const uint8_t mac[MESH_MAC_LEN], const uint8_t *data,
                     size_t len, int64_t delay_us)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!s_pending[i].used) {
            pending_t *p = &s_pending[i];
            p->used = true;
            p->due_us = esp_timer_get_time() + delay_us;
            p->is_send_done = is_send_done;
            p->delivered = delivered;
            memcpy(p->mac, mac, MESH_MAC_LEN);
            p->len = (uint8_t)len;
            if (data != NULL && len > 0) {
                memcpy(p->data, data, len);
            }
            return;
        }
    }
    /* Table full: behaves like a dropped frame. */
}

static void make_ack(mesh_frame_t *out, uint16_t src_id, const mesh_frame_t *in)
{
    mesh_frame_init(out, MESH_MSG_ACK, src_id, s_remote_seq++, (uint32_t)(esp_timer_get_time() / 1000), 0);
    out->u.ack.acked_seq = in->hdr.seq;
    out->u.ack.acked_type = in->hdr.type;
    out->u.ack.acked_attempt = in->hdr.attempt;
    out->u.ack.status = MESH_ACK_OK;
    out->u.ack.rssi = -40;
}

/* ------------------------------------------------------------------------- */
#if CONFIG_MESH_RADIO_LOOPBACK_GATEWAY

static const uint8_t GW_MAC[MESH_MAC_LEN] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static uint8_t s_node_mac[MESH_MAC_LEN];
static bool s_node_joined;
static uint16_t s_node_id;
static int64_t s_next_config_us;
static bool s_config_toggle;

static void reply_frame(const uint8_t from_mac[MESH_MAC_LEN], mesh_frame_t *f)
{
    uint8_t buf[MESH_FRAME_MAX];
    size_t len;
    if (mesh_encode(f, buf, sizeof(buf), &len) == MESH_OK && !lost()) {
        schedule(false, true, from_mac, buf, len, LATENCY_US + mesh_rand_below(&s_rng, 1000));
    }
}

static void peer_react(const tx_req_t *req)
{
    mesh_frame_t in, out;
    if (mesh_decode(req->data, req->len, &in) != MESH_OK) {
        return;
    }
    if (in.hdr.type == MESH_MSG_JOIN) {
        mesh_frame_init(&out, MESH_MSG_JOIN_ACK, MESH_GATEWAY_NODE_ID, s_remote_seq++,
                        (uint32_t)(esp_timer_get_time() / 1000), 0);
        out.u.join_ack.status = MESH_JOIN_ACCEPTED;
        out.u.join_ack.wifi_channel = mesh_radio_channel();
        out.u.join_ack.offline_timeout_ms = 4 * in.u.join.heartbeat_interval_ms;
        s_node_joined = true;
        s_node_id = in.hdr.node_id;
        reply_frame(GW_MAC, &out);
    } else if (in.hdr.flags & MESH_FLAG_ACK_REQ) {
        make_ack(&out, MESH_GATEWAY_NODE_ID, &in);
        reply_frame(GW_MAC, &out);
    }
}

static void periodic(int64_t now_us)
{
    if (!s_node_joined) {
        return;
    }
    if (s_next_config_us == 0) {
        s_next_config_us = now_us + 30000000;
    }
    if (now_us >= s_next_config_us) {
        s_next_config_us = now_us + 30000000;
        mesh_frame_t f;
        mesh_frame_init(&f, MESH_MSG_CONFIG, MESH_GATEWAY_NODE_ID, s_remote_seq++, (uint32_t)(now_us / 1000),
                        MESH_FLAG_ACK_REQ);
        f.u.config.key = MESH_CFG_REPORT_INTERVAL_MS;
        f.u.config.value = s_config_toggle ? 1000u : 2000u;
        s_config_toggle = !s_config_toggle;
        ESP_LOGI(TAG, "emulated gateway -> node 0x%04x: CONFIG report_ms=%u", s_node_id, (unsigned)f.u.config.value);
        reply_frame(GW_MAC, &f);
    }
}

static void loopback_get_mac(uint8_t mac[MESH_MAC_LEN])
{
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    (void)s_node_mac;
}

/* ------------------------------------------------------------------------- */
#elif CONFIG_MESH_RADIO_LOOPBACK_NODES

#define N_NODES CONFIG_MESH_LOOPBACK_NODE_COUNT

typedef struct {
    uint8_t mac[MESH_MAC_LEN];
    uint16_t id;
    uint16_t seq;
    bool joined;
    int64_t next_data_us;
    int64_t next_hb_us;
    int64_t next_join_us;
    uint32_t tx_ok;
} fake_node_t;

static fake_node_t s_nodes[N_NODES];
static int64_t s_start_us;

static fake_node_t *node_by_mac(const uint8_t mac[MESH_MAC_LEN])
{
    for (int i = 0; i < N_NODES; i++) {
        if (memcmp(s_nodes[i].mac, mac, MESH_MAC_LEN) == 0) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

static void node_send(fake_node_t *n, mesh_frame_t *f)
{
    uint8_t buf[MESH_FRAME_MAX];
    size_t len;
    if (mesh_encode(f, buf, sizeof(buf), &len) == MESH_OK && !lost()) {
        schedule(false, true, n->mac, buf, len, LATENCY_US + mesh_rand_below(&s_rng, 1000));
    }
}

static void peer_react(const tx_req_t *req)
{
    fake_node_t *n = node_by_mac(req->mac);
    mesh_frame_t in, out;
    if (n == NULL || mesh_decode(req->data, req->len, &in) != MESH_OK) {
        return;
    }
    if (in.hdr.type == MESH_MSG_JOIN_ACK) {
        n->joined = (in.u.join_ack.status == MESH_JOIN_ACCEPTED);
    } else if (in.hdr.type == MESH_MSG_ACK) {
        if (in.u.ack.status == MESH_ACK_REJECTED) {
            n->joined = false;
        } else {
            n->tx_ok++;
        }
    } else if (in.hdr.type == MESH_MSG_CONFIG) {
        make_ack(&out, n->id, &in);
        out.hdr.seq = n->seq++;
        node_send(n, &out);
    }
}

static bool silent(const fake_node_t *n, int64_t now_us)
{
    const int64_t t = now_us - s_start_us;
    return n == &s_nodes[N_NODES - 1] && N_NODES > 1 && t >= 40000000 && t < 80000000;
}

static void periodic(int64_t now_us)
{
    if (s_start_us == 0) {
        s_start_us = now_us;
    }
    for (int i = 0; i < N_NODES; i++) {
        fake_node_t *n = &s_nodes[i];
        if (silent(n, now_us)) {
            continue;
        }
        mesh_frame_t f;
        const uint32_t up = (uint32_t)((now_us - s_start_us) / 1000);
        if (!n->joined) {
            if (now_us >= n->next_join_us) {
                n->next_join_us = now_us + 1000000;
                mesh_frame_init(&f, MESH_MSG_JOIN, n->id, n->seq++, up, 0);
                f.u.join.fw_version = mesh_fw_version(1, 0, 0);
                f.u.join.boot_count = 1;
                f.u.join.report_interval_ms = 1000;
                f.u.join.heartbeat_interval_ms = 5000;
                f.u.join.backend = MESH_BACKEND_SIM;
                node_send(n, &f);
            }
            continue;
        }
        if (now_us >= n->next_data_us) {
            n->next_data_us = now_us + 1000000;
            mesh_frame_init(&f, MESH_MSG_DATA, n->id, n->seq++, up, MESH_FLAG_ACK_REQ);
            f.u.data.backend = MESH_BACKEND_SIM;
            (void)mesh_data_add(&f, MESH_Q_GAS_PPM, 180000 + (int32_t)mesh_rand_below(&s_rng, 20000));
            (void)mesh_data_add(&f, MESH_Q_MOTOR_RPM, 3000000 + (int32_t)mesh_rand_below(&s_rng, 50000));
            (void)mesh_data_add(&f, MESH_Q_ACCEL_Z, 1000 + (int32_t)mesh_rand_below(&s_rng, 30));
            node_send(n, &f);
        }
        if (now_us >= n->next_hb_us) {
            n->next_hb_us = now_us + 5000000;
            mesh_frame_init(&f, MESH_MSG_HEARTBEAT, n->id, n->seq++, up, MESH_FLAG_ACK_REQ);
            f.u.hb.uptime_s = up / 1000;
            f.u.hb.tx_ok = n->tx_ok;
            f.u.hb.task_count = 0;
            node_send(n, &f);
        }
    }
}

static void loopback_get_mac(uint8_t mac[MESH_MAC_LEN])
{
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

#endif
/* ------------------------------------------------------------------------- */

static void loop_task(void *arg)
{
    (void)arg;
    static tx_req_t req;
    for (;;) {
        int64_t now = esp_timer_get_time();
        int64_t next_due = now + 10000; /* periodic tick every 10 ms at most */
        for (int i = 0; i < MAX_PENDING; i++) {
            if (s_pending[i].used && s_pending[i].due_us < next_due) {
                next_due = s_pending[i].due_us;
            }
        }
        const TickType_t wait = (next_due > now) ? pdMS_TO_TICKS((next_due - now + 999) / 1000) : 0;
        if (xQueueReceive(s_tx_q, &req, wait) == pdTRUE) {
            /* The emulated MAC layer "acks" unicast frames unless lost; broadcast always succeeds. */
            const bool bcast = mesh_mac_is_broadcast(req.mac);
            const bool mac_ok = bcast || !lost();
            schedule(true, mac_ok, req.mac, NULL, 0, 300);
            if (mac_ok) {
                peer_react(&req);
            }
        }
        now = esp_timer_get_time();
        periodic(now);
        for (int i = 0; i < MAX_PENDING; i++) {
            pending_t *p = &s_pending[i];
            if (!p->used || p->due_us > now) {
                continue;
            }
            if (p->is_send_done) {
                if (s_on_send) {
                    s_on_send(p->mac, p->delivered);
                }
            } else if (s_on_recv) {
                s_on_recv(p->mac, p->data, p->len, (int8_t)(-45 - (int)mesh_rand_below(&s_rng, 20)), false);
            }
            p->used = false;
        }
    }
}

esp_err_t mesh_radio_init(mesh_radio_recv_cb_t on_recv, mesh_radio_send_cb_t on_send)
{
    s_on_recv = on_recv;
    s_on_send = on_send;
    if (s_task != NULL) {
        return ESP_OK;
    }
#if CONFIG_MESH_RADIO_LOOPBACK_NODES
    for (int i = 0; i < N_NODES; i++) {
        const uint8_t mac[MESH_MAC_LEN] = {0x02, 0x00, 0x00, 0x00, 0x10, (uint8_t)(i + 1)};
        memcpy(s_nodes[i].mac, mac, MESH_MAC_LEN);
        s_nodes[i].id = (uint16_t)(0x0100 + i);
    }
#endif
    s_tx_q = xQueueCreate(8, sizeof(tx_req_t));
    s_send_mtx = xSemaphoreCreateMutex();
    if (s_tx_q == NULL || s_send_mtx == NULL ||
        xTaskCreatePinnedToCore(loop_task, "radio_loop", LOOP_STACK, NULL, LOOP_PRIO, &s_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG, "LOOPBACK radio backend active (loss %d%%) - no RF traffic", CONFIG_MESH_LOOPBACK_LOSS_PCT);
    return ESP_OK;
}

esp_err_t mesh_radio_add_peer(const uint8_t mac[MESH_MAC_LEN], bool encrypt)
{
    (void)encrypt;
    int free_slot = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peer_used[i] && memcmp(s_peers[i], mac, MESH_MAC_LEN) == 0) {
            return ESP_OK;
        }
        if (!s_peer_used[i] && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return ESP_ERR_NO_MEM;
    }
    s_peer_used[free_slot] = true;
    memcpy(s_peers[free_slot], mac, MESH_MAC_LEN);
    return ESP_OK;
}

esp_err_t mesh_radio_del_peer(const uint8_t mac[MESH_MAC_LEN])
{
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peer_used[i] && memcmp(s_peers[i], mac, MESH_MAC_LEN) == 0) {
            s_peer_used[i] = false;
        }
    }
    return ESP_OK;
}

bool mesh_radio_has_peer(const uint8_t mac[MESH_MAC_LEN])
{
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peer_used[i] && memcmp(s_peers[i], mac, MESH_MAC_LEN) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t mesh_radio_send(const uint8_t mac[MESH_MAC_LEN], const uint8_t *data, size_t len)
{
    if (s_tx_q == NULL || len > MESH_FRAME_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!mesh_mac_is_broadcast(mac) && !mesh_radio_has_peer(mac)) {
        return ESP_ERR_ESPNOW_NOT_FOUND;
    }
    xSemaphoreTake(s_send_mtx, portMAX_DELAY);
    memcpy(s_send_buf.mac, mac, MESH_MAC_LEN);
    s_send_buf.len = (uint8_t)len;
    memcpy(s_send_buf.data, data, len);
    const BaseType_t ok = xQueueSendToBack(s_tx_q, &s_send_buf, 0);
    xSemaphoreGive(s_send_mtx);
    return ok == pdTRUE ? ESP_OK : ESP_ERR_ESPNOW_NO_MEM;
}

bool mesh_radio_encryption_enabled(void)
{
#if CONFIG_MESH_ENCRYPTION
    return true;
#else
    return false;
#endif
}

uint8_t mesh_radio_channel(void)
{
    return (uint8_t)CONFIG_MESH_WIFI_CHANNEL;
}

void mesh_radio_get_mac(uint8_t mac[MESH_MAC_LEN])
{
    loopback_get_mac(mac);
}
