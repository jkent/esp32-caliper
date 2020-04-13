#include <esp_err.h>

typedef enum {
    CALIPER_UNITS_MM,
    CALIPER_UNITS_INCHES,
} caliper_units_t;

typedef struct caliper* caliper_handle;

typedef void (*caliper_cb_t)(caliper_handle handle, double value, caliper_units_t units, bool power);

esp_err_t caliper_init(void);
struct caliper* caliper_add(int clock_pin, int data_pin, caliper_cb_t cb);
void caliper_remove(caliper_handle handle);
void caliper_poll(caliper_handle handle, double* value, caliper_units_t* units, bool* power);
