/*
 * SPDX-License-Identifier: MIT
 *
 * sensor_mq2.c - MQ-2 combustible gas sensor on ADC1 with curve-fitting
 * calibration (ESP32-S3 eFuse data), oversampling, heater warm-up tracking
 * and one-shot clean-air R0 calibration persisted in NVS.
 */
#include "sdkconfig.h"

#if CONFIG_SENSOR_MQ2_ENABLE

#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "nvs.h"
#include "sensor_backends.h"
#include "sensor_math.h"

static const char *TAG = "mq2";
#define NVS_NS "mq2"
#define NVS_KEY_R0 "r0_ohm"
#define CAL_SAMPLES 50

typedef struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t cali;
    adc_channel_t channel;
    mq2_circuit_t circuit;
    float r0_ohm;
    bool r0_from_nvs;
    uint32_t first_read_ms;
    bool started;
    uint32_t cal_count;
    float cal_rs_sum;
} mq2_ctx_t;

static mq2_ctx_t s_ctx;

static void load_r0(mq2_ctx_t *c)
{
    c->r0_ohm = (float)CONFIG_SENSOR_MQ2_R0_OHM;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t r0 = 0;
        if (nvs_get_u32(h, NVS_KEY_R0, &r0) == ESP_OK && r0 > 0) {
            c->r0_ohm = (float)r0;
            c->r0_from_nvs = true;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "R0 = %.0f ohm (%s)", (double)c->r0_ohm, c->r0_from_nvs ? "NVS" : "Kconfig default");
}

static void store_r0(float r0)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u32(h, NVS_KEY_R0, (uint32_t)(r0 + 0.5f)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static sensor_err_t mq2_init(void *vctx)
{
    mq2_ctx_t *c = (mq2_ctx_t *)vctx;
    memset(c, 0, sizeof(*c));
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(CONFIG_SENSOR_MQ2_GPIO, &unit, &c->channel) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin", CONFIG_SENSOR_MQ2_GPIO);
        return SENSOR_ERR_NOT_FOUND;
    }
    const adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE};
    if (adc_oneshot_new_unit(&ucfg, &c->unit) != ESP_OK) {
        return SENSOR_ERR_IO;
    }
    const adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_config_channel(c->unit, c->channel, &ccfg) != ESP_OK) {
        return SENSOR_ERR_IO;
    }
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1, .chan = c->channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cal, &c->cali) != ESP_OK) {
        ESP_LOGW(TAG, "no eFuse calibration; using linear raw->mV estimate");
        c->cali = NULL;
    }
    const float top = (float)CONFIG_SENSOR_MQ2_DIVIDER_TOP_OHM;
    const float bottom = (float)CONFIG_SENSOR_MQ2_DIVIDER_BOTTOM_OHM;
    c->circuit.vc_mv = (float)CONFIG_SENSOR_MQ2_VC_MV;
    c->circuit.rl_ohm = (float)CONFIG_SENSOR_MQ2_RL_OHM;
    c->circuit.divider_ratio = (top > 0.0f && bottom > 0.0f) ? (top + bottom) / bottom : 1.0f;
    load_r0(c);
    return SENSOR_OK;
}

static sensor_err_t read_mv(mq2_ctx_t *c, float *mv_out)
{
    int64_t acc = 0;
    for (int i = 0; i < CONFIG_SENSOR_MQ2_OVERSAMPLE; i++) {
        int raw = 0;
        if (adc_oneshot_read(c->unit, c->channel, &raw) != ESP_OK) {
            return SENSOR_ERR_IO;
        }
        int mv = 0;
        if (c->cali != NULL && adc_cali_raw_to_voltage(c->cali, raw, &mv) == ESP_OK) {
            acc += mv;
        } else {
            acc += (int64_t)raw * 3100 / 4095; /* approximate full scale at 12 dB */
        }
    }
    *mv_out = (float)acc / (float)CONFIG_SENSOR_MQ2_OVERSAMPLE;
    return SENSOR_OK;
}

static sensor_err_t mq2_read(void *vctx, uint32_t now_ms, sensor_reading_t *out)
{
    mq2_ctx_t *c = (mq2_ctx_t *)vctx;
    if (c->unit == NULL) {
        return SENSOR_ERR_STATE;
    }
    if (!c->started) {
        c->started = true;
        c->first_read_ms = now_ms;
    }
    float adc_mv;
    const sensor_err_t err = read_mv(c, &adc_mv);
    if (err != SENSOR_OK) {
        return err;
    }
    const float v_sensor = mq2_sensor_mv(&c->circuit, adc_mv);
    const float rs = mq2_rs_ohm(&c->circuit, v_sensor);
    const bool warming = (now_ms - c->first_read_ms) < (uint32_t)CONFIG_SENSOR_MQ2_WARMUP_S * 1000u;

#if CONFIG_SENSOR_MQ2_AUTOCAL
    /* Calibrate once, in clean air, right after warm-up: R0 = Rs / 9.83. */
    if (!warming && !c->r0_from_nvs) {
        c->cal_rs_sum += rs;
        if (++c->cal_count >= CAL_SAMPLES) {
            c->r0_ohm = (c->cal_rs_sum / (float)c->cal_count) / MQ2_CLEAN_AIR_RATIO;
            c->r0_from_nvs = true;
            store_r0(c->r0_ohm);
            ESP_LOGI(TAG, "calibrated R0 = %.0f ohm, stored in NVS", (double)c->r0_ohm);
        } else {
            out->status |= MESH_STATUS_CALIBRATING;
        }
    }
#endif
    const float ratio = rs / c->r0_ohm;
    if (warming) {
        out->status |= MESH_STATUS_WARMING_UP;
    }
    sensor_reading_add(out, MESH_Q_GAS_ADC_MV, sensor_to_milli(adc_mv));
    sensor_reading_add(out, MESH_Q_GAS_RS_R0, sensor_to_milli(ratio));
    sensor_reading_add(out, MESH_Q_GAS_PPM, sensor_to_milli(mq2_ppm_from_ratio(ratio)));
    return SENSOR_OK;
}

sensor_driver_t sensor_mq2_driver(void)
{
    const sensor_driver_t d = {
        .name = "mq2", .backend = MESH_BACKEND_MQ2, .init = mq2_init, .read = mq2_read, .ctx = &s_ctx};
    return d;
}

#endif /* CONFIG_SENSOR_MQ2_ENABLE */
