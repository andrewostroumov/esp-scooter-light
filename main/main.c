#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include <driver/ledc.h>
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
#include <esp_tls.h>

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"

#include "esp_http_client.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include "math.h"
#include "holo.h"
#include "http.h"

#define LOG_TAG        "light"

#define GPIO_WHITE_IO  GPIO_NUM_22
#define GPIO_RED_IO    GPIO_NUM_17
#define GPIO_GREEN_IO  GPIO_NUM_18
#define GPIO_BLUE_IO   GPIO_NUM_19


#define GPIO_INPUT_IO  14
#define GPIO_INPUT_SEL 1ULL << GPIO_INPUT_IO

#define LED_USE_FADE
#define LED_FADE_MS     500
#define LED_MAX_DUTY    256

#define INPUT_DELAY     50
#define INPUT_THRESHOLD 1000

#define EVENT_QUEUE_SIZE 32

typedef enum {
    EVENT_CLICK,
    EVENT_LONG_CLICK
} esp_event_t;

typedef struct {
    bool power;
    int led;
} esp_state_t;

extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_cert_pem_end[] asm("_binary_ca_crt_end");

extern const uint8_t default_effects_start[] asm("_binary_effects_json_start");
extern const uint8_t default_effects_end[] asm("_binary_effects_json_start");

static QueueHandle_t esp_light_queue, esp_holo_queue;
static EventGroupHandle_t esp_input, esp_shutdown, esp_light_live, esp_holo_live, wifi_event_group, http_event_group;

static const int CONNECTED_BIT = BIT0;
//static const int SHUTDOWN_BIT = BIT0;
//static const int CANCEL_BIT = BIT1;


//esp_state_t esp_state = {
//        .power = false,
//        .led   = 0,
//};

holo_handle_t holo_handle = {
        .namespace = "store",
        .key = "holo",
        .red_pin = GPIO_RED_IO,
        .green_pin = GPIO_GREEN_IO,
        .blue_pin = GPIO_BLUE_IO,
};

http_handle_t http_handle = {
        .proto    = CONFIG_HTTP_PROTO,
        .host     = CONFIG_HTTP_HOST,
        .port     = CONFIG_HTTP_PORT,
        .username = CONFIG_HTTP_USERNAME,
        .password = CONFIG_HTTP_PASSWORD,
        .cert = (char *) ca_cert_pem_start,
};
//
//static void IRAM_ATTR gpio_input_handler(void *args) {
//    EventGroupHandle_t event_group = (EventGroupHandle_t) args;
//    xEventGroupSetBitsFromISR(event_group, CONNECTED_BIT, NULL);
//}

//void update_state(esp_state_t *local_state, esp_event_t event) {
//    switch (event) {
//        case EVENT_CLICK:
//            if (!local_state->power) {
//                local_state->power = true;
//                local_state->led = LED_MAX_DUTY / 2;
//                break;
//            }
//
//            if (local_state->led + LED_MAX_DUTY / 4 > LED_MAX_DUTY) {
//                local_state->led = LED_MAX_DUTY / 4;
//            } else {
//                local_state->led += LED_MAX_DUTY / 4;
//            }
//
//            break;
//        case EVENT_LONG_CLICK:
//            local_state->power = false;
//            local_state->led = 0;
//            break;
//    }
//}

//void apply_state(esp_state_t *local_state) {
//    if (!local_state->power) {
//
//#ifdef LED_USE_FADE
//        ledc_set_fade_with_time(ledc_led_channel.speed_mode, ledc_led_channel.channel, 0, LED_FADE_MS);
//        ledc_fade_start(ledc_led_channel.speed_mode, ledc_led_channel.channel, LEDC_FADE_NO_WAIT);
//#else
//        ledc_set_duty(ledc_led_channel.speed_mode, ledc_led_channel.channel, 0);
//        ledc_update_duty(ledc_led_channel.speed_mode, ledc_led_channel.channel);
//#endif
//
//        vTaskDelay(LED_FADE_MS / portTICK_PERIOD_MS);
//        return;
//    }
//
//#ifdef LED_USE_FADE
//    ledc_set_fade_with_time(ledc_led_channel.speed_mode, ledc_led_channel.channel, local_state->led, LED_FADE_MS);
//    ledc_fade_start(ledc_led_channel.speed_mode, ledc_led_channel.channel, LEDC_FADE_NO_WAIT);
//#else
//    ledc_set_duty(ledc_led_channel.speed_mode, ledc_led_channel.channel, local_state->led);
//    ledc_update_duty(ledc_led_channel.speed_mode, ledc_led_channel.channel);
//#endif
//
//    vTaskDelay(LED_FADE_MS / portTICK_PERIOD_MS);
//}

//void task_input_receive(void *param) {
//    bool pressed = false;
//    TickType_t last_tick = 0;
//    TickType_t tick = 0;
//    TickType_t tick_shift = 0;
//    esp_event_t esp_event = EVENT_CLICK;
//
//    while (true) {
//        xEventGroupWaitBits(esp_input_group, CONNECTED_BIT, true, true, portMAX_DELAY);
//        tick = xTaskGetTickCount();
//
//        if (!last_tick) {
//            last_tick = tick;
//            pressed = true;
//            xEventGroupSetBits(esp_shutdown, SHUTDOWN_BIT);
//            continue;
//        }
//
//        tick_shift = (tick - last_tick) * portTICK_RATE_MS;
//
//        if (tick_shift < INPUT_DELAY) {
//            continue;
//        }
//
//        if (!pressed || tick_shift >= INPUT_THRESHOLD * 5) {
//            last_tick = tick;
//            pressed = true;
//            xEventGroupSetBits(esp_shutdown, SHUTDOWN_BIT);
//            continue;
//        }
//
//        last_tick = tick;
//        pressed = false;
//
//        if (tick_shift >= INPUT_THRESHOLD) {
//            continue;
//        }
//
//        xEventGroupSetBits(esp_shutdown, CANCEL_BIT);
//        xQueueSend(esp_event_queue, &esp_event, portMAX_DELAY);
//    }
//}

//void task_shutdown(void *param) {
//    TickType_t last_tick = 0;
//    TickType_t tick = 0;
//    TickType_t tick_shift = 0;
//    EventBits_t xEventBits;
//    esp_event_t esp_event = EVENT_LONG_CLICK;
//
//    while (true) {
//        xEventGroupWaitBits(esp_shutdown, SHUTDOWN_BIT, true, true, portMAX_DELAY);
//        last_tick = xTaskGetTickCount();
//
//        while (true) {
//            vTaskDelay(INPUT_DELAY / portTICK_RATE_MS);
//
//            tick = xTaskGetTickCount();
//            tick_shift = (tick - last_tick) * portTICK_RATE_MS;
//
//            if (tick_shift >= INPUT_THRESHOLD) {
//                xQueueSend(esp_event_queue, &esp_event, portMAX_DELAY);
//                break;
//            }
//
//            xEventBits = xEventGroupGetBits(esp_shutdown);
//
//            if (xEventBits == (EventBits_t) CANCEL_BIT) {
//                break;
//            }
//        }
//
//        xEventGroupClearBits(esp_shutdown, CANCEL_BIT);
//    }
//}

//void task_event_light_receive(void *args) {
//    QueueHandle_t queue = (QueueHandle_t) args;
//    esp_event_t esp_event;
//
//    while (true) {
//        xQueueReceive(queue, &esp_event, portMAX_DELAY);
//        update_state(&esp_state, esp_event);
//        apply_state(&esp_state);
//    }
//}

// TODO: push from handler to this queue
// TODO: run this loop
void task_event_led_receive(void *args) {
    QueueHandle_t queue = (QueueHandle_t) args;
    esp_event_t esp_event;

    while (true) {
        xQueueReceive(queue, &esp_event, portMAX_DELAY);
        holo_action(&holo_handle, esp_event);
        xEventGroupSetBits(esp_holo_live, CONNECTED_BIT);
    }
}

void task_holo_live(void *args) {
    TickType_t last_tick = 0;
    TickType_t tick = 0;
    TickType_t tick_shift = 0;
    holo_state_t *holo_state;
    size_t last_eid;
    EventGroupHandle_t event_group = (EventGroupHandle_t) args;

    while (true) {
        xEventGroupWaitBits(event_group, CONNECTED_BIT, true, true, portMAX_DELAY);

        while (true) {
            if (!holo_handle.power) { break; }

            holo_state = holo_get_state(&holo_handle);
            last_eid = holo_handle.eid;
            ESP_LOGI(LOG_TAG, "Got state %d %d %d %d %d", holo_state->red, holo_state->green, holo_state->blue,
                     holo_state->fade, holo_state->delay);

            holo_state_apply(&holo_handle, holo_state);
            holo_state_increment(&holo_handle);
            ESP_LOGI(LOG_TAG, "Loop: eid %d sid %d", holo_handle.eid, holo_handle.sid);

            last_tick = xTaskGetTickCount();

            while (true) {
                vTaskDelay(INPUT_DELAY / portTICK_RATE_MS);

                tick = xTaskGetTickCount();
                tick_shift = (tick - last_tick) * portTICK_RATE_MS;

                if (tick_shift >= holo_state->delay) {
                    break;
                }

                if (last_eid != holo_handle.eid || !holo_handle.power) {
                    break;
                }
            }
        }
    }
}

void task_http_poll(void *args) {
    EventGroupHandle_t event_group = (EventGroupHandle_t) args;

    while (true) {
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
        xEventGroupWaitBits(event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

        http_response_t http_response = {};
        http_get_holo(&http_handle, &http_response);
        if (!http_response.data) { continue; }

        ESP_LOGI(LOG_TAG, "Response (%d) %s", http_response.data_len, http_response.data);

        holo_deserialize(&holo_handle, (char *) http_response.data);
        http_response_cleanup(&http_handle, &http_response);
        vTaskDelay(30000 / portTICK_PERIOD_MS);
    }
}


void wifi_event_handler(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data) {
    switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(LOG_TAG, "[WIFI] Connecting to %s...", CONFIG_WIFI_SSID);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(LOG_TAG, "[WIFI] Connected");
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(LOG_TAG, "[WIFI] Reconnecting to %s...", CONFIG_WIFI_SSID);
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;

        default:
            ESP_LOGI(LOG_TAG, "[WIFI] Event base %s with ID %d", base, id);
            break;
    }
}

void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(LOG_TAG, "[IP] Got IP:"
                IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    } else {
        ESP_LOGI(LOG_TAG, "[IP] Event base %s with ID %d", base, id);
    }
}

void wifi_init_sta() {
    wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL));

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {
            .sta = {
                    .ssid = CONFIG_WIFI_SSID,
                    .password = CONFIG_WIFI_PASSWORD,
            }
    };

    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {

    ESP_LOGI(LOG_TAG, "[APP] Startup..");
    ESP_LOGI(LOG_TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(LOG_TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("esp-tls", ESP_LOG_INFO);
    esp_log_level_set(LOG_TAG, ESP_LOG_INFO);


    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());


//    esp_input_group = xEventGroupCreate();
//    esp_shutdown = xEventGroupCreate();
//
//    esp_event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(esp_event_t));
//
//    if (esp_event_queue == NULL) {
//        ESP_LOGE(LOG_TAG, "Error initialize event queue");
//        exit(ESP_FAIL);
//    }

    holo_err_t holo_err = holo_init(&holo_handle);
    if (holo_err) {
        ESP_LOGE(LOG_TAG, "Holo initialize error %d", holo_err);
    }

    http_init(&http_handle);


//    gpio_config_t io_conf;
//    io_conf.intr_type = GPIO_INTR_ANYEDGE;
//    io_conf.pin_bit_mask = GPIO_INPUT_SEL;
//    io_conf.mode = GPIO_MODE_INPUT;
//    io_conf.pull_down_en = 1;
//    gpio_config(&io_conf);
//
//    gpio_install_isr_service(0);
//    gpio_isr_handler_add(GPIO_INPUT_IO, gpio_input_handler, (void *) esp_input_group);
//
//    xTaskCreate(task_input_receive, "task_input_receive", 4096, NULL, 0, NULL);
//    xTaskCreate(task_event_light_receive, "task_event_light_receive", 4096, (void *) esp_event_light_queue, 0, NULL);
//    xTaskCreate(task_event_led_receive, "task_event_led_receive", 4096, (void *) esp_event_led_queue, 0, NULL);
//    xTaskCreate(task_shutdown, "task_shutdown", 4096, NULL, 0, NULL);


    wifi_init_sta();

    esp_holo_live = xEventGroupCreate();
    http_event_group = xEventGroupCreate();

    // TODO: whenever we success deserialize json - persist it (holo_save)
    holo_load(&holo_handle, (const char *) default_effects_start);

    xEventGroupSetBits(http_event_group, CONNECTED_BIT);

    xTaskCreate(task_holo_live, "task_holo_live", 4096, (void *) esp_holo_live, 0, NULL);
    xTaskCreate(task_http_poll, "task_http_poll", 4096, (void *) http_event_group, 0, NULL);

//    int i = 0;
//    while (true) {
//        i++;
//
//        if (i % 3 == 0) {
//            holo_action(&holo_handle, EVENT_LONG_CLICK);
//        } else {
//            holo_action(&holo_handle, EVENT_CLICK);
//        }
//
//        xEventGroupSetBits(esp_holo_live, CONNECTED_BIT);
//        ESP_LOGI(LOG_TAG, "Click: power %d eid %d sid %d", holo_handle.power, holo_handle.eid, holo_handle.sid);
//        vTaskDelay(15000 / portTICK_PERIOD_MS);
//    }
}
