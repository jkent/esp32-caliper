#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "caliper.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sys/queue.h"


#define TAG "caliper"

#define MAX_IDLE_US 200000

struct caliper {
    SLIST_ENTRY(caliper) entries;
    struct caliper_config config;
    spinlock_t spinlock;
    uint32_t gpio_data;
    uint8_t gpio_bits;
    volatile bool power;
    volatile uint32_t sample;
    volatile int64_t last_seen;
};

SLIST_HEAD(caliper_head, caliper) caliper_head =
    SLIST_HEAD_INITIALIZER(caliper_head);

static QueueHandle_t queue = NULL;
static gptimer_handle_t gptimer = NULL;
static volatile bool stop = false;
static volatile bool running = false;

static void IRAM_ATTR gpio_clock_isr_handler(void* arg)
{
    caliper_t caliper = (caliper_t) arg;

    uint64_t now = esp_timer_get_time();
    int clock = gpio_get_level(caliper->config.clock_pin);
    int data = gpio_get_level(caliper->config.data_pin);
    if (caliper->config.invert) {
        clock = !clock;
        data = !data;
    }

    portENTER_CRITICAL_ISR(&caliper->spinlock);
    if (clock) {
        caliper->power = true;
        caliper->gpio_data = (data << 23) | (caliper->gpio_data >> 1);
        caliper->gpio_bits += 1;
        if (caliper->gpio_bits == 24) {
            caliper->sample = caliper->gpio_data;
            xQueueSendFromISR(queue, &caliper, NULL);
        }
    } else if (now - caliper->last_seen > 1000) {
        caliper->gpio_bits = 0;
    }
    caliper->last_seen = now;
    portEXIT_CRITICAL_ISR(&caliper->spinlock);
}

static bool gptimer_alarm_cb(gptimer_handle_t timer,
        const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    caliper_t caliper;

    uint64_t now = esp_timer_get_time();

    SLIST_FOREACH(caliper, &caliper_head, entries) {
        portENTER_CRITICAL_ISR(&caliper->spinlock);
        if (caliper->power && (now - caliper->last_seen >= MAX_IDLE_US)) {
            caliper->power = false;
            xQueueSendFromISR(queue, &caliper, NULL);
        }
        portEXIT_CRITICAL_ISR(&caliper->spinlock);
    }

    return false;
}

static void caliper_task(void* arg)
{
    caliper_t caliper;
    struct caliper_data data;

    running = true;

    while (!stop) {
        if (xQueueReceive(queue, &caliper, pdMS_TO_TICKS(100))) {
            if (!caliper->config.cb) {
                continue;
            }

            portENTER_CRITICAL(&caliper->spinlock);
            int sample = caliper->sample;
            data.power = caliper->power;
            data.name = caliper->config.name;
            portEXIT_CRITICAL(&caliper->spinlock);

            data.unit = (sample >> 23) & 1;
            data.value = sample & 0x0FFFFF;
            if ((sample >> 20) & 1) {
                data.value *= -1;
            }
            if (data.unit == CALIPER_UNIT_MM) {
                data.value = data.value / 100.0L;
            } else {
                data.value = data.value / 2000.0L;
            }

            caliper->config.cb(caliper, &data);
        }
    }

    running = false;
    vTaskDelete(NULL);
}

esp_err_t caliper_init(void)
{
    if (queue != NULL) {
        ESP_LOGE(TAG, "Already initialized!");
        return ESP_FAIL;
    }

    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not install gpio isr service!");
        return err;
    }

    SLIST_INIT(&caliper_head);

    stop = false;
    running = true;
    queue = xQueueCreate(5, sizeof(struct caliper*));
    xTaskCreate(caliper_task, "caliper_task", 2048, NULL, tskIDLE_PRIORITY + 2,
            NULL);

    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000, // 1us
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = 1 * 1000 * 10, // 10ms
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    gptimer_event_callbacks_t gptimer_callbacks = {
        .on_alarm = gptimer_alarm_cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer,
            &gptimer_callbacks, NULL));

    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));

    return ESP_OK;
}

caliper_t caliper_add(caliper_config_t caliper_config)
{
    caliper_t caliper = calloc(1, sizeof(struct caliper));
    assert(caliper != NULL);

    memcpy(&caliper->config, caliper_config, sizeof(caliper->config));

    portMUX_INITIALIZE(&caliper->spinlock);
    caliper->sample = 0;

    int clock_pin_bit = BIT(caliper->config.clock_pin);
    int data_pin_bit = BIT(caliper->config.data_pin);

    gpio_config_t config = {
        .pin_bit_mask = clock_pin_bit | data_pin_bit,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
    };
    gpio_config(&config);
    gpio_set_intr_type(caliper->config.clock_pin, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(caliper->config.clock_pin, gpio_clock_isr_handler,
            caliper);

    SLIST_INSERT_HEAD(&caliper_head, caliper, entries);

    return caliper;
}

void caliper_remove(caliper_t caliper)
{
    gpio_isr_handler_remove(caliper->config.clock_pin);

    SLIST_REMOVE(&caliper_head, caliper, caliper, entries);

    free(caliper);
}

void caliper_deinit(void)
{
    caliper_t caliper, temp;

    gptimer_stop(gptimer);
    gptimer_disable(gptimer);
    gptimer_del_timer(gptimer);
    stop = true;
    while (running)
        ;
    vQueueDelete(queue);
    queue = NULL;

    SLIST_FOREACH_SAFE(caliper, &caliper_head, entries, temp) {
        caliper_remove(caliper);
    }

    // Optional: remove the gpio service -- left installed for user's code
    // gpio_uninstall_isr_service();
}

void caliper_poll(caliper_t caliper, caliper_data_t data)
{
    assert(data != NULL);

    portENTER_CRITICAL(&caliper->spinlock);
    int sample = caliper->sample;
    data->power = caliper->power;
    data->name = caliper->config.name;
    portEXIT_CRITICAL(&caliper->spinlock);

    data->unit = (sample >> 23) & 1;
    data->value = sample & 0x0FFFFF;
    if ((sample >> 20) & 1) {
        data->value *= -1;
    }
    if (data->unit == CALIPER_UNIT_MM) {
        data->value = data->value / 100.0L;
    } else {
        data->value = data->value / 2000.0L;
    }
}
