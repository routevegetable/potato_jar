#include "button.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <driver/touch_pad.h>
#include <assert.h>
#include <stdbool.h>
#include <esp_log.h>

#define TAG "Button"

/* Active whenever the button is pushed */
static TimerHandle_t debounce_timer;

static bool last_button_reading = false;

/* When did the current button hold start */
static uint64_t hold_start_ms;

static int isr_calls = 0;

static bool ignore_intr = false;

static void button_isr(void *arg)
{
    /* Gets called repeatedly while touch pad is pushed */
    isr_calls++;
    touch_pad_clear_status();

    /* (re)start the debounce timer */

    if(ignore_intr)
    {
        /* We know we're being pushed.
         * being handled by the debounce timer now
         */
        return;
    }

    ignore_intr = true;

    int yield;
    xTimerResetFromISR(debounce_timer, &yield);

    if(yield)
    {
        portYIELD_FROM_ISR();
    }
}

/* Check if any buttons are pushed */
static int button_pushed()
{
    uint16_t touch_filter_value;
    //touch_pad_read_filtered(TOUCH_PAD_ID, &touch_filter_value);
    touch_pad_read(TOUCH_PAD_ID, &touch_filter_value);
    ESP_LOGI(TAG, "touch pad value: %d", touch_filter_value);
    return touch_filter_value < TOUCH_THRESHOLD;
}


/* This is called by the debounce timer. */
static void debug_callback(TimerHandle_t xTimer)
{
    button_pushed();
    ESP_LOGI(TAG, "ISR calls: %d", isr_calls);
}

/* This is called by the debounce timer. */
static void debounce_callback(TimerHandle_t xTimer)
{
    bool pushed_now = button_pushed();
    //bool pushed_now = touch_pad_get_status();

    uint64_t hold_time = pdTICKS_TO_MS(xTaskGetTickCount()) - hold_start_ms;

    ESP_LOGI(TAG, "%d -> %d", last_button_reading, pushed_now);

    if(!last_button_reading && pushed_now)
    {
        hold_start_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        last_button_reading = true;

        /* Rising edge */
        button_down_event();

        xTimerReset(debounce_timer, 0);
    }
    else if (last_button_reading && !pushed_now)
    {
        /* Falling edge */
        last_button_reading = false;

        button_up_event(hold_time);

        ignore_intr = false;
    }
    else if(last_button_reading && pushed_now)
    {
        /* Still pushed */
        button_hold_event(hold_time);

        xTimerReset(debounce_timer, 0);
    }
    else
    {
        /* Blip - ignore */
        ignore_intr = false;
    }
}


void button_init(bool pushed_on)
{

    /* Debouncing stuff */
    debounce_timer = xTimerCreate("Debounce Timer",
                                  pdMS_TO_TICKS(DEBOUNCE_MS), // 200ms
                                  0, // No autoreload
                                  0, // Timer ID = 0
                                  debounce_callback // Callback fn
        );


    /* Debouncing stuff */
    TimerHandle_t debug = xTimerCreate("Debug Timer",
                 pdMS_TO_TICKS(500), // 200ms
                 1,
                 0, // Timer ID = 0
                 debug_callback // Callback fn
        );

    xTimerStart(debug, 0);


    if(pushed_on)
    {
        /* We were woken by a push! */
        touch_pad_clear_status();
        ignore_intr = true;
        hold_start_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        last_button_reading = true;
        xTimerReset(debounce_timer, 0);
    }

    /* Touch Pad */
    touch_pad_init();
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);

    // Lowest voltage range possible cuz batteries suck
    touch_pad_set_voltage(TOUCH_HVOLT_2V4, TOUCH_LVOLT_0V8, TOUCH_HVOLT_ATTEN_1V);
    touch_pad_config(TOUCH_PAD_ID, TOUCH_THRESHOLD);
    touch_pad_set_trigger_mode(TOUCH_TRIGGER_BELOW);
    //touch_pad_filter_start(20); // 20ms filter
    touch_pad_set_group_mask(1 << TOUCH_PAD_ID, 0, 1 << TOUCH_PAD_ID);

    // Make the sleep cycle long
    touch_pad_set_meas_time(TOUCH_SLEEP_CYCLE, TOUCH_PAD_MEASURE_CYCLE_DEFAULT);

    touch_trigger_src_t src;
    touch_pad_get_trigger_source(&src);

    ESP_LOGI(TAG, "trigger source: %d", src);
    touch_pad_set_trigger_source(TOUCH_TRIGGER_SOURCE_SET1);
    touch_pad_isr_register(button_isr, NULL);
    touch_pad_intr_enable();

}
