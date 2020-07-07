#include <stdbool.h>
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <driver/ledc.h>
#include <cJSON.h>
#include <esp_log.h>
#include "string.h"

#include "nvs_flash.h"
#include "esp_system.h"
#include "holo.h"

#define LEDC_FREQ_HZ 5000
#define DEFAULT_FADE_MS 500
#define LOG_TAG "holo"

static const double max_brightness = 255;

static ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = LEDC_FREQ_HZ,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
        .clk_cfg = LEDC_AUTO_CLK,
};

ledc_channel_config_t ledc_red_channel = {
        .channel    = LEDC_CHANNEL_1,
        .duty       = 0,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .hpoint     = 0,
        .timer_sel  = LEDC_TIMER_1,
        .intr_type  = LEDC_INTR_FADE_END
};

ledc_channel_config_t ledc_green_channel = {
        .channel    = LEDC_CHANNEL_2,
        .duty       = 0,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .hpoint     = 0,
        .timer_sel  = LEDC_TIMER_1,
        .intr_type  = LEDC_INTR_FADE_END
};

ledc_channel_config_t ledc_blue_channel = {
        .channel    = LEDC_CHANNEL_3,
        .duty       = 0,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .hpoint     = 0,
        .timer_sel  = LEDC_TIMER_1,
        .intr_type  = LEDC_INTR_FADE_END
};

holo_err_t holo_init(holo_handle_t *holo_handle) {
    holo_handle->effects = NULL;
    holo_handle->effects_length = 0;
    holo_handle->power = false;
    holo_handle->eid = 0;
    holo_handle->sid = 0;

    holo_load_default_effects(holo_handle);
    holo_config(holo_handle);
    return HOLO_OK;
}

// TODO: load effects from store
holo_err_t holo_load_default_effects(holo_handle_t *holo_handle) {
    holo_handle->effects_length = 2;
    holo_handle->effects = malloc(holo_handle->effects_length * sizeof(holo_effect_t));

    holo_effect_t police = {
            .states_length = 2,
            .states = malloc(2 * sizeof(holo_state_t))
    };

    holo_effect_t rand = {
            .states_length = 32,
            .states = malloc(32 * sizeof(holo_state_t))
    };

    police.states[0] = (holo_state_t) {
            .red = 255,
            .green = 0,
            .blue = 0,
            .brightness = 255,
            .fade = 500,
            .delay = 500
    };

    police.states[1] = (holo_state_t) {
            .red = 0,
            .green = 0,
            .blue = 255,
            .brightness = 255,
            .fade = 500,
            .delay = 500
    };

    for (int i = 0; i < 32; ++i) {
        rand.states[i] = (holo_state_t) {
                .red = esp_random() % 255,
                .green = esp_random() % 255,
                .blue = esp_random() % 255,
                .brightness = esp_random() % 255,
                .fade = 500,
                .delay = 500
        };
    }

    holo_handle->effects[0] = police;
    holo_handle->effects[1] = rand;

    return HOLO_OK;
}

holo_err_t holo_config(holo_handle_t *holo_handle) {
    ledc_red_channel.gpio_num = holo_handle->red_pin;
    ledc_green_channel.gpio_num = holo_handle->green_pin;
    ledc_blue_channel.gpio_num = holo_handle->blue_pin;

    ledc_timer_config(&ledc_timer);
    ledc_channel_config(&ledc_red_channel);
    ledc_channel_config(&ledc_green_channel);
    ledc_channel_config(&ledc_blue_channel);
    ledc_fade_func_install(0);
    return HOLO_OK;
}

holo_err_t holo_load(holo_handle_t *holo_handle, const char *default_effects) {
    char *effects;
    uint8_t *data;
    holo_err_t err;
    nvs_handle_t nvs;

    err = nvs_open(holo_handle->namespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    size_t required_size = 0;
    err = nvs_get_blob(nvs, holo_handle->key, NULL, &required_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;

//    if (required_size) {
//        data = malloc(required_size * sizeof(uint8_t));
//        err = nvs_get_blob(nvs, holo_handle->key, data, &required_size);
//        if (err != ESP_OK) return err;
//        effects = (char *) data;
//    } else {
//        effects = (char *) default_effects;
//    }

    effects = (char *) default_effects;

    err = holo_deserialize(holo_handle, effects);
    nvs_close(nvs);
    return err;
}

holo_err_t holo_save(holo_handle_t *holo_handle, char *data) {
    holo_err_t err;
    nvs_handle_t nvs;

    err = nvs_open(holo_handle->namespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(nvs, holo_handle->key, data, strlen(data));
    if (err != ESP_OK) return err;

    nvs_close(nvs);
    return HOLO_OK;
}

// NOTE: event is enum
void holo_action(holo_handle_t *holo_handle, int event) {
    if (event) {
        holo_handle->power = false;
        holo_handle->eid = 0;
        holo_handle->sid = 0;
        return;
    }

    if (!holo_handle->power) {
        holo_handle->power = true;
        holo_handle->sid = 0;
        holo_handle->eid = 0;
        return;
    }

    holo_handle->eid = (holo_handle->eid + 1) % holo_handle->effects_length;
    holo_handle->sid = 0;
}

void holo_state_increment(holo_handle_t *holo_handle) {
    if (!holo_handle->effects) { return; }
    holo_effect_t *holo_effect = &holo_handle->effects[holo_handle->eid];

    if (holo_effect->bits) {
        return;
    }

    holo_handle->sid = (holo_handle->sid + 1) % holo_effect->states_length;
}

holo_state_t *holo_get_state(holo_handle_t *holo_handle) {
    if (!holo_handle->effects) { return NULL; }
    holo_effect_t *holo_effect = &holo_handle->effects[holo_handle->eid];

    if (holo_effect->bits & HOLO_BIT_RAND) {
        return holo_rand_state(holo_handle);
    }

    if (!holo_effect->states) { return NULL; }
    return &holo_effect->states[holo_handle->sid];
}

holo_state_t *holo_rand_state(holo_handle_t *holo_handle) {
    holo_state_t *holo_state = malloc(sizeof(holo_state_t));
    holo_state->fade = 300;
    holo_state->delay = 200;
    holo_state->brightness = 255;

    uint8_t *data = malloc(sizeof(uint8_t));

    esp_fill_random((void *) data, 1);
    holo_state->red = *data;

    esp_fill_random((void *) data, 1);
    holo_state->green = *data;

    esp_fill_random((void *) data, 1);
    holo_state->blue = *data;

    free(data);

    return holo_state;
}

void holo_free_state(holo_handle_t *holo_handle, holo_state_t *holo_state) {
    if (!holo_state) {
        return;
    }

    if (!holo_state->bits) {
        return;
    }

    free(holo_state);
}

holo_err_t holo_state_apply(holo_handle_t *holo_handle, holo_state_t *holo_state) {
    if (!holo_handle->power) {
        ledc_set_fade_with_time(ledc_red_channel.speed_mode, ledc_red_channel.channel, 0, DEFAULT_FADE_MS);
        ledc_set_fade_with_time(ledc_green_channel.speed_mode, ledc_green_channel.channel, 0, DEFAULT_FADE_MS);
        ledc_set_fade_with_time(ledc_blue_channel.speed_mode, ledc_blue_channel.channel, 0, DEFAULT_FADE_MS);

        ledc_fade_start(ledc_red_channel.speed_mode, ledc_red_channel.channel, LEDC_FADE_NO_WAIT);
        ledc_fade_start(ledc_green_channel.speed_mode, ledc_green_channel.channel, LEDC_FADE_NO_WAIT);
        ledc_fade_start(ledc_blue_channel.speed_mode, ledc_blue_channel.channel, LEDC_FADE_NO_WAIT);

        vTaskDelay(DEFAULT_FADE_MS / portTICK_PERIOD_MS);
        return HOLO_OK;
    }

    if (holo_state) {
        double c = holo_state->brightness / max_brightness;

        if (holo_state->fade) {
            ledc_set_fade_with_time(ledc_red_channel.speed_mode, ledc_red_channel.channel, (int) (holo_state->red * c),
                                    holo_state->fade);
            ledc_set_fade_with_time(ledc_green_channel.speed_mode, ledc_green_channel.channel,
                                    (int) (holo_state->green * c), holo_state->fade);
            ledc_set_fade_with_time(ledc_blue_channel.speed_mode, ledc_blue_channel.channel,
                                    (int) (holo_state->blue * c),
                                    holo_state->fade);

            ledc_fade_start(ledc_red_channel.speed_mode, ledc_red_channel.channel, LEDC_FADE_NO_WAIT);
            ledc_fade_start(ledc_green_channel.speed_mode, ledc_green_channel.channel, LEDC_FADE_NO_WAIT);
            ledc_fade_start(ledc_blue_channel.speed_mode, ledc_blue_channel.channel, LEDC_FADE_NO_WAIT);
            vTaskDelay(DEFAULT_FADE_MS / portTICK_PERIOD_MS);
        } else {
            ledc_set_duty(ledc_red_channel.speed_mode, ledc_red_channel.channel, (int) (holo_state->red * c));
            ledc_set_duty(ledc_green_channel.speed_mode, ledc_green_channel.channel, (int) (holo_state->green * c));
            ledc_set_duty(ledc_blue_channel.speed_mode, ledc_blue_channel.channel, (int) (holo_state->blue * c));

            ledc_update_duty(ledc_red_channel.speed_mode, ledc_red_channel.channel);
            ledc_update_duty(ledc_green_channel.speed_mode, ledc_green_channel.channel);
            ledc_update_duty(ledc_blue_channel.speed_mode, ledc_blue_channel.channel);
        }
    }

    return HOLO_OK;
}

holo_err_t holo_deserialize(holo_handle_t *holo_handle, char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return HOLO_DESERIALIZE_ERROR;
    }

    cJSON *app = cJSON_GetObjectItem(root, "application");
    cJSON *version = cJSON_GetObjectItem(root, "version");

    if (holo_handle->version && version) {
        if (!strcmp(holo_handle->version, version->valuestring)) {
            ESP_LOGI(LOG_TAG, "Version %s in memory", version->valuestring);
            return HOLO_DESERIALIZE_REJECT;
        }
    }

    if (app && version) {
        holo_handle->version = version->valuestring;
        ESP_LOGI(LOG_TAG, "Deserialize application %s with version %s", app->valuestring, version->valuestring);
    }

    cJSON *effect;
    cJSON *effects = cJSON_GetObjectItem(root, "effects");

    if (!effects || !cJSON_IsArray(effects)) {
        return HOLO_DESERIALIZE_ERROR;
    }

    if (holo_handle->effects) {
        free(holo_handle->effects);
    }

    holo_handle->effects_length = cJSON_GetArraySize(effects);
    holo_handle->effects = malloc(holo_handle->effects_length * sizeof(holo_effect_t));


    int effects_index = 0;
    int states_index = 0;

    cJSON_ArrayForEach(effect, effects) {
        holo_effect_t holo_effect = {};

        cJSON *state;
        cJSON *states = cJSON_GetObjectItem(effect, "states");
        cJSON *name = cJSON_GetObjectItem(effect, "name");
        cJSON *bits = cJSON_GetObjectItem(effect, "bits");

        if (name && cJSON_IsString(name)) {
            holo_effect.name = name->valuestring;
        }

        if (bits && cJSON_IsNumber(bits)) {
            holo_effect.bits = bits->valueint;
        }

        if (!states || !cJSON_IsArray(states)) {
            ESP_LOGE(LOG_TAG, "Effect %s requires states array", holo_effect.name);
            continue;
        }

        holo_effect.states_length = cJSON_GetArraySize(states);
        holo_effect.states = malloc(holo_effect.states_length * sizeof(holo_state_t));

        ESP_LOGI(LOG_TAG, "Process effect %s bits 0x%08x state length %d", holo_effect.name, holo_effect.bits,
                 holo_effect.states_length);

        states_index = 0;
        cJSON_ArrayForEach(state, states) {
            holo_state_t holo_state = {};

            cJSON *red = cJSON_GetObjectItem(state, "red");
            cJSON *green = cJSON_GetObjectItem(state, "green");
            cJSON *blue = cJSON_GetObjectItem(state, "blue");
            cJSON *brightness = cJSON_GetObjectItem(state, "brightness");
            cJSON *fade = cJSON_GetObjectItem(state, "fade");
            cJSON *delay = cJSON_GetObjectItem(state, "delay");

            holo_state.bits = holo_effect.bits;

            if (red && cJSON_IsNumber(red)) {
                holo_state.red = red->valueint;
            }

            if (green && cJSON_IsNumber(green)) {
                holo_state.green = green->valueint;
            }

            if (blue && cJSON_IsNumber(blue)) {
                holo_state.blue = blue->valueint;
            }

            if (brightness && cJSON_IsNumber(brightness)) {
                holo_state.brightness = brightness->valueint;
            }

            if (fade && cJSON_IsNumber(fade)) {
                holo_state.fade = fade->valueint;
            }
            if (delay && cJSON_IsNumber(delay)) {
                holo_state.delay = delay->valueint;
            }

            ESP_LOGI(LOG_TAG, "Process state RGB (%d, %d, %d) brightness %d fade %d delay %d", holo_state.red,
                     holo_state.green, holo_state.blue, holo_state.brightness, holo_state.fade, holo_state.delay);

            holo_effect.states[states_index] = holo_state;
            states_index++;
        }

        holo_handle->effects[effects_index] = holo_effect;
        effects_index++;
    }

    cJSON_free(root);
    return HOLO_OK;
}