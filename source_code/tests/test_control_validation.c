#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control_validation.h"

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

    char state_json[CONTROL_STATE_JSON_BUFFER_SIZE];
    int state_length = snprintf(state_json, sizeof(state_json), CONTROL_STATE_JSON_FORMAT,
        UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX,
        UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX,
        UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, "0.4.5",
        UINT_MAX, INT_MIN);
    assert(state_length > 0);
    assert((size_t)state_length < sizeof(state_json));

    char *main_source = read_text("main/main.c");
    assert(strstr(main_source, "boot_analog_video_task") == NULL);
    assert(strstr(main_source, "#define FIRMWARE_VERSION \"0.4.5\"") != NULL);
    const char *uart_init = strstr(main_source, "Mini2_init(&cam)");
    assert(uart_init != NULL);
    const char *ready_delay = strstr(main_source, "vTaskDelay(pdMS_TO_TICKS(5000))");
    assert(ready_delay != NULL);
    assert(ready_delay < uart_init);
    const char *cold_boot = strstr(uart_init, "Mini2_apply_preset");
    assert(cold_boot != NULL);
    assert(strstr(main_source, "apply_preset_with_crosshair(stored.active_preset)") != NULL);
    free(main_source);

    char *html = read_text("main/index.html");
    assert(strstr(html, "aim zoom must be at least 1.1×") != NULL);
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
    assert(strstr(apply, "Mini2_NUC(cam)") == NULL);
    free(mini2);
    return 0;
}
