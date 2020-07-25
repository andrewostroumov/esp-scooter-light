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
#include "light.h"
#include "http.h"

#define LOG_TAG  "root"

#define GPIO_LIGHT_IO       GPIO_NUM_21
#define GPIO_HOLO_RED_IO    GPIO_NUM_18
#define GPIO_HOLO_GREEN_IO  GPIO_NUM_19
#define GPIO_HOLO_BLUE_IO   GPIO_NUM_17


#define GPIO_LIGHT_INPUT_IO  GPIO_NUM_22
#define GPIO_HOLO_INPUT_IO  GPIO_NUM_23
#define GPIO_INPUT_SEL (1ULL << GPIO_LIGHT_INPUT_IO) | (1ULL << GPIO_HOLO_INPUT_IO)

#define INPUT_DELAY 10
#define DEBOUNCE_DELAY 50
#define INPUT_THRESHOLD 1000
#define WIFI_RECONNECT_DELAY 60000
#define EVENT_QUEUE_SIZE 32

typedef enum {
    EVENT_CLICK,
    EVENT_LONG_CLICK
} esp_event_t;

typedef struct {
    EventGroupHandle_t event_group;
    QueueHandle_t queue;
} event_queue_t;

typedef struct {
    EventGroupHandle_t event_group;
    EventGroupHandle_t cancel_group;
    QueueHandle_t queue;
} cancel_event_queue_t;

extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_cert_pem_end[] asm("_binary_ca_crt_end");

extern const uint8_t default_effects_start[] asm("_binary_effects_json_start");
extern const uint8_t default_effects_end[] asm("_binary_effects_json_start");

static QueueHandle_t esp_light_queue, esp_holo_queue;

static EventGroupHandle_t wifi_event_group, http_event_group;
static EventGroupHandle_t esp_light_input, esp_light_cancel, esp_light_live;
static EventGroupHandle_t esp_holo_input, esp_holo_cancel, esp_holo_live;

static cancel_event_queue_t light_input_receive_cancel_event_queue;
static cancel_event_queue_t holo_input_receive_cancel_event_queue;
static cancel_event_queue_t light_cancel_receive_cancel_event_queue;
static cancel_event_queue_t holo_cancel_receive_cancel_event_queue;
static event_queue_t light_event_receive_event_queue;
static event_queue_t holo_event_receive_event_queue;

static const int CONNECTED_BIT = BIT0;
static const int CANCEL_BIT = BIT1;
static const int RECONNECT_BIT = BIT1;

light_handle_t light_handle = {
        .delay = 50,
        .pin = GPIO_LIGHT_IO,
};

holo_handle_t holo_handle = {
        .namespace = "store",
        .key = "holo",
        .red_pin = GPIO_HOLO_RED_IO,
        .green_pin = GPIO_HOLO_GREEN_IO,
        .blue_pin = GPIO_HOLO_BLUE_IO,
};

http_handle_t http_handle = {
        .proto    = CONFIG_HTTP_PROTO,
        .host     = CONFIG_HTTP_HOST,
        .port     = CONFIG_HTTP_PORT,
        .username = CONFIG_HTTP_USERNAME,
        .password = CONFIG_HTTP_PASSWORD,
        .cert = (char *) ca_cert_pem_start,
};

static void IRAM_ATTR gpio_input_light_handler(void *args) {
    EventGroupHandle_t event_group = (EventGroupHandle_t) args;
    xEventGroupSetBitsFromISR(event_group, CONNECTED_BIT, NULL);
}

static void IRAM_ATTR gpio_input_holo_handler(void *args) {
    EventGroupHandle_t event_group = (EventGroupHandle_t) args;
    xEventGroupSetBitsFromISR(event_group, CONNECTED_BIT, NULL);
}

void task_input_receive(void *args) {
    bool pressed = false;
    TickType_t last_tick = 0;
    TickType_t tick = 0;
    TickType_t tick_shift = 0;
    esp_event_t esp_event = EVENT_CLICK;
    cancel_event_queue_t *cancel_event_queue = (cancel_event_queue_t *) args;

    while (true) {
        xEventGroupWaitBits(cancel_event_queue->event_group, CONNECTED_BIT, true, true, portMAX_DELAY);
        ESP_LOGI(LOG_TAG, "Receive with %p", cancel_event_queue);

        tick = xTaskGetTickCount();
        tick_shift = (tick - last_tick) * portTICK_RATE_MS;

        if (last_tick && tick_shift < DEBOUNCE_DELAY) {
            ESP_LOGI(LOG_TAG, "Debounce - shift %d with %p", tick_shift, cancel_event_queue);
            continue;
        }

        if (!pressed || tick_shift >= INPUT_THRESHOLD * 5) {
            ESP_LOGI(LOG_TAG, "Click fall with %p", cancel_event_queue);
            pressed = true;
            last_tick = tick;
            xEventGroupSetBits(cancel_event_queue->cancel_group, CONNECTED_BIT);
            continue;
        }

        ESP_LOGI(LOG_TAG, "Click raise with %p", cancel_event_queue);
        last_tick = tick;
        pressed = false;

        if (tick_shift >= INPUT_THRESHOLD) {
            ESP_LOGI(LOG_TAG, "Shutdown - shift %d with %p", tick_shift, cancel_event_queue);
            continue;
        }

        xEventGroupSetBits(cancel_event_queue->cancel_group, CANCEL_BIT);
        ESP_LOGI(LOG_TAG, "Send with %p", cancel_event_queue);
        xQueueSend(cancel_event_queue->queue, &esp_event, portMAX_DELAY);
    }
}

void task_cancel_receive(void *args) {
    TickType_t last_tick = 0;
    TickType_t tick = 0;
    TickType_t tick_shift = 0;
    EventBits_t xEventBits;
    esp_event_t esp_event = EVENT_LONG_CLICK;
    cancel_event_queue_t *cancel_event_queue = (cancel_event_queue_t *) args;

    while (true) {
        xEventGroupWaitBits(cancel_event_queue->cancel_group, CONNECTED_BIT, true, true, portMAX_DELAY);
        last_tick = xTaskGetTickCount();

        while (true) {
            vTaskDelay(INPUT_DELAY / portTICK_RATE_MS);

            tick = xTaskGetTickCount();
            tick_shift = (tick - last_tick) * portTICK_RATE_MS;

            if (tick_shift >= INPUT_THRESHOLD) {
                xQueueSend(cancel_event_queue->queue, &esp_event, portMAX_DELAY);
                break;
            }

            xEventBits = xEventGroupGetBits(cancel_event_queue->cancel_group);

            if (xEventBits == (EventBits_t) CANCEL_BIT) {
                break;
            }
        }

        xEventGroupClearBits(cancel_event_queue->cancel_group, CANCEL_BIT);
    }
}

void task_light_event_receive(void *args) {
    event_queue_t *event_queue = (event_queue_t *) args;
    esp_event_t esp_event;

    while (true) {
        xQueueReceive(event_queue->queue, &esp_event, portMAX_DELAY);
        light_action(&light_handle, esp_event);
        xEventGroupSetBits(event_queue->event_group, CONNECTED_BIT);
    }
}

void task_holo_event_receive(void *args) {
    event_queue_t *event_queue = (event_queue_t *) args;
    esp_event_t esp_event;

    while (true) {
        xQueueReceive(event_queue->queue, &esp_event, portMAX_DELAY);
        holo_action(&holo_handle, esp_event);
        xEventGroupSetBits(event_queue->event_group, CONNECTED_BIT);
    }
}

void task_light_live(void *args) {
    TickType_t last_tick = 0;
    TickType_t tick = 0;
    TickType_t tick_shift = 0;
    EventGroupHandle_t event_group = (EventGroupHandle_t) args;

    while (true) {
        light_live:
        xEventGroupWaitBits(event_group, CONNECTED_BIT, true, true, portMAX_DELAY);

        if (light_handle.mood != LIGHT_MOOD_BLINK) {
            light_apply(&light_handle);
            continue;
        }

        while (true) {
            light_apply(&light_handle);
            light_blink_action(&light_handle);

            last_tick = xTaskGetTickCount();

            while (true) {
                vTaskDelay(INPUT_DELAY / portTICK_RATE_MS);

                tick = xTaskGetTickCount();
                tick_shift = (tick - last_tick) * portTICK_RATE_MS;

                if (tick_shift >= light_handle.delay) {
                    break;
                }

                if (light_handle.mood != LIGHT_MOOD_BLINK || !light_handle.power) {
                    goto light_live;
                }
            }
        }
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
            if (!holo_handle.power) {
                holo_state_apply(&holo_handle, NULL);
                break;
            }

            holo_state = holo_get_state(&holo_handle);
            last_eid = holo_handle.eid;
            holo_state_apply(&holo_handle, holo_state);
            holo_state_increment(&holo_handle);
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

            holo_free_state(&holo_handle, holo_state);
        }
    }
}

void task_http_poll(void *args) {
    holo_err_t err;
    EventGroupHandle_t event_group = (EventGroupHandle_t) args;

    while (true) {
        http_poll:
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
        xEventGroupWaitBits(event_group, CONNECTED_BIT, true, true, portMAX_DELAY);

        while (true) {
            http_response_t http_response = {};
            http_get_holo(&http_handle, &http_response);
            if (!http_response.data) { continue; }

            ESP_LOGI(LOG_TAG, "Response (%d) %s", http_response.data_len, http_response.data);

            err = holo_deserialize(&holo_handle, (char *) http_response.data);
            if (!err) {
                holo_save(&holo_handle, (char *) http_response.data);
            }

            http_response_cleanup(&http_handle, &http_response);
            esp_wifi_disconnect();

            goto http_poll;
        }
    }
}

void task_wifi_start(void *args) {
    EventGroupHandle_t event_group = (EventGroupHandle_t) args;
    while(true) {
        xEventGroupWaitBits(event_group, RECONNECT_BIT, true, true, portMAX_DELAY);
        vTaskDelay(WIFI_RECONNECT_DELAY / portTICK_PERIOD_MS);
        ESP_LOGI(LOG_TAG, "[WIFI] Reconnecting to %s...", CONFIG_WIFI_SSID);
        esp_wifi_start();
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
            ESP_LOGI(LOG_TAG, "[WIFI] Disconnected");
            esp_wifi_stop();
            break;
        case WIFI_EVENT_STA_STOP:
            ESP_LOGI(LOG_TAG, "[WIFI] Stopped");
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            xEventGroupSetBits(wifi_event_group, RECONNECT_BIT);
            break;
        default:
            ESP_LOGI(LOG_TAG, "[WIFI] Event base %s with ID %d", base, id);
            break;
    }
}

void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(LOG_TAG, "[IP] Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        xEventGroupSetBits(http_event_group, CONNECTED_BIT);
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

    esp_light_input = xEventGroupCreate();
    esp_light_cancel = xEventGroupCreate();
    esp_light_live = xEventGroupCreate();

    esp_holo_input = xEventGroupCreate();
    esp_holo_cancel = xEventGroupCreate();
    esp_holo_live = xEventGroupCreate();

    http_event_group = xEventGroupCreate();

    esp_light_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(esp_event_t));
    esp_holo_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(esp_event_t));

    if (!esp_light_queue || !esp_holo_queue) {
        ESP_LOGE(LOG_TAG, "Error initialize event queues");
        exit(ESP_FAIL);
    }

    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.pin_bit_mask = GPIO_INPUT_SEL;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_LIGHT_INPUT_IO, gpio_input_light_handler, (void *) esp_light_input);
    gpio_isr_handler_add(GPIO_HOLO_INPUT_IO, gpio_input_holo_handler, (void *) esp_holo_input);

    wifi_init_sta();

    holo_err_t holo_err = holo_init(&holo_handle);
    if (holo_err) {
        ESP_LOGE(LOG_TAG, "Holo initialize error %d", holo_err);
    }

    esp_err_t esp_err = light_init(&light_handle);
    if (esp_err) {
        ESP_LOGE(LOG_TAG, "Light initialize error %d", esp_err);
    }

    http_init(&http_handle);
    holo_load(&holo_handle, (const char *) default_effects_start);

    light_input_receive_cancel_event_queue = (cancel_event_queue_t) {
            .event_group = esp_light_input,
            .cancel_group = esp_light_cancel,
            .queue = esp_light_queue
    };

    holo_input_receive_cancel_event_queue = (cancel_event_queue_t) {
            .event_group = esp_holo_input,
            .cancel_group = esp_holo_cancel,
            .queue = esp_holo_queue
    };

    light_cancel_receive_cancel_event_queue = (cancel_event_queue_t) {
            .cancel_group = esp_light_cancel,
            .queue = esp_light_queue
    };

    holo_cancel_receive_cancel_event_queue = (cancel_event_queue_t) {
            .cancel_group = esp_holo_cancel,
            .queue = esp_holo_queue
    };

    light_event_receive_event_queue = (event_queue_t) {
            .event_group = esp_light_live,
            .queue = esp_light_queue
    };

    holo_event_receive_event_queue = (event_queue_t) {
            .event_group = esp_holo_live,
            .queue = esp_holo_queue
    };

    ESP_LOGI(LOG_TAG, "Light pointer %p", &light_input_receive_cancel_event_queue);
    ESP_LOGI(LOG_TAG, "Holo pointer %p", &holo_input_receive_cancel_event_queue);

    xTaskCreate(task_input_receive, "task_light_input_receive", 4096, (void *) &light_input_receive_cancel_event_queue, 0, NULL);
    xTaskCreate(task_input_receive, "task_holo_input_receive", 4096, (void *) &holo_input_receive_cancel_event_queue, 0, NULL);

    xTaskCreate(task_cancel_receive, "task_light_cancel_receive", 4096, (void *) &light_cancel_receive_cancel_event_queue, 0, NULL);
    xTaskCreate(task_cancel_receive, "task_holo_cancel_receive", 4096, (void *) &holo_cancel_receive_cancel_event_queue, 0, NULL);

    xTaskCreate(task_light_event_receive, "task_light_event_receive", 4096, (void *) &light_event_receive_event_queue, 0, NULL);
    xTaskCreate(task_holo_event_receive, "task_holo_event_receive", 4096, (void *) &holo_event_receive_event_queue, 0, NULL);

    xTaskCreate(task_light_live, "task_light_live", 4096, (void *) esp_light_live, 0, NULL);
    xTaskCreate(task_holo_live, "task_holo_live", 4096, (void *) esp_holo_live, 0, NULL);
    xTaskCreate(task_http_poll, "task_http_poll", 4096, (void *) http_event_group, 0, NULL);
    xTaskCreate(task_wifi_start, "task_wifi_start", 4096, (void *) wifi_event_group, 0, NULL);
}
