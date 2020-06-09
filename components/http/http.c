#include <esp_err.h>
#include <esp_log.h>
#include <string.h>
#include "esp_tls.h"
#include "esp_http_client.h"
#include "http.h"

#define TAG "http"

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    static int data_len = 0;

    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (evt->user_data) {
                    http_response_t *http_response = (http_response_t *) evt->user_data;
                    if (!http_response->data) {
                        http_response->data_len = esp_http_client_get_content_length(evt->client) + 1;
                        http_response->data = malloc(http_response->data_len);
                    }

                    memcpy(http_response->data + data_len, evt->data, evt->data_len);
                    data_len += evt->data_len;
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            data_len = 0;

            if (evt->user_data) {
                http_response_t *http_response = (http_response_t *) evt->user_data;
                *(http_response->data + http_response->data_len) = 0;
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            break;
    }
    return ESP_OK;
}

esp_err_t http_init(http_handle_t *http_handle) {
    return ESP_OK;
}

void http_get_holo(http_handle_t *http_handle, http_response_t *http_response) {
    esp_http_client_handle_t client = http_do(http_handle, HTTP_METHOD_GET, "/espressif/holo.json", http_response);
    if (!client) { return; }
    esp_http_client_cleanup(client);
}

esp_http_client_handle_t
http_do(http_handle_t *http_handle, esp_http_client_method_t method, const char *path, http_response_t *http_response) {
    esp_http_client_config_t config = {
            .host = http_handle->host,
            .port = http_handle->port,
            .username = http_handle->username,
            .password = http_handle->password,
            .auth_type = HTTP_AUTH_TYPE_BASIC,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .method = method,
            .path = path,
            .event_handler = http_event_handler,
            .user_data = (void *) http_response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP status = %d content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
        return client;
    } else {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
        return NULL;
    }
}

esp_err_t http_response_cleanup(http_handle_t *http_handle, http_response_t *http_response) {
    if (!http_response) {
        return ESP_FAIL;
    }

    if (http_response->data) {
        free(http_response->data);
    }

    return ESP_OK;
}