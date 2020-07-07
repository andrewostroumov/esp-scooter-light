#include <hal/gpio_types.h>
#include "stdbool.h"
#include "esp_err.h"

typedef enum {
    LIGHT_MOOD_LOW,
    LIGHT_MOOD_MEDIUM,
    LIGHT_MOOD_HIGH,
    LIGHT_MOOD_MAX,
    LIGHT_MOOD_BLINK,
} light_mood_t;

typedef struct {
    bool power;
    bool blink;
    light_mood_t mood;
    uint32_t delay;
    gpio_num_t pin;
} light_handle_t;


esp_err_t light_init(light_handle_t *light_handle);

esp_err_t light_config(light_handle_t *light_handle);

void light_action(light_handle_t *light_handle, int event);

void light_blink_action(light_handle_t *light_handle);

int light_brightness(light_handle_t *light_handle);

esp_err_t light_apply(light_handle_t *light_handle);
