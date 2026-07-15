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
        UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, "0.4.3",
        UINT_MAX, INT_MIN);
    assert(state_length > 0);
    assert((size_t)state_length < sizeof(state_json));

    char *main_source = read_text("main/main.c");
    assert(strstr(main_source, "boot_analog_video_task") == NULL);
    assert(strstr(main_source, "#define FIRMWARE_VERSION \"0.4.3\"") != NULL);
    assert(strstr(main_source, "apply_preset_with_crosshair(stored.active_preset)") != NULL);
    free(main_source);

    char *html = read_text("main/index.html");
    assert(strstr(html, "including at true 1.0×") != NULL);
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
    const char *nuc = strstr(apply, "Mini2_NUC(cam)");
    assert(digital != NULL && analog != NULL && nuc != NULL && digital < analog && analog < nuc);
    assert(strstr(apply, "format_read_err == ESP_OK && format != alignment->av_format") != NULL);
    free(mini2);
    return 0;
}
