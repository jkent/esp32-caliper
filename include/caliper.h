#include <esp_err.h>

typedef enum caliper_mode_t {
    CALIPER_MODE_MM,
    CALIPER_MODE_INCHES,
} caliper_mode_t;

esp_err_t caliper_init(void);
struct caliper* caliper_add(int clock_pin, int data_pin);
void caliper_remove(struct caliper* caliper);
double caliper_value(struct caliper* caliper, caliper_mode_t* mode, bool* power);