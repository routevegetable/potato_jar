#include "beacon.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <memory.h>
#include <string.h>
#include "freertos/semphr.h"


#define TAG "Beacon"

static esp_ble_adv_params_t ble_adv_params = {
    .adv_int_min        = FAST_ADV_INTERVAL,
    .adv_int_max        = FAST_ADV_INTERVAL,
    .adv_type           = ADV_TYPE_NONCONN_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static SemaphoreHandle_t ble_mutex;
static bool advertising_on = false;

static TimerHandle_t ble_timer;

static void check_for_next_message(void);


static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    esp_err_t err;
    assert(xSemaphoreTake(ble_mutex, portMAX_DELAY) == pdTRUE);

    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&ble_adv_params);
        ESP_LOGI(TAG, "Starting adv");

        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        //adv start complete event to indicate adv start successfully or failed
        if ((err = param->adv_start_cmpl.status) != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Adv start failed: %s", esp_err_to_name(err));
        }
        else
        {
            ESP_LOGI(TAG, "Started adv successful");
        }

        /* Reset the 'stop' timer */
        xTimerReset(ble_timer, 0);

        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:

        /* Stop complete. Are we here because we're changing messages or because we're done */

        if ((err = param->adv_stop_cmpl.status) != ESP_BT_STATUS_SUCCESS){
            ESP_LOGE(TAG, "Adv stop failed: %s", esp_err_to_name(err));
        }
        else {
            ESP_LOGI(TAG, "Stop adv successfully");
        }
        check_for_next_message();
        break;
    default:
        break;
    }

    xSemaphoreGive(ble_mutex);
}


const uint8_t uuid_zeros[ESP_UUID_LEN_128] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

#define ESP_UUID    {0xFD, 0xA5, 0x06, 0x93, 0xA4, 0xE2, 0x4F, 0xB1, 0xAF, 0xCF, 0xC6, 0xEB, 0x07, 0x64, 0x78, 0x25}
#define ESP_MAJOR   10167
#define ESP_MINOR   61958


typedef struct {
    uint8_t flags[3];
    uint8_t length;
    uint8_t type;
    uint16_t company_id;
    uint16_t beacon_type;
}__attribute__((packed)) esp_ble_ibeacon_head_t;

typedef struct {
    uint8_t proximity_uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t measured_power;
}__attribute__((packed)) esp_ble_ibeacon_vendor_t;


typedef struct {
    esp_ble_ibeacon_head_t ibeacon_head;
    esp_ble_ibeacon_vendor_t ibeacon_vendor;
}__attribute__((packed)) esp_ble_ibeacon_t;

/* Constant part of iBeacon data */
static esp_ble_ibeacon_head_t ibeacon_common_head = {
    .flags = {0x02, 0x01, 0x06},
    .length = 0x1A,
    .type = 0xFF,
    .company_id = 0x9001,
    .beacon_type = 0x1502
};

#define ENDIAN_CHANGE_U16(x) ((((x)&0xFF00)>>8) + (((x)&0xFF)<<8))

/* Vendor part of iBeacon data*/
static esp_ble_ibeacon_vendor_t vendor_config = {
    .proximity_uuid = ESP_UUID,
    .major = ENDIAN_CHANGE_U16(ESP_MAJOR), //Major=ESP_MAJOR
    .minor = ENDIAN_CHANGE_U16(ESP_MINOR), //Minor=ESP_MINOR
    .measured_power = 0xC5
};


static esp_err_t esp_ble_config_ibeacon_data (esp_ble_ibeacon_vendor_t *vendor_config, esp_ble_ibeacon_t *ibeacon_adv_data){
    if ((vendor_config == NULL) || (ibeacon_adv_data == NULL) || (!memcmp(vendor_config->proximity_uuid, uuid_zeros, sizeof(uuid_zeros)))){
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&ibeacon_adv_data->ibeacon_head, &ibeacon_common_head, sizeof(esp_ble_ibeacon_head_t));
    memcpy(&ibeacon_adv_data->ibeacon_vendor, vendor_config, sizeof(esp_ble_ibeacon_vendor_t));

    return ESP_OK;
}


/* A BLE message to send */
struct set_message
{
    char name[20];
    int value;
    bool dirty;
};


#define MAX_MESSAGES 5
static struct set_message messages[MAX_MESSAGES];
static int msg_ptr = 0; // points at the slot after the tail of the queue - where new thing goes


static void check_for_next_message(void)
{
    ESP_LOGI(TAG, "checking message cache for next dirty message");

    int msg_index = -1;

    for(int i = 0; i < MAX_MESSAGES; i++)
    {
        if(messages[i].dirty)
        {
            /* Do this one */
            messages[i].dirty = false;
            msg_index = i;
            break;
        }
    }

    if(msg_index == -1)
    {
        /* nothing to do */
        advertising_on = false;
        return;
    }

    ESP_LOGI(TAG, "Start adv message: %s=%d", messages[msg_index].name, messages[msg_index].value);

    /* Set up the new data */
    esp_ble_ibeacon_t ibeacon_adv_data;
    esp_err_t status = esp_ble_config_ibeacon_data (&vendor_config, &ibeacon_adv_data);

    char *uuid = (char*)ibeacon_adv_data.ibeacon_vendor.proximity_uuid;
    memcpy(uuid, &messages[msg_index].value, 4);
    strcpy(uuid + 4, messages[msg_index].name);

    if (status == ESP_OK) {
        esp_ble_gap_config_adv_data_raw((uint8_t*)&ibeacon_adv_data, sizeof(ibeacon_adv_data));
        advertising_on = true;
    }
    else {
        ESP_LOGE(TAG, "Config iBeacon data failed: %s\n", esp_err_to_name(status));
    }
}


/* Fires when we should go quiet after no new activity */
static void ble_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "BLE Timer");

    if(advertising_on)
    {
        /* Stop broadcasting the current message */
        esp_ble_gap_stop_advertising();
    }
}

void beacon_init(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);


    for(int i = 0; i < MAX_MESSAGES; i++)
    {
        messages[i].dirty = false;
        messages[i].name[0] = '\0';
    }

    ble_mutex = xSemaphoreCreateMutex();
    esp_bluedroid_init();
    esp_bluedroid_enable();
    esp_err_t status;
    if ((status = esp_ble_gap_register_callback(esp_gap_cb)) != ESP_OK) {
        ESP_LOGE(TAG, "gap register error: %s", esp_err_to_name(status));
        return;
    }


    ble_timer = xTimerCreate("BLE Timer",
                             pdMS_TO_TICKS(RETRANSMIT_TIME_MS),
                             0, // No autoreload
                             0, // Timer ID = 0
                             ble_timer_callback // Callback fn
        );
}

void beacon_set_int_var(char *name, int value)
{
    assert(xSemaphoreTake(ble_mutex, portMAX_DELAY) == pdTRUE);

    ESP_LOGI(TAG, "set %s=%d, advertising_on=%d", name, value, advertising_on);

    // Add the new message to the cache

    int msg_index = -1;

    for(int i = 0; i < MAX_MESSAGES; i++)
    {
        if(!strcmp(messages[i].name, name))
        {
            msg_index = i;
            break;
        }
    }

    bool dirtied = false;

    if(msg_index != -1)
    {
        /* Found it in the cache */
        if(messages[msg_index].value == value)
        {
            /* This message has already been handled */
            xSemaphoreGive(ble_mutex);
            ESP_LOGI(TAG, "found matching message");
            return;
        }
        else
        {
            /* This value has changed */
            messages[msg_index].value = value;
            if(!messages[msg_index].dirty)
            {
                dirtied = true;
                messages[msg_index].dirty = true;
            }

            ESP_LOGI(TAG, "Updated message, marked dirty");
        }
    }
    else
    {
        /* Add a new message */
        strcpy(messages[msg_ptr].name, name);
        messages[msg_ptr].value = value;
        messages[msg_ptr].dirty = true;
        dirtied = true;
        msg_ptr = (msg_ptr + 1) % MAX_MESSAGES;
        ESP_LOGI(TAG, "Added new message");

    }

    if(dirtied && !advertising_on)
    {
        /* Start advertising again */
        check_for_next_message();
    }

    xSemaphoreGive(ble_mutex);
}
