#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control_validation.h"

typedef struct {
    int preset_result;
    int crosshair_result;
    int preset_calls;
    int crosshair_calls;
} transaction_fake_t;

typedef struct {
    int apply_result;
    int persist_failures;
    int rollback_result;
    int apply_calls;
    int persist_calls;
    int rollback_calls;
} state_transaction_fake_t;

static int fake_apply(void *context) {
    state_transaction_fake_t *fake = context;
    fake->apply_calls++;
    return fake->apply_result;
}

static int fake_persist(void *context) {
    state_transaction_fake_t *fake = context;
    fake->persist_calls++;
    return fake->persist_calls <= fake->persist_failures ? -5 : 0;
}

static int fake_rollback(void *context) {
    state_transaction_fake_t *fake = context;
    fake->rollback_calls++;
    return fake->rollback_result;
}

static int fake_apply_preset(void *context, uint8_t preset) {
    transaction_fake_t *fake = context;
    (void)preset;
    fake->preset_calls++;
    return fake->preset_result;
}

static int fake_apply_crosshair(void *context, bool enabled) {
    transaction_fake_t *fake = context;
    (void)enabled;
    fake->crosshair_calls++;
    return fake->crosshair_result;
}

static char *read_text(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length >= 0);
    rewind(file);
    char *text = malloc((size_t)length + 1);
    assert(text != NULL);
    assert(fread(text, 1, (size_t)length, file) == (size_t)length);
    text[length] = '\0';
    fclose(file);
    return text;
}

int main(void) {
    assert(control_point_zoom_is_valid(0, 0, 11, 256, 192));
    assert(control_point_zoom_is_valid(255, 191, 80, 256, 192));
    assert(!control_point_zoom_is_valid(-1, 96, 10, 256, 192));
    assert(!control_point_zoom_is_valid(256, 96, 10, 256, 192));
    assert(!control_point_zoom_is_valid(128, -1, 10, 256, 192));
    assert(!control_point_zoom_is_valid(128, 192, 10, 256, 192));
    assert(!control_point_zoom_is_valid(128, 96, 10, 256, 192));
    assert(!control_point_zoom_is_valid(128, 96, 81, 256, 192));
    assert(!control_point_zoom_is_valid(0, 0, 10, 0, 192));

    control_button_state_t button;
    control_button_init(&button, false, 0);
    assert(control_button_sample(&button, true, 1000, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, false, 20000, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, true, 30000, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, true, 79999, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, true, 80000, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, false, 100000, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, false, 150000, 50000, 2000000) == CONTROL_BUTTON_SHORT_PRESS);

    control_button_init(&button, false, 0);
    assert(control_button_sample(&button, true, 1000, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, true, 51000, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, true, 2000999, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, true, 2001000, 50000, 2000000) == CONTROL_BUTTON_LONG_PRESS);
    assert(control_button_sample(&button, true, 3000000, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, false, 3100000, 50000, 2000000) == CONTROL_BUTTON_NONE);
    assert(control_button_sample(&button, false, 3150000, 50000, 2000000) == CONTROL_BUTTON_NONE);

    transaction_fake_t transaction = {.preset_result = -7};
    assert(control_apply_preset_transaction(&transaction, 1, true,
        fake_apply_preset, fake_apply_crosshair) == -7);
    assert(transaction.preset_calls == 1 && transaction.crosshair_calls == 0);
    transaction = (transaction_fake_t) {.crosshair_result = -9};
    assert(control_apply_preset_transaction(&transaction, 1, true,
        fake_apply_preset, fake_apply_crosshair) == -9);
    assert(transaction.preset_calls == 1 && transaction.crosshair_calls == 1);

    state_transaction_fake_t state_transaction = {.apply_result = -7};
    control_transaction_result_t state_result = control_run_transaction(
        &state_transaction, fake_apply, fake_persist, fake_rollback, 3);
    assert(state_result.result == -7 && state_result.authoritative);
    assert(state_transaction.persist_calls == 0 && state_transaction.rollback_calls == 1);
    state_transaction = (state_transaction_fake_t) {.apply_result = -7, .rollback_result = -8};
    state_result = control_run_transaction(
        &state_transaction, fake_apply, fake_persist, fake_rollback, 3);
    assert(!state_result.authoritative && state_result.rollback_result == -8);
    state_transaction = (state_transaction_fake_t) {.persist_failures = 2};
    state_result = control_run_transaction(
        &state_transaction, fake_apply, fake_persist, fake_rollback, 3);
    assert(state_result.result == 0 && state_result.authoritative);
    assert(state_transaction.persist_calls == 3 && state_transaction.rollback_calls == 0);
    state_transaction = (state_transaction_fake_t) {.persist_failures = 3};
    state_result = control_run_transaction(
        &state_transaction, fake_apply, fake_persist, fake_rollback, 3);
    assert(state_result.result == -5 && state_result.authoritative);
    assert(state_transaction.persist_calls == 3 && state_transaction.rollback_calls == 1);
    state_transaction = (state_transaction_fake_t) {.persist_failures = 3, .rollback_result = -9};
    state_result = control_run_transaction(
        &state_transaction, fake_apply, fake_persist, fake_rollback, 3);
    assert(!state_result.authoritative && state_result.rollback_result == -9);
    state_transaction = (state_transaction_fake_t) {0};
    state_result = control_run_transaction(
        &state_transaction, fake_apply, fake_persist, fake_rollback, 3);
    assert(state_result.result == 0 && state_result.authoritative);
    transaction = (transaction_fake_t) {0};
    assert(control_apply_preset_transaction(&transaction, 1, true,
        fake_apply_preset, fake_apply_crosshair) == 0);
    assert(transaction.preset_calls == 1 && transaction.crosshair_calls == 1);

    char state_json[CONTROL_STATE_JSON_BUFFER_SIZE];
    int state_length = snprintf(state_json, sizeof(state_json), CONTROL_STATE_JSON_FORMAT,
        UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX,
        UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX,
        UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, "0.4.7",
        UINT_MAX, INT_MIN, UINT_MAX, INT_MIN);
    assert(state_length > 0);
    assert((size_t)state_length < sizeof(state_json));

    char *main_source = read_text("main/main.c");
    assert(strstr(main_source, "boot_analog_video_task") == NULL);
    assert(strstr(main_source, "#define FIRMWARE_VERSION \"0.4.7\"") != NULL);
    assert(strstr(main_source, "value < NTSC || value > PAL") != NULL);
    assert(strstr(main_source, "stored.alignment.av_format = (enum AnalogVideoFormat)value") != NULL);
    assert(strstr(main_source, "Unable to set analog video format") != NULL);
    const char *uart_init = strstr(main_source, "Mini2_init(&cam)");
    assert(uart_init != NULL);
    const char *ready_delay = strstr(main_source, "vTaskDelay(pdMS_TO_TICKS(5000))");
    assert(ready_delay != NULL);
    assert(ready_delay < uart_init);
    const char *cold_boot = strstr(uart_init, "Mini2_apply_preset");
    assert(cold_boot != NULL);
    assert(strstr(main_source, "switch_preset(preset)") != NULL);
    assert(strstr(main_source, "#define BUTTON_LONG_PRESS_US (2 * 1000 * 1000)") != NULL);
    assert(strstr(main_source, ".intr_type = GPIO_INTR_DISABLE") != NULL);
    assert(strstr(main_source, "control_button_sample") != NULL);
    assert(strstr(main_source, "switch_preset(value) != ESP_OK") != NULL);
    assert(strstr(main_source, "set_preset_crosshair_enabled(stored.active_preset, enabled)") != NULL);
    assert(strstr(main_source, "control_apply_preset_transaction") != NULL);
    assert(strstr(main_source, "nvs_set_u8(flash_handle, \"crosshair_mask\", crosshair_preset_mask)") != NULL);
    assert(strstr(main_source, "Mini2_set_crosshair(&cam, preset_crosshair_enabled(stored.active_preset))") != NULL);
    free(main_source);

    char *html = read_text("main/index.html");
    assert(strstr(html, "aim zoom must be at least 1.1×") != NULL);
    assert(strstr(html, "Hold the preset button for 2 seconds") != NULL);
    assert(strstr(html, "id=\"aim_zoom\" min=\"11\"") != NULL);
    assert(strstr(html, "zoom_x_minus") != NULL && strstr(html, "zoom_y_plus") != NULL);
    assert(strstr(html, "zoom_x.min = 0") != NULL);
    assert(strstr(html, "zoom_x.max = sensor_width - 1") != NULL);
    assert(strstr(html, "zoom_y.min = 0") != NULL);
    assert(strstr(html, "zoom_y.max = sensor_height - 1") != NULL);
    assert(strstr(html, "zoom_x.disabled") == NULL);
    assert(strstr(html, "x: zoom_x") != NULL && strstr(html, "y: zoom_y") != NULL);
    assert(strstr(html, "zoom: zoom") != NULL);
    free(html);

    char *mini2 = read_text("components/Mini2/Mini2.c");
    const char *apply = strstr(mini2, "esp_err_t Mini2_apply_preset");
    assert(apply != NULL);
    const char *digital = strstr(apply, "Mini2_set_digital_video_format(cam, true, UsbProgressive, Hz50)");
    const char *analog = strstr(apply, "Mini2_set_analog_video_format(cam, alignment->av_format)");
    const char *save = strstr(apply, "Mini2_save_video(cam)");
    const char *scene = strstr(apply, "Mini2_set_scene_mode");
    assert(digital != NULL && analog != NULL && save != NULL && scene != NULL &&
           digital < analog && analog < save && save < scene);
    assert(strstr(apply, "Boot video digital enable at %lld ms") != NULL);
    assert(strstr(apply, "Boot video analog format at %lld ms") != NULL);
    assert(strstr(apply, "Boot video save/apply at %lld ms") != NULL);
    assert(strstr(apply, "format_read_err != ESP_OK || format != alignment->av_format") != NULL);
    assert(strstr(apply, "vTaskDelay(pdMS_TO_TICKS(500))") != NULL);
    assert(strstr(apply, "Mini2_NUC(cam)") == NULL);
    assert(strstr(mini2, "memset(out_buf, 0, expected_len)") != NULL);
    assert(strstr(mini2, "bytes_read <= 0 || (size_t)bytes_read != expected_len") != NULL);
    assert(strstr(mini2, "uint8_t rx_buffer[11] = {0}") != NULL);
    free(mini2);
    return 0;
}
