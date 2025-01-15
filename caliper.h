#include <esp_err.h>
#include <stdbool.h>


typedef enum {
    CALIPER_UNIT_MM,
    CALIPER_UNIT_INCH,
} caliper_unit_t;

typedef struct caliper *caliper_t;
typedef struct caliper_config *caliper_config_t;
typedef struct caliper_data *caliper_data_t;
typedef void (*caliper_cb_t)(caliper_t caliper, caliper_data_t data);

struct caliper_config {
    const char *name;       /// Caliper name
    int clock_pin;          /// Clock pin number
    int data_pin;           /// Data pin number
    bool invert;            /// Invert the logic for clock pin and data pin
    caliper_cb_t cb;        /// Callback function.  NULL for polling mode
};

struct caliper_data {
    const char *name;       /// Caliper name
    double value;           /// Caliper current value
    caliper_unit_t unit;    /// Caliper unit mode, can be one of:
                            ///      CALIPER_UNIT_MM
                            ///      CALIPER_UNIT_INCH
    bool power;             /// Caliper power state, true for ON, false for OFF
                            ///      If OFF, value and units are invalid
};

esp_err_t caliper_init(void);
void caliper_deinit(void);
caliper_t caliper_add(caliper_config_t config);
void caliper_remove(caliper_t caliper);
void caliper_poll(caliper_t caliper, caliper_data_t data);
