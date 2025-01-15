# esp32-caliper

This is an ESP-IDF component that allows you to read the value off of digital
calipers.  A caliper can be read in one of two modes: callback, or polling. It
has been written to be compatible with ESP-IDF 5.4 as of the time of this
writing.

## Hardware Setup

TODO

## Component Setup

To add this component to your IDF project, place this component in your
project's component directory.  If you're on Linux or OSX, you would do the
following from your project's root directory:

```sh
mkdir -p components
cd components
git checkout https://github.com/jkent/esp32-caliper
```

Then add `esp32-caliper` to your main component's `CMakeLists.txt`'s
`PRIV_REQUIRES` section.  For example:

```cmake
idf_component_register(
SRCS
    "main.c"
PRIV_REQUIRES
    esp32-caliper
)
```

Now you're ready to use the component in your code.

## Component Usage

Lets dive right in!

Here is some common code for all of our examples, it includes the headers and
the caliper print function:

```c
#include <stdbool.h>
#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "caliper.h"

static void caliper_print(caliper_data_t data)
{
    if (data->power) {
        const char *unit = (data->units == CALIPER_UNIT_MM) ? "mm" : "in";
        printf("%s: %.2f %s\n", data->name, data->value, unit);
    } else {
        printf("%s: OFF\n", data->name);
    }
}
```

Lets look at the documentation for our data struct:

```c
struct caliper_data {
    const char *name;       /// Caliper name
    double value;           /// Caliper current value
    caliper_unit_t unit;    /// Caliper unit mode, can be one of:
                            ///      CALIPER_UNIT_MM
                            ///      CALIPER_UNIT_INCH
    bool power;             /// Caliper power state, true for ON, false for OFF
                            ///      If OFF, value and units are invalid
};
```

OK. So now we know how to use the data that the caliper component gives us.
Lets set up a caliper in callback mode!

### Callback mode

```c
static void caliper_cb(caliper_t caliper, caliper_data_t data)
{
    caliper_print(data);
}

void app_main(void)
{
    caliper_init(); // This must be called before using the caliper component

    struct caliper_config config_x = {
        .name = "X",
        .clock_pin = 26,
        .data_pin = 27,
        .cb = caliper_cb,
    };

    caliper_t caliper_x = caliper_add(&config_x);
    if (caliper == NULL) {
        ESP_LOGE(__FUNCTION__, "caliper_add for X failed");
        goto done;
    }
```

OK cool, thats not too bad!  Lets take a peek at the documentation for our
caliper_config struct:

```c
struct caliper_config {
    const char *name;       /// Caliper name
    int clock_pin;          /// Clock pin number
    int data_pin;           /// Data pin number
    bool invert;            /// Invert the logic for clock pin and data pin
    caliper_cb_t cb;        /// Callback function.  NULL for polling mode
};
```

### Polling mode

Finally, lets add caliper with inverted logic and polling code:

```c
    struct caliper_config config_y = {
        .name = "Y",
        .clock_pin = 14,
        .data_pin = 15,
        .invert = true,
    };

    caliper_t caliper_y = caliper_add(&config_y);
    if (caliper == NULL) {
        ESP_LOGE(__FUNCTION__, "caliper_add for Y failed");
        goto done;
    }

    for (int i = 0; i < 30; i++) {
        struct caliper_data data;
        vTaskDelay(pdMS_TO_TICKS(1000));
        caliper_poll(caliper_y, &data);
        caliper_print(&data);
    }

done:
    printf("cleaning up!\n");
    caliper_deinit();
}
```

Hopefully that should get you started!  Happy measuring!
