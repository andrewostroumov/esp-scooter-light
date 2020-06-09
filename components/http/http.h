#include "esp_err.h"

typedef struct {
    char *proto;
    char *host;
    int  port;
    char *username;
    char *password;
    char *cert;
} http_handle_t;

typedef struct {
    uint8_t *data;
    int data_len;
} http_response_t;

esp_err_t http_init(http_handle_t *http_handle);

void http_get_holo(http_handle_t *http_handle, http_response_t *http_response);

esp_http_client_handle_t
http_do(http_handle_t *http_handle, esp_http_client_method_t method, const char *path, http_response_t *http_response);

esp_err_t http_response_cleanup(http_handle_t *http_handle, http_response_t *http_response);
