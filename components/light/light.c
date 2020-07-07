#include <driver/ledc.h>
#include "freertos/FreeRTOS.h"
#include <freertos/task.h>
#include <esp_log.h>
#include "light.h"

#define MAX_DUTY 256
#define LEDC_FREQ_HZ 5000
#define DEFAULT_FADE_MS 500
#define LOG_TAG "light"

static ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = LEDC_FREQ_HZ,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .clk_cfg = LEDC_AUTO_CLK,
};

ledc_channel_config_t ledc_channel = {
        .channel    = LEDC_CHANNEL_0,
        .duty       = 0,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .hpoint     = 0,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_FADE_END
};

esp_err_t light_init(light_handle_t *light_handle) {
    light_handle->power = false;
    light_config(light_handle);
    return ESP_OK;
}

esp_err_t light_config(light_handle_t *light_handle) {
    ledc_channel.gpio_num = light_handle->pin;

    ledc_timer_config(&ledc_timer);
    ledc_channel_config(&ledc_channel);
    ledc_fade_func_install(0);
    return ESP_OK;
}

void light_action(light_handle_t *light_handle, int event) {
    if (!light_handle) {
        return;
    }

    if (event) {
        light_handle->power = false;
        return;
    }

    if (!light_handle->power) {
        light_handle->power = true;
        light_handle->mood = LIGHT_MOOD_LOW;
        return;
    }

    light_handle->mood = (light_handle->mood + 1) % (LIGHT_MOOD_BLINK + 1);

    if (light_handle->mood == LIGHT_MOOD_BLINK) {
        light_handle->blink = false;
    }
}

void light_blink_action(light_handle_t *light_handle) {
    if (!light_handle) {
        return;
    }

    if (light_handle->mood != LIGHT_MOOD_BLINK) {
        return;
    }

    light_handle->blink = !light_handle->blink;
}

int light_brightness(light_handle_t *light_handle) {
    if (!light_handle) {
        return 0;
    }

    uint16_t bright;
    uint16_t x = MAX_DUTY / (LIGHT_MOOD_MAX + 1);
    bright = x * (light_handle->mood + 1);

    if (bright > MAX_DUTY) {
        return MAX_DUTY;
    }

    return bright;
}

esp_err_t light_apply(light_handle_t *light_handle) {
    if (!light_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!light_handle->power) {
        ledc_set_fade_with_time(ledc_channel.speed_mode, ledc_channel.channel, 0, DEFAULT_FADE_MS);
        ledc_fade_start(ledc_channel.speed_mode, ledc_channel.channel, LEDC_FADE_NO_WAIT);

        vTaskDelay(DEFAULT_FADE_MS / portTICK_PERIOD_MS);
        return ESP_OK;
    }

    int brightness = light_brightness(light_handle);

    if (light_handle->mood == LIGHT_MOOD_BLINK) {
        ledc_set_duty(ledc_channel.speed_mode, ledc_channel.channel, light_handle->blink * brightness);
        ledc_update_duty(ledc_channel.speed_mode, ledc_channel.channel);
    } else {
        ledc_set_fade_with_time(ledc_channel.speed_mode, ledc_channel.channel, brightness, DEFAULT_FADE_MS);
        ledc_fade_start(ledc_channel.speed_mode, ledc_channel.channel, LEDC_FADE_NO_WAIT);

        vTaskDelay(DEFAULT_FADE_MS / portTICK_PERIOD_MS);
    }

    return ESP_OK;
}
