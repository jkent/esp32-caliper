#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "caliper.h"
#include "driver/gpio.h"
#include "driver/timer.h"
#include "esp_log.h"
#include "sys/queue.h"

const char TAG[] = "caliper";

#define MAX_IDLE_CLOCK 200000
#define CALIPER_HOST SPI3_HOST
#define TIMER_DIVIDER (16)
#define TIMER_SCALE (TIMER_BASE_CLK / TIMER_DIVIDER)

struct caliper {
    SLIST_ENTRY(caliper) entries;
    const char *name;
    int clock_pin;
    int data_pin;
    caliper_cb_t cb;
    portMUX_TYPE mutex;
    uint32_t gpio_data;
    uint8_t gpio_bits;
    volatile bool power;
    volatile uint32_t sample;
    volatile int64_t last_seen;
};

SLIST_HEAD(caliper_head, caliper) caliper_head =
    SLIST_HEAD_INITIALIZER(caliper_head);

static xQueueHandle queue = NULL;


static void IRAM_ATTR gpio_clock_isr_handler(void* arg)
{
    caliper_handle handle = (caliper_handle)arg;

    int clock = gpio_get_level(handle->clock_pin); /* TODO: invert */
    int data = gpio_get_level(handle->data_pin); /* TODO: invert */
    uint64_t now = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&handle->mutex);
    if (clock) {
        handle->power = true;
        handle->gpio_data = (data << 23) | (handle->gpio_data >> 1);
        handle->gpio_bits += 1;
        if (handle->gpio_bits == 24) {
            handle->sample = handle->gpio_data;
            xQueueSendFromISR(queue, &handle, NULL);
        }

    } else if (now - handle->last_seen > 1000) {
        handle->gpio_bits = 0;
    }
    handle->last_seen = now;
    portEXIT_CRITICAL_ISR(&handle->mutex);
}


static void IRAM_ATTR timer_isr_handler(void* arg)
{
    timer_spinlock_take(TIMER_GROUP_0);
    caliper_handle handle;

    uint64_t now = esp_timer_get_time();

    SLIST_FOREACH(handle, &caliper_head, entries) {
        portENTER_CRITICAL_ISR(&handle->mutex);
        if (handle->power && (now - handle->last_seen >= MAX_IDLE_CLOCK)) {
            handle->power = false;
            xQueueSendFromISR(queue, &handle, NULL);
        }
        portEXIT_CRITICAL_ISR(&handle->mutex);
    }

    timer_group_clr_intr_status_in_isr(TIMER_GROUP_0, 0);
    timer_group_enable_alarm_in_isr(TIMER_GROUP_0, 0);
    timer_spinlock_give(TIMER_GROUP_0);
}


static void caliper_task(void* arg)
{
    caliper_handle handle;
    struct caliper_data data;

    while (true) {
        if (xQueueReceive(queue, &handle, portMAX_DELAY)) {
            if (!handle->cb) {
                continue;
            }

            portENTER_CRITICAL(&handle->mutex);
            int sample = handle->sample;
            data.power = handle->power;
            data.name = handle->name;
            portEXIT_CRITICAL(&handle->mutex);

            data.units = (sample >> 23) & 1;
            data.value = sample & 0x0FFFFF;
            if ((sample >> 20) & 1) {
                data.value *= -1;
            }
            if (data.units == CALIPER_UNITS_MM) {
                data.value = data.value / 100.0l;
            } else {
                data.value = data.value / 2000.0l;
            }

            handle->cb(handle, &data);
        }
    }
}


esp_err_t caliper_init(void)
{
    if (queue != NULL) {
        ESP_LOGE(TAG, "already initialized!");
        return ESP_FAIL;
    }

    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not install gpio isr service!");
        return err;
    }

    SLIST_INIT(&caliper_head);

    queue = xQueueCreate(5, sizeof(struct caliper*));
    xTaskCreate(caliper_task, "caliper_task", 2048, NULL, 10, NULL);

    timer_config_t timer_config = {
        .divider = TIMER_DIVIDER,
        .counter_dir = TIMER_COUNT_UP,
        .counter_en = TIMER_PAUSE,
        .alarm_en = TIMER_ALARM_EN,
        .intr_type = TIMER_INTR_LEVEL,
        .auto_reload = true,
#ifdef TIMER_GROUP_SUPPORTS_XTAL_CLOCK
        .clk_src = TIMER_SRC_CLK_APB,
#endif
    };

    timer_init(TIMER_GROUP_0, 0, &timer_config);
    timer_set_counter_value(TIMER_GROUP_0, 0, 0ULL);
    timer_set_alarm_value(TIMER_GROUP_0, 0, 0.1l * TIMER_SCALE);
    timer_enable_intr(TIMER_GROUP_0, 0);
    timer_isr_register(TIMER_GROUP_0, 0, timer_isr_handler, NULL,
        ESP_INTR_FLAG_IRAM, NULL);
    timer_start(TIMER_GROUP_0, 0);

    return ESP_OK;
}


caliper_handle caliper_add(const char *name, int clock_pin, int data_pin, caliper_cb_t cb)
{
    caliper_handle handle = calloc(1, sizeof(struct caliper));
    assert(handle != NULL);

    vPortCPUInitializeMutex(&handle->mutex);

    handle->name = name;
    handle->clock_pin = clock_pin;
    handle->data_pin = data_pin;
    handle->cb = cb;

    handle->sample = 0;

    gpio_config_t config = {
        .pin_bit_mask = 1 << clock_pin | 1 << data_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
    };
    gpio_config(&config);
    gpio_set_intr_type(clock_pin, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(clock_pin, gpio_clock_isr_handler, handle);

    SLIST_INSERT_HEAD(&caliper_head, handle, entries);

    return handle;
}


void caliper_remove(caliper_handle handle)
{
    gpio_isr_handler_remove(handle->clock_pin);

    SLIST_REMOVE(&caliper_head, handle, caliper, entries);

    free(handle);
}

void caliper_poll(caliper_handle handle, caliper_data data)
{
    assert(data != NULL);

    portENTER_CRITICAL(&handle->mutex);
    int sample = handle->sample;
    data->power = handle->power;
    data->name = handle->name;
    portEXIT_CRITICAL(&handle->mutex);

    data->units = (sample >> 23) & 1;
    data->value = sample & 0x0FFFFF;
    if ((sample >> 20) & 1) {
        data->value *= -1;
    }
    if (data->units == CALIPER_UNITS_MM) {
        data->value = data->value / 100.0l;
    } else {
        data->value = data->value / 2000.0l;
    }
}
