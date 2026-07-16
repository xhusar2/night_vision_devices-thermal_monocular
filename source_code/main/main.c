#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "json_parser.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "Mini2.h"
#include "control_validation.h"

#define TAG "MAIN"

#define SSID "THERMAL_MONOCULAR"
#define PASSWORD "password123"
#define PRESET_COUNT 3
#define FIRMWARE_VERSION "0.4.10"
#define BUTTON_DEBOUNCE_US (50 * 1000)
#define BUTTON_LONG_PRESS_US (2 * 1000 * 1000)
#define PERSIST_ATTEMPTS 3

#define UART_TX GPIO_NUM_1
#define UART_RX GPIO_NUM_2
#define POTI_PIN GPIO_NUM_4
#define MULTI_BTN GPIO_NUM_8


extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_netif_t *sta_netif = NULL;

nvs_handle_t flash_handle;

int8_t brightness_button_count = 0;

typedef struct stored_values_t{
    uint8_t active_preset;
    alignment_preset_t alignment;
    value_preset_t presets[PRESET_COUNT];
    bool first_boot;
} stored_values_t;

value_preset_t default_presets[] = {
    {
        .preset_en = true,
        .pseudo_color = WHOT,
        .scene_mode = GeneralMode,
        .contrast = 100,
        .edge_enhancment_gear = 1,
        .detail_enhancement_gear = 55,
        .burn_protection_en = true,
        .auto_shutter_en = true,
    },
    {
        .preset_en = true,
        .pseudo_color = WHOT,
        .scene_mode = Outline,
        .contrast = 100,
        .edge_enhancment_gear = 1,
        .detail_enhancement_gear = 50,
        .burn_protection_en = true,
        .auto_shutter_en = true,
    }
};

value_preset_t base_preset = {
    .preset_en = false,
    .pseudo_color = WHOT,
    .scene_mode = GeneralMode,
    .contrast = 50,
    .edge_enhancment_gear = 1,
    .detail_enhancement_gear = 50,
    .burn_protection_en = true,
    .auto_shutter_en = true,
};

stored_values_t stored = {
    .active_preset = 0,
    .alignment = {
        .zoom = 10,
        .zoom_x = 128,
        .zoom_y = 96,
        .av_format = PAL,
        .flip_mode = No_Flip,
        .fps = Hz50,
        .refresh_flip_mode = false,
    },
    .first_boot = true
};

static uint8_t crosshair_preset_mask = 0;
typedef struct {
    uint16_t x[PRESET_COUNT];
    uint16_t y[PRESET_COUNT];
} crosshair_positions_t;
static crosshair_positions_t crosshair_positions = {
    .x = {128, 128, 128},
    .y = {96, 96, 96},
};
typedef struct {
    uint8_t spatial[PRESET_COUNT];
    uint8_t temporal[PRESET_COUNT];
    uint8_t gamma[PRESET_COUNT];
    uint8_t spatial_mask;
    uint8_t temporal_mask;
    uint8_t gamma_mask;
} image_controls_t;
static image_controls_t image_controls;
static bool wifi_next_boot = false;
static esp_err_t boot_analog_video_initial_status = ESP_ERR_INVALID_STATE;
static volatile uint8_t last_applied_preset;
static bool camera_state_authoritative = true;
static esp_err_t camera_state_status = ESP_OK;
static bool persistence_authoritative = true;
static esp_err_t persistence_status = ESP_OK;

static bool preset_crosshair_enabled(uint8_t preset) {
    return (crosshair_preset_mask & (1U << preset)) != 0;
}

static void set_preset_crosshair_enabled(uint8_t preset, bool enabled) {
    if (enabled) {
        crosshair_preset_mask |= (1U << preset);
    } else {
        crosshair_preset_mask &= ~(1U << preset);
    }
}

static esp_err_t commit_settings(void) {
    esp_err_t err = nvs_set_blob(flash_handle, "stored_values", &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_set_u8(flash_handle, "crosshair_mask", crosshair_preset_mask);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(flash_handle, "wifi_next", wifi_next_boot);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(flash_handle, "image_controls", &image_controls, sizeof(image_controls));
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(flash_handle, "cursor_pos", &crosshair_positions, sizeof(crosshair_positions));
    }
    return err == ESP_OK ? nvs_commit(flash_handle) : err;
}

static int persist_settings(void *context) {
    (void)context;
    return commit_settings();
}

static esp_err_t commit_settings_with_retry(void) {
    return control_retry_operation(NULL, persist_settings, PERSIST_ATTEMPTS);
}

static bool json_obj_has_key(const jparse_ctx_t *jctx, const char *key) {
    const int object_index = jctx->cur - jctx->tokens;
    const size_t key_length = strlen(key);

    for (int i = object_index + 1; i < jctx->num_tokens; i++) {
        const json_tok_t *token = &jctx->tokens[i];
        if (token->parent == object_index && token->type == JSMN_STRING &&
            (size_t)(token->end - token->start) == key_length &&
            strncmp(jctx->js + token->start, key, key_length) == 0) {
            return true;
        }
    }

    return false;
}

Mini2_t cam = {
    .uart_port = UART_NUM_1, // C3 only has num0 and num1, and num0 is used for debug / USB_CDC
    .uart_tx = UART_TX,
    .uart_rx = UART_RX,
    .variant = Mini2_256
};

static int apply_preset_step(void *context, uint8_t preset) {
    Mini2_t *camera = context;
    return Mini2_apply_preset(camera, &stored.presets[preset], &stored.alignment, true);
}

static int apply_crosshair_step(void *context, bool enabled) {
    return Mini2_set_crosshair(context, enabled);
}

static esp_err_t apply_preset_with_crosshair(uint8_t preset) {
    esp_err_t err = control_apply_preset_transaction(
        &cam, preset, preset_crosshair_enabled(preset),
        apply_preset_step, apply_crosshair_step);
    if (err != ESP_OK) {
        return err;
    }
    err = Mini2_set_crosshair_position(&cam, crosshair_positions.x[preset],
                                       crosshair_positions.y[preset]);
    if (image_controls.spatial_mask & (1U << preset))
        err = Mini2_set_spatial_noise_reduction(&cam, image_controls.spatial[preset]);
    if (err == ESP_OK && (image_controls.temporal_mask & (1U << preset)))
        err = Mini2_set_temporal_noise_reduction(&cam, image_controls.temporal[preset]);
    if (err == ESP_OK && (image_controls.gamma_mask & (1U << preset)))
        err = Mini2_set_gamma_intensity(&cam, image_controls.gamma[preset]);
    if (err != ESP_OK) return err;
    last_applied_preset = preset;
    return ESP_OK;
}

static esp_err_t restore_camera_snapshot(const stored_values_t *snapshot,
                                         uint8_t snapshot_crosshair_mask) {
    const uint8_t preset_index = snapshot->active_preset;
    const value_preset_t *preset = &snapshot->presets[preset_index];
    esp_err_t err = ESP_OK;

    err = control_first_error(err, Mini2_set_color_pallet(&cam, preset->pseudo_color));
    err = control_first_error(err, Mini2_set_scene_mode(&cam, preset->scene_mode));
    err = control_first_error(err, Mini2_set_contrast(&cam, preset->contrast));
    err = control_first_error(err, Mini2_set_edge_enhancment(&cam, preset->edge_enhancment_gear));
    err = control_first_error(err, Mini2_set_detail_enhancement(&cam, preset->detail_enhancement_gear));
    err = control_first_error(err, Mini2_set_burn_protection(&cam, preset->burn_protection_en));
    err = control_first_error(err, Mini2_set_auto_shutter(&cam, preset->auto_shutter_en));
    if (image_controls.spatial_mask & (1U << preset_index))
        err = control_first_error(err, Mini2_set_spatial_noise_reduction(&cam, image_controls.spatial[preset_index]));
    if (image_controls.temporal_mask & (1U << preset_index))
        err = control_first_error(err, Mini2_set_temporal_noise_reduction(&cam, image_controls.temporal[preset_index]));
    if (image_controls.gamma_mask & (1U << preset_index))
        err = control_first_error(err, Mini2_set_gamma_intensity(&cam, image_controls.gamma[preset_index]));
    err = control_first_error(err, Mini2_set_analog_video_format(&cam, snapshot->alignment.av_format));
    err = control_first_error(err, Mini2_set_flip_mode(&cam, snapshot->alignment.flip_mode));
    err = control_first_error(err, Mini2_set_point_zoom(
        &cam, snapshot->alignment.zoom_x, snapshot->alignment.zoom_y,
        snapshot->alignment.zoom));
    err = control_first_error(err, Mini2_set_crosshair(
        &cam, (snapshot_crosshair_mask & (1U << preset_index)) != 0));
    err = control_first_error(err, Mini2_set_crosshair_position(
        &cam, crosshair_positions.x[preset_index], crosshair_positions.y[preset_index]));

    if (err == ESP_OK) {
        last_applied_preset = preset_index;
    }
    return err;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}

static uint8_t next_preset(uint8_t current) {
    do {
        current = (current + 1) % PRESET_COUNT;
    } while (current != 0 && !stored.presets[current].preset_en);
    return current;
}

static esp_err_t switch_preset(uint8_t preset) {
    const stored_values_t previous = stored;
    const uint8_t previous_crosshair_mask = crosshair_preset_mask;
    esp_err_t err = apply_preset_with_crosshair(preset);
    if (err == ESP_OK) {
        stored.active_preset = preset;
        camera_state_authoritative = true;
        camera_state_status = ESP_OK;
    } else {
        esp_err_t rollback_err = restore_camera_snapshot(&previous, previous_crosshair_mask);
        camera_state_authoritative = rollback_err == ESP_OK;
        camera_state_status = rollback_err == ESP_OK ? err : rollback_err;
        ESP_LOGE(TAG, "Preset %u apply failed (%s), rollback %s", preset,
                 esp_err_to_name(err), rollback_err == ESP_OK ? "succeeded" : "failed");
    }
    return err;
}


static esp_err_t rollback_preset(uint8_t preset) {
    stored_values_t snapshot = stored;
    snapshot.active_preset = preset;
    esp_err_t err = restore_camera_snapshot(&snapshot, crosshair_preset_mask);
    if (err == ESP_OK) {
        stored = snapshot;
        esp_err_t commit_err = commit_settings_with_retry();
        persistence_authoritative = control_commit_is_authoritative(commit_err);
        persistence_status = commit_err;
    }
    camera_state_authoritative = err == ESP_OK;
    camera_state_status = err;
    return err;
}

static void loop_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    int max_gain = 0;

    adc_unit_t unit;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(POTI_PIN, &unit, &channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get ADC info about GPIO%d", (int)POTI_PIN);
    }

    int adc_raw;
    int last_adc_val = 0;
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, channel, &config));

    int64_t last_flip_check = esp_timer_get_time();
    control_button_state_t button_state;
    control_button_init(&button_state, gpio_get_level(MULTI_BTN) == 0,
                        esp_timer_get_time());
    #define FLIP_CHECK_INTERVAL (5 * 1000 * 1000)

    while (true) {
        const int64_t now = esp_timer_get_time();
        control_button_action_t button_action = control_button_sample(
            &button_state, gpio_get_level(MULTI_BTN) == 0, now,
            BUTTON_DEBOUNCE_US, BUTTON_LONG_PRESS_US);
        if (button_action == CONTROL_BUTTON_SHORT_PRESS) {
            const uint8_t previous = stored.active_preset;
            const uint8_t preset = next_preset(stored.active_preset);
            esp_err_t button_err = switch_preset(preset);
            if (button_err == ESP_OK) {
                button_err = commit_settings_with_retry();
                persistence_authoritative = control_commit_is_authoritative(button_err);
                persistence_status = button_err;
                if (button_err != ESP_OK) {
                    esp_err_t rollback_err = rollback_preset(previous);
                    if (rollback_err != ESP_OK) {
                        ESP_LOGE(TAG, "Short press rollback failed: %s",
                                 esp_err_to_name(rollback_err));
                    }
                }
            }
            if (button_err != ESP_OK) {
                ESP_LOGE(TAG, "Short press: unable to switch preset: %s",
                         esp_err_to_name(button_err));
            }
        } else if (button_action == CONTROL_BUTTON_LONG_PRESS) {
            const bool enabled = !preset_crosshair_enabled(stored.active_preset);
            if (Mini2_set_crosshair(&cam, enabled) == ESP_OK) {
                set_preset_crosshair_enabled(stored.active_preset, enabled);
                esp_err_t button_err = commit_settings_with_retry();
                persistence_authoritative = control_commit_is_authoritative(button_err);
                persistence_status = button_err;
                if (button_err == ESP_OK) {
                    ESP_LOGI(TAG, "Long press: preset %u crosshair %s",
                             stored.active_preset, enabled ? "enabled" : "disabled");
                } else {
                    esp_err_t rollback_err = Mini2_set_crosshair(&cam, !enabled);
                    set_preset_crosshair_enabled(stored.active_preset, !enabled);
                    if (rollback_err == ESP_OK) {
                        rollback_err = commit_settings_with_retry();
                        persistence_authoritative = control_commit_is_authoritative(rollback_err);
                        persistence_status = rollback_err;
                    }
                    if (rollback_err != ESP_OK) {
                        camera_state_authoritative = false;
                        camera_state_status = rollback_err;
                    }
                    ESP_LOGE(TAG, "Unable to persist long-press crosshair state: %s",
                             esp_err_to_name(button_err));
                }
            } else {
                ESP_LOGE(TAG, "Long press: unable to set crosshair");
            }
        }
        adc_oneshot_read(adc_handle, channel, &adc_raw);
        if (abs(adc_raw - last_adc_val) >= 100) {
            last_adc_val = adc_raw;
            float new_brightness = ((float)adc_raw / 4095.0) * 100.0;
            max_gain = (int)new_brightness;
            Mini2_set_brightness(&cam, max_gain);
        }
        if (stored.alignment.refresh_flip_mode && esp_timer_get_time() - last_flip_check > FLIP_CHECK_INTERVAL) {
            last_flip_check = esp_timer_get_time();
            
            Mini2_set_flip_mode(&cam, stored.alignment.flip_mode);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static esp_err_t post_handler(httpd_req_t *req) {
    int ret;
    esp_err_t request_err = ESP_OK;
    const char *request_error_message = "Unable to apply setting";
    bool device_mutated = false;

    const stored_values_t previous_stored = stored;
    const uint8_t previous_crosshair_mask = crosshair_preset_mask;
    const image_controls_t previous_image_controls = image_controls;
    const crosshair_positions_t previous_crosshair_positions = crosshair_positions;
    const bool previous_wifi_next_boot = wifi_next_boot;

    char* buf = (char*)malloc(req->content_len);

    if (buf == NULL) {
        ESP_LOGE(TAG, "Unable to malloc buffer for HTTP req");
        return ESP_FAIL;
    }

    jparse_ctx_t jctx;

    int bytes_read = httpd_req_recv(req, buf, req->content_len);

    ESP_LOG_BUFFER_HEXDUMP(TAG, buf, bytes_read, ESP_LOG_WARN);

    ret = json_parse_start(&jctx, buf, bytes_read);
    if (ret != OS_SUCCESS) {
        ESP_LOGE(TAG, "Parser failed");
        return ESP_FAIL;
    }
    
    int value;
    ret = json_obj_get_int(&jctx, "pseudo_color", &value);
    if (ret == OS_SUCCESS) {
        if (!control_pseudo_color_is_valid(value)) {
            request_err = ESP_ERR_INVALID_ARG;
            request_error_message = "Invalid pseudo color";
            goto invalid_request;
        }
        request_err = Mini2_set_color_pallet(&cam, (enum PseudoColor)value);
        if (request_err != ESP_OK) goto uart_failure;
        device_mutated = true;
        request_err = Mini2_set_flip_mode(&cam, stored.alignment.flip_mode);
        if (request_err != ESP_OK) goto uart_failure;
        stored.presets[stored.active_preset].pseudo_color = (enum PseudoColor)value;
    }
    ret = json_obj_get_int(&jctx, "scene_mode", &value);
    if (ret == OS_SUCCESS) {
        if (!control_scene_mode_is_valid(value)) {
            request_err = ESP_ERR_INVALID_ARG;
            request_error_message = "Invalid scene mode";
            goto invalid_request;
        }
        request_err = Mini2_set_scene_mode(&cam, (enum SceneMode)value);
        if (request_err != ESP_OK) goto uart_failure;
        device_mutated = true;
        request_err = Mini2_set_flip_mode(&cam, stored.alignment.flip_mode);
        if (request_err != ESP_OK) goto uart_failure;
        stored.presets[stored.active_preset].scene_mode = (enum SceneMode)value;
    }
    ret = json_obj_get_int(&jctx, "flip_mode", &value);
    if (ret == OS_SUCCESS) {
        if (!control_flip_mode_is_valid(value)) {
            request_err = ESP_ERR_INVALID_ARG;
            request_error_message = "Invalid flip mode";
            goto invalid_request;
        }
        request_err = Mini2_set_flip_mode(&cam, (enum FlipMode)value);
        if (request_err != ESP_OK) goto uart_failure;
        device_mutated = true;
        stored.alignment.flip_mode = (enum FlipMode)value;
    }
    ret = json_obj_get_int(&jctx, "av_format", &value);
    if (ret == OS_SUCCESS) {
        if (value < NTSC || value > PAL) {
            request_err = ESP_ERR_INVALID_ARG;
            request_error_message = "Invalid analog video format";
            goto invalid_request;
        }
        request_err = Mini2_set_analog_video_format(&cam, (enum AnalogVideoFormat)value);
        if (request_err != ESP_OK) goto uart_failure;
        device_mutated = true;
        stored.alignment.av_format = (enum AnalogVideoFormat)value;
    }
    /* Brightness is done via Poti, so no need
            ret = json_obj_get_int(&jctx, "brightness", &value);
    if (ret == OS_SUCCESS) {
        Mini2_set_brightness(&cam, value);
        stored.presets[stored.active_preset].brightness = value;
    }
    */
    ret = json_obj_get_int(&jctx, "contrast", &value);
    if (ret == OS_SUCCESS) {
        if (!control_percent_is_valid(value)) {
            request_err = ESP_ERR_INVALID_ARG;
            request_error_message = "Invalid contrast";
            goto invalid_request;
        }
        request_err = Mini2_set_contrast(&cam, value);
        if (request_err != ESP_OK) goto uart_failure;
        device_mutated = true;
        stored.presets[stored.active_preset].contrast = value;
    }
    ret = json_obj_get_int(&jctx, "edge_enhancment_gear", &value);
    if (ret == OS_SUCCESS) {
        if (!control_edge_gear_is_valid(value)) {
            request_err = ESP_ERR_INVALID_ARG;
            request_error_message = "Invalid edge enhancement";
            goto invalid_request;
        }
        request_err = Mini2_set_edge_enhancment(&cam, value);
        if (request_err != ESP_OK) goto uart_failure;
        device_mutated = true;
        stored.presets[stored.active_preset].edge_enhancment_gear = value;
    }
    ret = json_obj_get_int(&jctx, "detail_enhancement_gear", &value);
    if (ret == OS_SUCCESS) {
        if (!control_percent_is_valid(value)) {
            request_err = ESP_ERR_INVALID_ARG;
            request_error_message = "Invalid detail enhancement";
            goto invalid_request;
        }
        request_err = Mini2_set_detail_enhancement(&cam, value);
        if (request_err != ESP_OK) goto uart_failure;
        device_mutated = true;
        stored.presets[stored.active_preset].detail_enhancement_gear = value;
    }

#define APPLY_IMAGE_CONTROL(key, member, mask, setter, message) \
    ret = json_obj_get_int(&jctx, key, &value); \
    if (ret == OS_SUCCESS) { \
        if (!control_image_level_is_valid(value)) { request_err = ESP_ERR_INVALID_ARG; request_error_message = message; goto invalid_request; } \
        request_err = setter(&cam, value); \
        if (request_err != ESP_OK) goto uart_failure; \
        device_mutated = true; \
        image_controls.member[stored.active_preset] = value; \
        image_controls.mask |= (1U << stored.active_preset); \
    }
    APPLY_IMAGE_CONTROL("spatial_noise_reduction", spatial, spatial_mask, Mini2_set_spatial_noise_reduction, "Invalid spatial noise reduction");
    APPLY_IMAGE_CONTROL("temporal_noise_reduction", temporal, temporal_mask, Mini2_set_temporal_noise_reduction, "Invalid temporal noise reduction");
    APPLY_IMAGE_CONTROL("gamma_intensity", gamma, gamma_mask, Mini2_set_gamma_intensity, "Invalid gamma intensity");
#undef APPLY_IMAGE_CONTROL

    bool bool_val;
    ret = json_obj_get_bool(&jctx, "burn_protection_en", &bool_val);
    if (ret == OS_SUCCESS) {
        request_err = Mini2_set_burn_protection(&cam, bool_val);
        if (request_err != ESP_OK) goto uart_failure;
        device_mutated = true;
        stored.presets[stored.active_preset].burn_protection_en = bool_val;
    }
    ret = json_obj_get_bool(&jctx, "auto_shutter_en", &bool_val);
    if (ret == OS_SUCCESS) {
        request_err = Mini2_set_auto_shutter(&cam, bool_val);
        if (request_err != ESP_OK) goto uart_failure;
        device_mutated = true;
        stored.presets[stored.active_preset].auto_shutter_en = bool_val;
    }

    ret = json_obj_get_bool(&jctx, "resend", &bool_val);
    if (ret == OS_SUCCESS && bool_val) {
        Mini2_set_digital_video_format(&cam, true, UsbProgressive, Hz50);
        Mini2_set_analog_video_format(&cam, PAL);
    }

    ret = json_obj_get_bool(&jctx, "refresh_flip_mode", &bool_val);
    if (ret == OS_SUCCESS) {
        stored.alignment.refresh_flip_mode = bool_val;
    }

    ret = json_obj_get_bool(&jctx, "preset_en", &bool_val);
    if (ret == OS_SUCCESS) {
        if (stored.active_preset == 0) {
            stored.presets[stored.active_preset].preset_en = true; // Preset0 is always enabled.
        } else {
            stored.presets[stored.active_preset].preset_en = bool_val;
        }
    }

    const bool has_zoom = json_obj_has_key(&jctx, "zoom");
    ret = json_obj_get_object(&jctx, "zoom");
    if (has_zoom && ret != OS_SUCCESS) {
        request_err = ESP_ERR_INVALID_ARG;
        request_error_message = "Invalid zoom object";
        goto invalid_request;
    }
    if (has_zoom) {
        int x, y, zoom;
        if (json_obj_get_int(&jctx, "x", &x) != OS_SUCCESS ||
            json_obj_get_int(&jctx, "y", &y) != OS_SUCCESS ||
            json_obj_get_int(&jctx, "zoom", &zoom) != OS_SUCCESS ||
            !control_point_zoom_is_valid(x, y, zoom, cam.variant.sensor_width, cam.variant.sensor_height)) {
            json_obj_leave_object(&jctx);
            request_err = ESP_ERR_INVALID_ARG;
            request_error_message = "Invalid zoom coordinates";
            goto invalid_request;
        }
        request_err = Mini2_set_point_zoom(&cam, (uint16_t)x, (uint16_t)y, (uint8_t)zoom);
        if (request_err != ESP_OK) {
            json_obj_leave_object(&jctx);
            goto uart_failure;
        }
        device_mutated = true;
        stored.alignment.zoom = zoom;
        stored.alignment.zoom_x = x;
        stored.alignment.zoom_y = y;
        json_obj_leave_object(&jctx);
    }

    const bool has_crosshair_position = json_obj_has_key(&jctx, "crosshair_position");
    ret = json_obj_get_object(&jctx, "crosshair_position");
    if (has_crosshair_position && ret != OS_SUCCESS) {
        request_err = ESP_ERR_INVALID_ARG;
        request_error_message = "Invalid crosshair position object";
        goto invalid_request;
    }
    if (has_crosshair_position) {
        int x, y;
        if (json_obj_get_int(&jctx, "x", &x) != OS_SUCCESS ||
            json_obj_get_int(&jctx, "y", &y) != OS_SUCCESS ||
            !control_crosshair_position_is_valid(x, y, cam.variant.sensor_width,
                                                  cam.variant.sensor_height)) {
            json_obj_leave_object(&jctx);
            request_err = ESP_ERR_INVALID_ARG;
            request_error_message = "Invalid crosshair coordinates";
            goto invalid_request;
        }
        request_err = Mini2_set_crosshair_position(&cam, (uint16_t)x, (uint16_t)y);
        if (request_err != ESP_OK) {
            json_obj_leave_object(&jctx);
            goto uart_failure;
        }
        device_mutated = true;
        crosshair_positions.x[stored.active_preset] = (uint16_t)x;
        crosshair_positions.y[stored.active_preset] = (uint16_t)y;
        json_obj_leave_object(&jctx);
    }

    const bool has_crosshair_enabled = json_obj_has_key(&jctx, "crosshair_enabled");
    ret = json_obj_get_bool(&jctx, "crosshair_enabled", &bool_val);
    if (has_crosshair_enabled && ret != OS_SUCCESS) {
        request_err = ESP_ERR_INVALID_ARG;
        request_error_message = "Invalid crosshair setting";
        goto invalid_request;
    }
    if (has_crosshair_enabled) {
        request_err = Mini2_set_crosshair(&cam, bool_val);
        if (request_err != ESP_OK) goto uart_failure;
        device_mutated = true;
        set_preset_crosshair_enabled(stored.active_preset, bool_val);
    }

    ret = json_obj_get_bool(&jctx, "wifi_next_boot", &bool_val);
    if (ret == OS_SUCCESS) {
        wifi_next_boot = bool_val;
    }

    ret = json_obj_get_int(&jctx, "active_profile", &value);
    if (ret == OS_SUCCESS) {
        if ((0 <= value) && (value < PRESET_COUNT)) {
            if (switch_preset(value) != ESP_OK) {
                request_err = camera_state_status;
                request_error_message = "Unable to switch active profile";
                goto uart_failure;
            }
            device_mutated = true;
        } else {
            ESP_LOGE(TAG, "Active profile number would be out-of-bounds!");
        }   
    }

    ret = json_obj_get_bool(&jctx, "NUC", &bool_val);
    if (ret == OS_SUCCESS && bool_val) {
        Mini2_NUC(&cam);
    }

    ret = json_obj_get_bool(&jctx, "BG", &bool_val);
    if (ret == OS_SUCCESS && bool_val) {
        Mini2_Background_Correction(&cam);
    }

    ret = json_obj_get_bool(&jctx, "parameters_save", &bool_val);
    if (ret == OS_SUCCESS && bool_val) {
        Mini2_parameters_save(&cam);
    }

    json_parse_end(&jctx);
    free(buf);

    if (stored.first_boot) {
        stored.first_boot = false;
    }

    esp_err_t err = commit_settings_with_retry();
    if (err == ESP_OK) {
        persistence_authoritative = true;
        persistence_status = ESP_OK;
        ESP_LOGI(TAG, "Stored values in NVS");
    } else {
        stored = previous_stored;
        crosshair_preset_mask = previous_crosshair_mask;
        image_controls = previous_image_controls;
        crosshair_positions = previous_crosshair_positions;
        wifi_next_boot = previous_wifi_next_boot;
        esp_err_t rollback_err = ESP_OK;
        if (control_rollback_required(device_mutated)) {
            rollback_err = restore_camera_snapshot(&stored, crosshair_preset_mask);
            camera_state_authoritative = rollback_err == ESP_OK;
            camera_state_status = rollback_err == ESP_OK ? err : rollback_err;
        }
        esp_err_t rollback_commit_err = commit_settings_with_retry();
        persistence_authoritative = control_commit_is_authoritative(rollback_commit_err);
        persistence_status = rollback_commit_err;
        ESP_LOGE(TAG, "Unable to persist web changes (%s), rollback %s",
                 esp_err_to_name(err),
                 rollback_err == ESP_OK && rollback_commit_err == ESP_OK ? "succeeded" : "failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to persist settings");
        return ESP_OK;
    }
    return httpd_resp_send_chunk(req, NULL, 0);

uart_failure:
    stored = previous_stored;
    crosshair_preset_mask = previous_crosshair_mask;
    image_controls = previous_image_controls;
    crosshair_positions = previous_crosshair_positions;
    wifi_next_boot = previous_wifi_next_boot;
    if (control_rollback_required(device_mutated)) {
        esp_err_t rollback_err = restore_camera_snapshot(&stored, crosshair_preset_mask);
        camera_state_authoritative = rollback_err == ESP_OK;
        camera_state_status = rollback_err == ESP_OK ? request_err : rollback_err;
        ESP_LOGE(TAG, "Web UART change failed (%s), rollback %s",
                 esp_err_to_name(request_err), rollback_err == ESP_OK ? "succeeded" : "failed");
    }
    json_parse_end(&jctx);
    free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, request_error_message);
    return ESP_OK;

invalid_request:
    stored = previous_stored;
    crosshair_preset_mask = previous_crosshair_mask;
    image_controls = previous_image_controls;
    crosshair_positions = previous_crosshair_positions;
    wifi_next_boot = previous_wifi_next_boot;
    if (control_rollback_required(device_mutated)) {
        esp_err_t rollback_err = restore_camera_snapshot(&stored, crosshair_preset_mask);
        camera_state_authoritative = rollback_err == ESP_OK;
        camera_state_status = rollback_err == ESP_OK ? request_err : rollback_err;
    }
    json_parse_end(&jctx);
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, request_error_message);
    return ESP_OK;
}

static esp_err_t retireve_values(httpd_req_t *req) {
    char out_json[CONTROL_STATE_JSON_BUFFER_SIZE];

    int res = snprintf(out_json, sizeof(out_json), CONTROL_STATE_JSON_FORMAT,
        PRESET_COUNT,
        stored.active_preset,
        (uint8_t)stored.presets[stored.active_preset].preset_en,
        (uint8_t)stored.presets[stored.active_preset].pseudo_color,
        (uint8_t)stored.presets[stored.active_preset].scene_mode,
        stored.presets[stored.active_preset].contrast,
        stored.presets[stored.active_preset].edge_enhancment_gear,
        stored.presets[stored.active_preset].detail_enhancement_gear,
        (image_controls.spatial_mask & (1U << stored.active_preset)) ? image_controls.spatial[stored.active_preset] : -1,
        (image_controls.temporal_mask & (1U << stored.active_preset)) ? image_controls.temporal[stored.active_preset] : -1,
        (image_controls.gamma_mask & (1U << stored.active_preset)) ? image_controls.gamma[stored.active_preset] : -1,
        (uint8_t)stored.presets[stored.active_preset].burn_protection_en,
        (uint8_t)stored.presets[stored.active_preset].auto_shutter_en,
        (uint8_t)stored.alignment.flip_mode,
        stored.alignment.zoom,
        crosshair_positions.x[stored.active_preset],
        crosshair_positions.y[stored.active_preset],
        stored.alignment.av_format,
        cam.variant.sensor_width,
        cam.variant.sensor_height,
        stored.alignment.refresh_flip_mode,
        (uint8_t)preset_crosshair_enabled(stored.active_preset),
        (uint8_t)wifi_next_boot,
        FIRMWARE_VERSION,
        boot_analog_video_initial_status == ESP_OK,
        boot_analog_video_initial_status,
        (uint8_t)camera_state_authoritative,
        camera_state_status,
        (uint8_t)persistence_authoritative,
        persistence_status
    );

    if (res <= 0 || (size_t)res >= sizeof(out_json)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"state_format_failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out_json, res);
}

static const httpd_uri_t retireve_values_route = {
    .uri       = "/get",
    .method    = HTTP_GET,
    .handler   = retireve_values,
    .user_ctx  = NULL
};

static const httpd_uri_t echo = {
    .uri       = "/set",
    .method    = HTTP_POST,
    .handler   = post_handler,
    .user_ctx  = NULL
};

esp_err_t index_get_handler(httpd_req_t *req) {
    const uint32_t html_size = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, html_size);
    return ESP_OK;
}

httpd_uri_t index_uri = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = index_get_handler,
    .user_ctx = NULL
};

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    for (int i=0; i<PRESET_COUNT; i++) {
        stored.presets[i] = i < (sizeof(default_presets) / sizeof(value_preset_t))
            ? default_presets[i] : base_preset;
    }

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &flash_handle));
    size_t len = sizeof(stored_values_t);
    esp_err_t err = nvs_get_blob(flash_handle, "stored_values", &stored, &len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Failed to read stored values from NVS, going with defaults.");
    }
    uint8_t persisted_value = 0;
    if (nvs_get_u8(flash_handle, "crosshair_mask", &persisted_value) == ESP_OK) {
        crosshair_preset_mask = persisted_value & ((1U << PRESET_COUNT) - 1U);
    } else if (nvs_get_u8(flash_handle, "crosshair_en", &persisted_value) == ESP_OK && persisted_value != 0) {
        crosshair_preset_mask = (1U << PRESET_COUNT) - 1U;
    }
    size_t image_controls_len = sizeof(image_controls);
    if (nvs_get_blob(flash_handle, "image_controls", &image_controls,
                     &image_controls_len) != ESP_OK ||
        image_controls_len != sizeof(image_controls)) {
        memset(&image_controls, 0, sizeof(image_controls));
    }
    size_t cursor_positions_len = sizeof(crosshair_positions);
    uint8_t cursor_migrated = 0;
    if (nvs_get_blob(flash_handle, "cursor_pos", &crosshair_positions,
                     &cursor_positions_len) != ESP_OK ||
        cursor_positions_len != sizeof(crosshair_positions)) {
        for (int i = 0; i < PRESET_COUNT; i++) {
            crosshair_positions.x[i] = stored.alignment.zoom_x < cam.variant.sensor_width
                ? stored.alignment.zoom_x : cam.variant.sensor_width - 1;
            crosshair_positions.y[i] = stored.alignment.zoom_y < cam.variant.sensor_height
                ? stored.alignment.zoom_y : cam.variant.sensor_height - 1;
        }
    }
    if (nvs_get_u8(flash_handle, "cursor_migrated", &cursor_migrated) != ESP_OK ||
        cursor_migrated == 0) {
        stored.alignment.zoom = 10;
        stored.alignment.zoom_x = cam.variant.sensor_width / 2;
        stored.alignment.zoom_y = cam.variant.sensor_height / 2;
        ESP_ERROR_CHECK(nvs_set_u8(flash_handle, "cursor_migrated", 1));
        ESP_ERROR_CHECK(commit_settings());
    }
    persisted_value = 0;
    if (nvs_get_u8(flash_handle, "wifi_next", &persisted_value) == ESP_OK) {
        wifi_next_boot = persisted_value != 0;
    }

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << MULTI_BTN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);

    bool wifi_en = (gpio_get_level(MULTI_BTN) == 0) || stored.first_boot || wifi_next_boot;
    if (wifi_next_boot) {
        wifi_next_boot = false;
        ESP_ERROR_CHECK(commit_settings());
    }
    ESP_LOGI(TAG, "Wifi: %d", (int)wifi_en);

    if (wifi_en) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        sta_netif = esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
        wifi_config_t wifi_config = {
            .ap = {
                .ssid = SSID,
                .ssid_len = strlen(SSID),
                .password = PASSWORD,
                .channel = 1,
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = {
                    .required = true,
                },
            }
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config) );
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    ESP_LOGI(TAG, "Waiting 5000 ms before Mini2 UART initialization");
    vTaskDelay(pdMS_TO_TICKS(5000));
    Mini2_init(&cam);
    ESP_LOGI(TAG, "Mini2 UART initialized at %lld ms", esp_timer_get_time() / 1000);

    boot_analog_video_initial_status = Mini2_apply_preset(&cam, &stored.presets[stored.active_preset], &stored.alignment, false);
    esp_err_t boot_crosshair_status = Mini2_set_crosshair(&cam, preset_crosshair_enabled(stored.active_preset));
    if (boot_analog_video_initial_status == ESP_OK && boot_crosshair_status != ESP_OK) {
        boot_analog_video_initial_status = boot_crosshair_status;
    }
    if (boot_analog_video_initial_status == ESP_OK) {
        boot_analog_video_initial_status = Mini2_set_crosshair_position(
            &cam, crosshair_positions.x[stored.active_preset],
            crosshair_positions.y[stored.active_preset]);
    }
    if (boot_analog_video_initial_status == ESP_OK &&
        (image_controls.spatial_mask & (1U << stored.active_preset)))
        boot_analog_video_initial_status = Mini2_set_spatial_noise_reduction(&cam, image_controls.spatial[stored.active_preset]);
    if (boot_analog_video_initial_status == ESP_OK &&
        (image_controls.temporal_mask & (1U << stored.active_preset)))
        boot_analog_video_initial_status = Mini2_set_temporal_noise_reduction(&cam, image_controls.temporal[stored.active_preset]);
    if (boot_analog_video_initial_status == ESP_OK &&
        (image_controls.gamma_mask & (1U << stored.active_preset)))
        boot_analog_video_initial_status = Mini2_set_gamma_intensity(&cam, image_controls.gamma[stored.active_preset]);
    last_applied_preset = stored.active_preset;
    if (boot_analog_video_initial_status == ESP_OK) {
        camera_state_authoritative = true;
        camera_state_status = ESP_OK;
        ESP_LOGI(TAG, "Boot video save/apply initialization succeeded");
    } else {
        camera_state_authoritative = false;
        camera_state_status = boot_analog_video_initial_status;
        ESP_LOGE(TAG, "Boot video save/apply initialization failed: %s", esp_err_to_name(boot_analog_video_initial_status));
    }
    xTaskCreate(loop_task, "loop task", 16384, NULL, 5, NULL);

    if (wifi_en) {
        httpd_handle_t server = NULL;
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.lru_purge_enable = true;

        ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
        if (httpd_start(&server, &config) == ESP_OK) {
            ESP_LOGI(TAG, "Registering URI handlers");
            httpd_register_uri_handler(server, &echo);
            httpd_register_uri_handler(server, &index_uri);
            httpd_register_uri_handler(server, &retireve_values_route);
        }
    }

}
