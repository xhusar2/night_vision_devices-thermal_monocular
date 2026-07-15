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
    assert(control_point_zoom_is_valid(0, 0, 10, 256, 192));
    assert(control_point_zoom_is_valid(255, 191, 80, 256, 192));
    assert(!control_point_zoom_is_valid(-1, 96, 10, 256, 192));
    assert(!control_point_zoom_is_valid(256, 96, 10, 256, 192));
    assert(!control_point_zoom_is_valid(128, -1, 10, 256, 192));
    assert(!control_point_zoom_is_valid(128, 192, 10, 256, 192));
    assert(!control_point_zoom_is_valid(128, 96, 9, 256, 192));
    assert(!control_point_zoom_is_valid(128, 96, 81, 256, 192));
    assert(!control_point_zoom_is_valid(0, 0, 10, 0, 192));

    char state_json[CONTROL_STATE_JSON_BUFFER_SIZE];
    int state_length = snprintf(state_json, sizeof(state_json), CONTROL_STATE_JSON_FORMAT,
        UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX,
        UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX,
        UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, "0.4.2",
        UINT_MAX, INT_MIN, UINT_MAX, UINT_MAX, INT_MIN, UINT_MAX, INT_MIN);
    assert(state_length > 0);
    assert((size_t)state_length < sizeof(state_json));

    char *main_source = read_text("main/main.c");
    const char *server_setup = strstr(main_source, "httpd_start(&server, &config)");
    const char *recovery_start = strstr(main_source, "xTaskCreate(boot_analog_video_task");
    assert(server_setup != NULL && recovery_start != NULL && recovery_start > server_setup);
    const char *task = strstr(main_source, "static void boot_analog_video_task");
    assert(task != NULL);
    const char *task_end = strstr(task, "static void wifi_event_handler");
    assert(task_end != NULL);
    const char *delay_five = strstr(task, "vTaskDelay(pdMS_TO_TICKS(5000))");
    const char *send_opposite = strstr(task, "Mini2_set_analog_video_format(&cam, opposite_format)");
    const char *delay_one = strstr(task, "vTaskDelay(pdMS_TO_TICKS(1000))");
    const char *restore = strstr(task, "Mini2_set_analog_video_format(&cam, boot_analog_video_format)");
    assert(delay_five != NULL && send_opposite != NULL && delay_one != NULL && restore != NULL);
    assert(delay_five < send_opposite && send_opposite < delay_one);
    assert(delay_one < restore && restore < task_end);
    assert(strstr(task, "Mini2_NUC") == NULL || strstr(task, "Mini2_NUC") > task_end);
    assert(strstr(task, "Mini2_Background_Correction") == NULL || strstr(task, "Mini2_Background_Correction") > task_end);
    assert(strstr(main_source, "apply_preset_with_crosshair(stored.active_preset)") != NULL);
    free(main_source);

    char *html = read_text("main/index.html");
    assert(strstr(html, "unavailable at true 1.0×") != NULL);
    assert(strstr(html, "zoom_x_minus") != NULL && strstr(html, "zoom_y_plus") != NULL);
    free(html);
    return 0;
}
