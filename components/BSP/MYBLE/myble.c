#include "myble.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define TAG "NimBLE-Demo"
#define DEVICE_NAME "ESP32S3-NimBLE"
static bool ble_adv_active = false;
static uint16_t rx_value_handler;
static uint16_t tx_value_handler;

static int gatt_event_handler(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        if (attr_handle == rx_value_handler)
        {
            if (ctxt->om->om_data[0] == 0x00)
            {
                ESP_LOGE(TAG, "第 0 字节收到数据 0x00");
            }
            else if (ctxt->om->om_data[0] == 0x01)
            {
                ESP_LOGE(TAG, "第 0 字节收到数据 0x01");
            }
            if (ctxt->om->om_data[1] == 0x00)
            {
                ESP_LOGE(TAG, "第 1 字节收到数据 0x00");
            }
            else if (ctxt->om->om_data[1] == 0x01)
            {
                ESP_LOGE(TAG, "第 1 字节收到数据 0x01");
            }
        }
    }
    else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        if (attr_handle == tx_value_handler)
        {
            const char *value = "Hello";
            os_mbuf_append(ctxt->om,value,strlen(value));
        }
    }

    return 0;
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_CONNECT)
    {
        if (event->connect.status == 0)
        {
            ESP_LOGE(TAG, "设备已连接");
            ble_adv_active = false;
        }
        else
        {
            ESP_LOGE(TAG, "连接失败，重新广播");
            if (!ble_adv_active)
            {
                start_advertising();
            }
        }
    }
    else if (event->type == BLE_GAP_EVENT_DISCONNECT)
    {
        ESP_LOGE(TAG, "设备断开连接");
        if (!ble_adv_active)
        {
            start_advertising();
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] =
    {
        {.type = BLE_GATT_SVC_TYPE_PRIMARY,
         .uuid = BLE_UUID16_DECLARE(0x00FF),
         .characteristics = (struct ble_gatt_chr_def[]){
             // ESP32S3接收手机数据
             {
                 .uuid = BLE_UUID16_DECLARE(0xFF01),
                 .access_cb = gatt_event_handler,
                 .flags = BLE_GATT_CHR_F_WRITE,
                 .val_handle = &rx_value_handler,
                 .arg = NULL,
             },
             // ESP32S3向手机发送数据
             {
                 .uuid = BLE_UUID16_DECLARE(0xFF02),
                 .access_cb = gatt_event_handler,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &tx_value_handler,
                 .arg = NULL,
             },
             {0}}},
        {0}};

void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "广播内容设置失败");
        return;
    }
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(200),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(500),
    };

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC,
                           NULL,
                           BLE_HS_FOREVER,
                           &adv_params,
                           gap_event_handler,
                           NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "广播启动失败");
    }
    else
    {
        ESP_LOGE(TAG, "广播已启动，等待连接...");
        ble_adv_active = true;
    }
}

static void on_sync(void)
{
    ESP_LOGE(TAG, "设备初始化完毕");
    start_advertising();
}

void host_task(void *arg)
{
    nimble_port_run();
}

void ble_init(void)
{
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);

    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);
}