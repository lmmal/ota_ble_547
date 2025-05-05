#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_ota_ops.h"
#include "inttypes.h"

#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define TAG "BLE_INIT"

// BLE UUID Definitions
#define OTA_SERVICE_UUID  0xFFF0
#define OTA_CHAR_UUID     0xFFF1
#define BLE_UUID16_GAP    0x1800
#define BLE_UUID16_GATT   0x1801

// OTA message types
#define OTA_MSG_INIT   0x01
#define OTA_MSG_CHUNK  0x02
#define OTA_MSG_END    0x03

// OTA state
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *ota_partition = NULL;
static int ota_total_size = 0;
static int ota_bytes_written = 0;
static int receiving_state = 0;  // 0: idle, 1: receiving, 2: error

void ble_host_task(void *param);  // forward declaration

// === OTA BLE Handlers ===
static void handle_ota_init(uint8_t *data, int len) {
    if (len < 4) {
        ESP_LOGW(TAG, "INIT too short (%d bytes)", len);
        return;
    }

    ota_total_size = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    ota_bytes_written = 0;
    receiving_state = 1;

    ESP_LOGI(TAG, "OTA INIT: firmware=%d bytes", ota_total_size);

    ota_partition = esp_ota_get_next_update_partition(NULL);
    esp_err_t err = esp_ota_begin(ota_partition, ota_total_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        receiving_state = 2;  // error state
    }
}

static void handle_ota_chunk(uint8_t *data, int len) {
    if (receiving_state != 1) {
        ESP_LOGW(TAG, "CHUNK received in invalid state (%d)", receiving_state);
        return;
    }

    if (!ota_handle) {
        ESP_LOGW(TAG, "CHUNK received before ota_begin");
        return;
    }

    esp_err_t err = esp_ota_write(ota_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
        receiving_state = 2;  // error state
    } else {
        ota_bytes_written += len;
        ESP_LOGI(TAG, "Firmware chunk: %d bytes (%d / %d)", len, ota_bytes_written, ota_total_size);
    }
}

static void handle_ota_end() {
    if (!ota_handle) {
        ESP_LOGW(TAG, "END received before ota_begin");
        return;
    }

    esp_err_t err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "OTA complete. Rebooting...");
    esp_restart();
}

static int ota_char_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            os_mbuf_append(ctxt->om, "Hello", strlen("Hello"));
            return 0;
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            uint8_t buf[512];
            int len = OS_MBUF_PKTLEN(ctxt->om);
            ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), NULL);

            ESP_LOGI(TAG, "Write %d bytes", len);
            if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

            switch (buf[0]) {
                case OTA_MSG_INIT:
                    handle_ota_init(&buf[1], len - 1);
                    break;
                case OTA_MSG_CHUNK:
                    handle_ota_chunk(&buf[1], len - 1);
                    break;
                case OTA_MSG_END:
                    handle_ota_end();
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown command 0x%02X", buf[0]);
                    break;
            }
            return 0;
        }
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

// === GATT + BLE ===

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_UUID16_GAP),
        .characteristics = (struct ble_gatt_chr_def[]){{0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_UUID16_GATT),
        .characteristics = (struct ble_gatt_chr_def[]){{0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(OTA_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(OTA_CHAR_UUID),
                .access_cb = ota_char_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {0}
        }
    },
    {0}
};

static void ble_app_on_sync(void) {
    ESP_LOGI(TAG, "BLE synced. Advertising...");
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields = {0};

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = "nimble";
    ble_svc_gap_device_name_set(name);
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    ble_gap_adv_start(0, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
}

static void ble_app_on_reset(int reason) {
    ESP_LOGE(TAG, "BLE reset; reason=%d", reason);
}

void ble_ota_init(void) {
    ESP_LOGI(TAG, "Starting BLE OTA");

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: 0x%x", ret);
        return;
    }

    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_hs_cfg.reset_cb = ble_app_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0 || ble_gatts_add_svcs(gatt_svcs) != 0) {
        ESP_LOGE(TAG, "GATT init failed");
        return;
    }

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE OTA service initialized");
}

void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}