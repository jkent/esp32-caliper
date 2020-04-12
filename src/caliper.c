#include <freertos/FreeRTOS.h>

#include "caliper.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sys/queue.h"

const char TAG[] = "caliper";

#define CALIPER_HOST SPI3_HOST

SLIST_HEAD(caliper_head, caliper) caliper_head =
    SLIST_HEAD_INITIALIZER(caliper_head);

struct caliper {
    SLIST_ENTRY(caliper) entries;
    int clock_pin;
    int data_pin;
    portMUX_TYPE mutex;
    volatile uint32_t sample;
    volatile int64_t last_seen;
    uint32_t isr_data;
    uint32_t isr_bit_count;
};


esp_err_t caliper_init(void)
{
    if (SLIST_FIRST(&caliper_head) != NULL) {
        ESP_LOGE(TAG, "already initialized!");
        return ESP_FAIL;
    }

    SLIST_INIT(&caliper_head);
    gpio_install_isr_service(0);

    return ESP_OK;
}


static void IRAM_ATTR gpio_clock_isr_handler(void* arg)
{
    struct caliper* caliper = (struct caliper*) arg;

    int clock = gpio_get_level(caliper->clock_pin); /* TODO: invert */
    int data = gpio_get_level(caliper->data_pin); /* TODO: invert */
    uint64_t now = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&caliper->mutex);
    if (clock) {
        caliper->isr_data = (data << 23) | (caliper->isr_data >> 1);
        caliper->isr_bit_count += 1;
        if (caliper->isr_bit_count == 24) {
            caliper->sample = caliper->isr_data;
        }
    } else if (now - caliper->last_seen > 1000) {
        caliper->isr_bit_count = 0;
    }
    caliper->last_seen = now;
    portEXIT_CRITICAL_ISR(&caliper->mutex);
}


struct caliper* caliper_add(int clock_pin, int data_pin)
{
    struct caliper *caliper = calloc(1, sizeof(struct caliper));
    assert(caliper != NULL);

    vPortCPUInitializeMutex(&caliper->mutex);

    caliper->clock_pin = clock_pin;
    caliper->data_pin = data_pin;
    caliper->sample = 0;

    gpio_config_t config = {
        .pin_bit_mask = 1 << clock_pin | 1 << data_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
    };
    gpio_config(&config);
    gpio_set_intr_type(clock_pin, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(clock_pin, gpio_clock_isr_handler, caliper);

    SLIST_INSERT_HEAD(&caliper_head, caliper, entries);
    return caliper;
}


void caliper_remove(struct caliper* caliper)
{
    gpio_isr_handler_remove(caliper->clock_pin);

    SLIST_REMOVE(&caliper_head, caliper, caliper, entries);
}


double caliper_value(struct caliper* caliper, caliper_mode_t* mode, bool* power)
{
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&caliper->mutex);
    int sample = caliper->sample;
    if (power) {
        *power = now - caliper->last_seen < 200000;
    }
    portEXIT_CRITICAL(&caliper->mutex);

    int sign = (sample >> 20) & 1;
    int _mode = (sample >> 23) & 1;
    sample &= 0x0FFFFF;
    if (sign) {
        sample *= -1;
    }
    if (mode) {
        *mode = _mode;
    }
    return _mode ? (sample * 5) / 10000.0l : sample / 100.0l;
}
