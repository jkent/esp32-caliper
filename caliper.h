#include <esp_err.h>
#include <stdbool.h>


typedef enum {
    CALIPER_UNITS_MM,
    CALIPER_UNITS_INCHES,
} caliper_units_t;

typedef struct caliper *caliper_t;
typedef struct caliper_config *caliper_config_t;
typedef struct caliper_data *caliper_data_t;
typedef void (*caliper_callback_t)(caliper_t caliper, caliper_data_t data);

struct caliper_config {
    const char *name;
    int clock_pin;
    int data_pin;
    bool invert;
    caliper_callback_t callback;
};

struct caliper_data {
    const char *name;
    double value;
    caliper_units_t units;
    bool power;
};

esp_err_t caliper_init(void);
void caliper_deinit(void);
caliper_t caliper_add(caliper_config_t config);
void caliper_remove(caliper_t caliper);
void caliper_poll(caliper_t caliper, caliper_data_t data);
