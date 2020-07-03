typedef enum {
    HOLO_OK,
    HOLO_DESERIALIZE_ERROR,
    HOLO_DESERIALIZE_REJECT
} holo_err_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t brightness;
    uint32_t fade;
    uint32_t delay;
} holo_state_t;

typedef struct {
    char *name;
    uint32_t bits;
    holo_state_t *states;
    size_t states_length;
} holo_effect_t;

typedef struct {
    char *version;
    char *namespace;
    char *key;
    bool power;
    holo_effect_t *effects;
    size_t effects_length;
    size_t eid;
    size_t sid;
    gpio_num_t red_pin;
    gpio_num_t green_pin;
    gpio_num_t blue_pin;
} holo_handle_t;

holo_err_t holo_init(holo_handle_t *holo_handle);

holo_err_t holo_load_default_effects(holo_handle_t *holo_handle);

holo_err_t holo_config(holo_handle_t *holo_handle);

holo_err_t holo_load(holo_handle_t *holo_handle, const char *default_effects);

holo_err_t holo_save(holo_handle_t *holo_handle, char *default_effects);

void holo_action(holo_handle_t *holo_handle, int event);

void holo_state_increment(holo_handle_t *holo_handle);

holo_state_t *holo_get_state(holo_handle_t *holo_handle);

holo_err_t holo_state_apply(holo_handle_t *holo_handle, holo_state_t *holo_state);

holo_err_t holo_deserialize(holo_handle_t *holo_handle, char *json);