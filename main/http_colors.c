/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "driver/touch_pad.h"
#include "button.h"
#include "driver/gpio.h"
#include "beacon.h"

#include <math.h>

#include "esp_http_client.h"

#define USE_BLUETOOTH

#define HTTP_HOST "neep"
#define HTTP_PORT 8080

#define MAX_HTTP_OUTPUT_BUFFER 2048

/* Time before going back to sleep */
#define SLEEP_TIMEOUT_MS 10000

/* A tap longer than this is considered a hold */
#define TAP_MAX_MS 500

/* Time required to sweep through all hues */
#define HUE_SWEEP_MS 12000

static const char *TAG = "HTTP Colors";

/* Single taps to change */
enum color_state_t
{
    cs_OFF,
    cs_SOLID_WHITE,
    cs_NORMAL_HIGH,
    cs_SOLID_HIGH,
    cs_SOLID_LOW,
    cs_MAX
};

/* These persist across sleep */
static RTC_DATA_ATTR enum color_state_t color_state = cs_OFF;
static RTC_DATA_ATTR int hue;

/* Device goes into deep sleep on expiry */
static TimerHandle_t sleep_timer;

/* Used to track whether we can sleept */
static bool requesting = false;
static uint64_t last_wakey_wakey;

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static int output_len;       // Stores number of bytes read

    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            output_len = 0;
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            // If user_data buffer is configured, copy the response into the buffer
            if (evt->user_data) {
                memcpy(evt->user_data + output_len, evt->data, evt->data_len);
            }
            output_len += evt->data_len;

            break;
    }
    return ESP_OK;
}

static void http_set_int_var(char *var, int value)
{
    char path_buff[100];
    sprintf(path_buff, "/vars/%s", var);

    char query_buff[100];
    sprintf(query_buff, "set=%d", value);

    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

    esp_http_client_config_t config = {
        .host = HTTP_HOST,
        .port = HTTP_PORT,
        .path = path_buff,
        .query = query_buff,
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer,        // Pass address of local buffer to get response
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // GET
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    ESP_LOG_BUFFER_CHAR(TAG, local_response_buffer, strlen(local_response_buffer));

    esp_http_client_cleanup(client);

}

static int calc_bgr(float brightness)
{
    float c = brightness;

    float wat = fmod(((float)hue / (float)60),2.0f) - 1.0f;
    if(wat < 0) wat = -wat;

    float x = c * (1 - wat);

    float rf = 0,gf = 0,bf = 0;
    if(hue < 60)       { rf = c; gf = x; bf = 0; }
    else if(hue < 120) { rf = x; gf = c; bf = 0; }
    else if(hue < 180) { rf = 0; gf = c; bf = x; }
    else if(hue < 240) { rf = 0; gf = x; bf = c; }
    else if(hue < 300) { rf = x; gf = 0; bf = c; }
    else if(hue <= 359){ rf = c; gf = 0; bf = x; }


    float m = brightness - c;

    rf += m;
    gf += m;
    bf += m;


    ESP_LOGI(TAG,"RGB: %f %f %f", rf, gf, bf);

    return
        ((int)(bf * 0xFF)) << 16 |
        ((int)(gf * 0xFF)) << 8 |
        ((int)(rf * 0xFF));
}

static void run_request_task(void *pvParameters)
{
    float brightness = 0;
    switch(color_state)
    {
    case cs_SOLID_WHITE:
    case cs_NORMAL_HIGH:
    case cs_SOLID_HIGH:
        brightness = 1;
        break;
    case cs_SOLID_LOW:
        brightness = 0.25;
        break;
    case cs_OFF:
        brightness = 0;
        break;
    default:
        break;
    }

    switch(color_state)
    {
    case cs_OFF:
#ifndef USE_BLUETOOTH
        http_set_int_var("col", 0);
#else
        beacon_set_int_var("col", 0);
#endif
        break;
    case cs_SOLID_WHITE:
#ifndef USE_BLUETOOTH

        http_set_int_var("solid_mode", 1);
        http_set_int_var("col", 0xFFFFFF);
#else
        beacon_set_int_var("solid_mode", 1);
        beacon_set_int_var("col", 0xFFFFFF);
#endif
        break;

    case cs_NORMAL_HIGH:

#ifndef USE_BLUETOOTH
        http_set_int_var("solid_mode", 0);
        http_set_int_var("col", calc_bgr(brightness));
#else
        beacon_set_int_var("solid_mode", 0);
        beacon_set_int_var("col", calc_bgr(brightness));
#endif
        break;

    case cs_SOLID_HIGH:
    case cs_SOLID_LOW:

#ifndef USE_BLUETOOTH
        http_set_int_var("solid_mode", 1);
        http_set_int_var("col", calc_bgr(brightness));
#else
        beacon_set_int_var("solid_mode", 1);
        beacon_set_int_var("col", calc_bgr(brightness));
#endif
        break;
    default:
        break;
    }

    /* Don't let us sleep until this + the sleep delay */
    last_wakey_wakey = pdTICKS_TO_MS(xTaskGetTickCount());

    /* Completed request; now it's down to last_wakey_wakey */
    requesting = false;

/*     /\* For debug *\/ */
/*     uint16_t touch_filter_value; */
/*     touch_pad_read_filtered(TOUCH_PAD_ID, &touch_filter_value); */
/* #ifndef USE_BLUETOOTH */
/*     http_set_int_var("touch_debug", touch_filter_value); */
/* #else */
/*     beacon_set_int_var("touch_debug", touch_filter_value); */
/* #endif */


    vTaskDelete(NULL);
}


static void run_request(void)
{
    requesting = true;

    /* Do an HTTP! */
    xTaskCreatePinnedToCore(&run_request_task, "http_test_task", 8192, NULL, 5, NULL, 0);
}

void button_down_event()
{
    last_wakey_wakey = pdTICKS_TO_MS(xTaskGetTickCount());
    ESP_LOGI(TAG, "button down");

    gpio_set_level(2, 1);

}

void button_up_event(uint64_t hold_ms)
{
    last_wakey_wakey = pdTICKS_TO_MS(xTaskGetTickCount());
    ESP_LOGI(TAG, "button up %" PRIu64 "ms", hold_ms);

    /* Turn the LED off */
    gpio_set_level(2, 0);


    if(requesting)
    {
        /* One request at a time */
        return;
    }

    if(hold_ms <= TAP_MAX_MS)
    {
        /* Show tap = next state */
        color_state = (color_state + 1) % cs_MAX;
        ESP_LOGI(TAG, "Next state: %d", color_state);
        run_request();
    }
    else
    {
        /* This was a hold */
        return;
    }

}

void button_hold_event(uint64_t hold_ms)
{
    static uint64_t last_update_hold_ms = TAP_MAX_MS;

    if(hold_ms <= TAP_MAX_MS)
    {
        /* Tap */
        last_update_hold_ms = TAP_MAX_MS;
    }
    else
    {
        /* Hold */
        uint64_t last_update_delta_ms = hold_ms - last_update_hold_ms;

        if(!requesting)
        {
            /* Change the color */

            /* Change it by this many degrees */
            int hue_sweep_delta = 360 * last_update_delta_ms / HUE_SWEEP_MS;
            ESP_LOGI(TAG, "button hold %" PRIu64 " ms => delta %d deg",
                     last_update_delta_ms,
                     hue_sweep_delta);

            hue = (hue + hue_sweep_delta) % 360;
            ESP_LOGI(TAG, "Next hue: %d deg", hue);

            run_request();

            last_update_hold_ms = hold_ms;
        }
    }


}

static void sleep_callback(TimerHandle_t xTimer)
{
    /* Don't sleep when busy */
    /* This is all in the main timer thread, so it's not going to change under our feet.
     * We just need to make sure there is no state where buttons are idle
     * and we need to do something, but 'requesting' is false.
     * To ensure this, we only set 'requesting' from the timer thread.
     */

    if(requesting ||
       last_wakey_wakey > pdTICKS_TO_MS(xTaskGetTickCount()) - SLEEP_TIMEOUT_MS)
    {
        /* sleep_callback is not one-shot, so we'll come here again */
        return;
    }


    ESP_LOGI(TAG, "Sleeping");

    esp_deep_sleep_start();
}



void app_main(void)
{
    /* Blue light! */

    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 1 << 2;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    /* Networking */

#ifndef USE_BLUETOOTH
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    ESP_LOGI(TAG, "Connected to AP, begin http example");

#else
    beacon_init();
#endif


    touch_pad_t tp = esp_sleep_get_touchpad_wakeup_status();

    bool woke_by_touch_pad = tp == TOUCH_PAD_ID;

    /* Init GPIO and touch pad */
    button_init(woke_by_touch_pad);

    /* Power saving stuff */

    esp_pm_config_esp32_t pm_config = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 40,
        .light_sleep_enable = true
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));

    /* Sleepy stuff */

    sleep_timer = xTimerCreate("Sleep Timer",
                              pdMS_TO_TICKS(SLEEP_TIMEOUT_MS),
                              1, // Autoreload
                              0, // Timer ID = 0
                              sleep_callback // Callback fn
        );
    xTimerStart(sleep_timer, 0);

    /* Ready to be worken from light sleep by gpio or touch pad */
    esp_sleep_enable_touchpad_wakeup();
}
