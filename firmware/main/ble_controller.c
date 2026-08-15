#include "ble_controller.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_ctrl";

/* NimBLE takes 128-bit UUIDs least-significant byte first, i.e. the reverse of
 * the written form. These are 4f1a000{0,1,2}-8b2c-4f5e-9d3a-1c7e6b9f0a01. */
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x01, 0x0a, 0x9f, 0x6b, 0x7e, 0x1c, 0x3a, 0x9d,
    0x5e, 0x4f, 0x2c, 0x8b, 0x00, 0x00, 0x1a, 0x4f);

static const ble_uuid128_t TELEMETRY_UUID = BLE_UUID128_INIT(
    0x01, 0x0a, 0x9f, 0x6b, 0x7e, 0x1c, 0x3a, 0x9d,
    0x5e, 0x4f, 0x2c, 0x8b, 0x01, 0x00, 0x1a, 0x4f);

static const ble_uuid128_t COMMAND_UUID = BLE_UUID128_INIT(
    0x01, 0x0a, 0x9f, 0x6b, 0x7e, 0x1c, 0x3a, 0x9d,
    0x5e, 0x4f, 0x2c, 0x8b, 0x02, 0x00, 0x1a, 0x4f);

typedef struct __attribute__((packed)) {
    int16_t steer;
    int16_t pitch;
    uint8_t buttons;
    uint8_t flags;
    uint16_t seq;
} telemetry_packet_t;

static uint16_t s_telemetry_handle;
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_notify_enabled;
static volatile bool s_advertising;
static bool s_zero_requested;
static uint16_t s_seq;
static uint8_t s_addr_type;
static const char *s_device_name = "IMU Racer";
static telemetry_packet_t s_last;

static void start_advertising(void);

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/**
 * NimBLE reports HCI disconnect reasons as 0x200 | code, so they show up as
 * unhelpful three-digit decimals in the log. Name the ones that actually come
 * up when a link drops.
 */
static const char *disconnect_reason_str(int reason)
{
    switch (reason) {
    case 0x208: return "supervision timeout (out of range, or the central stopped responding)";
    case 0x213: return "remote terminated the connection";
    case 0x216: return "terminated locally";
    case 0x222: return "link layer response timeout";
    case 0x23d: return "connection failed, MIC failure";
    case 0x23e: return "failed to establish connection";
    case 0x205: return "authentication failure (the central expected pairing/bonding)";
    case 0x206: return "PIN or key missing (stale pairing cached on the central)";
    default: return "see BLE_ERR_* in nimble/ble.h";
    }
}

/* -------------------------------------------------------------------------- */
/*                                    GATT                                    */
/* -------------------------------------------------------------------------- */

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* Reading telemetry directly is handy for debugging with nRF Connect. */
        return os_mbuf_append(ctxt->om, &s_last, sizeof(s_last)) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t cmd = 0;
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, &cmd, sizeof(cmd), &len);
        if (rc != 0 || len < 1) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (cmd == BLE_CMD_ZERO) {
            s_zero_requested = true;
            ESP_LOGI(TAG, "zero requested by central");
        }
        return 0;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &TELEMETRY_UUID.u,
                .access_cb = chr_access,
                .val_handle = &s_telemetry_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &COMMAND_UUID.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

/* -------------------------------------------------------------------------- */
/*                                     GAP                                    */
/* -------------------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_advertising = false;   /* the stack stops advertising on connect */
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected, handle %d", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed, status %d", event->connect.status);
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason 0x%03x -- %s",
                 event->disconnect.reason, disconnect_reason_str(event->disconnect.reason));
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        /* May well fail while the link is still being torn down; the
         * maintenance tick retries until it takes. */
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_telemetry_handle) {
            s_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "notifications %s", s_notify_enabled ? "on" : "off");

            /* Only now ask for a faster interval. Requesting it inside the
             * connect event upsets some centrals, and until something has
             * subscribed there is nothing to be responsive about. */
            if (s_notify_enabled) {
                struct ble_gap_upd_params params = {
                    .itvl_min = 12,   /* 15 ms, units of 1.25 ms */
                    .itvl_max = 24,   /* 30 ms */
                    .latency = 0,
                    .supervision_timeout = 400,  /* 4 s, units of 10 ms */
                };
                int rc = ble_gap_update_params(event->subscribe.conn_handle, &params);
                if (rc != 0) {
                    ESP_LOGW(TAG, "conn param update refused, rc=%d (harmless)", rc);
                }
            }
        }
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "conn params updated, status %d", event->conn_update.status);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU now %d", event->mtu.value);
        break;

    default:
        break;
    }

    return 0;
}

/**
 * A 128-bit service UUID eats 18 of the 31 advertising bytes, which leaves no
 * room for the name. Web Bluetooth filters on the service UUID, so that goes
 * in the advertisement and the name goes in the scan response.
 */
static void start_advertising(void)
{
    if (s_advertising || s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    /* Encode the same fields by hand once so the exact bytes going out are
     * visible in the log. Chrome filters on the 128-bit service UUID, and it
     * is 18 of the 31 available bytes, so it is worth being able to prove it
     * is actually in there. */
    static bool dumped;
    if (!dumped) {
        uint8_t buf[BLE_HS_ADV_MAX_SZ];
        uint8_t buf_len = 0;
        int enc = ble_hs_adv_set_fields(&fields, buf, &buf_len, sizeof(buf));
        if (enc == 0) {
            char hex[BLE_HS_ADV_MAX_SZ * 3 + 1];
            for (int i = 0; i < buf_len; i++) {
                sprintf(&hex[i * 3], "%02x ", buf[i]);
            }
            ESP_LOGI(TAG, "adv payload %u/%u bytes: %s", buf_len, BLE_HS_ADV_MAX_SZ, hex);
        } else {
            ESP_LOGE(TAG, "adv payload does not fit, rc=%d", enc);
        }
        dumped = true;
    }

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed, rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (uint8_t *)s_device_name;
    rsp.name_len = strlen(s_device_name);
    rsp.name_is_complete = 1;
    rsp.tx_pwr_lvl_is_present = 1;
    rsp.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed, rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc == BLE_HS_EALREADY) {
        s_advertising = true;          /* already up, nothing to do */
        return;
    }
    if (rc != 0) {
        /* Typically BLE_HS_EBUSY while a link is still tearing down. Leave
         * s_advertising false so ble_controller_maintain() tries again. */
        ESP_LOGW(TAG, "adv_start failed, rc=%d -- will retry", rc);
        return;
    }

    s_advertising = true;
    ESP_LOGI(TAG, "advertising as \"%s\"", s_device_name);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable address, rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto failed, rc=%d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();               /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* -------------------------------------------------------------------------- */
/*                                   Public                                   */
/* -------------------------------------------------------------------------- */

esp_err_t ble_controller_init(const char *device_name)
{
    if (device_name) {
        s_device_name = device_name;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* Web Bluetooth never asks us to pair, but an OS Bluetooth panel will. With
     * no security manager configured that attempt fails outright and the
     * central can cache the failure, so allow a just-works pairing and refuse
     * to store a bond we have no use for. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg failed, rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs failed, rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "device_name_set failed, rc=%d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void ble_controller_maintain(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE && !s_advertising) {
        start_advertising();
    }
}

bool ble_controller_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_controller_is_streaming(void)
{
    return ble_controller_is_connected() && s_notify_enabled;
}

void ble_controller_publish(float steer, float pitch, uint8_t buttons, bool zeroed)
{
    s_last.steer = (int16_t)lroundf(clampf(steer, -1.0f, 1.0f) * 1000.0f);
    s_last.pitch = (int16_t)lroundf(clampf(pitch, -1.0f, 1.0f) * 1000.0f);
    s_last.buttons = buttons;
    s_last.flags = zeroed ? 0x01 : 0x00;
    s_last.seq = s_seq++;

    if (!ble_controller_is_streaming()) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_last, sizeof(s_last));
    if (om == NULL) {
        return;  /* out of mbufs: drop this frame, the next one carries on */
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_telemetry_handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "notify failed, rc=%d", rc);
    }
}

bool ble_controller_take_zero_request(void)
{
    if (!s_zero_requested) {
        return false;
    }
    s_zero_requested = false;
    return true;
}
