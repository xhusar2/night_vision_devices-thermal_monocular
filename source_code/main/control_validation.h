#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CONTROL_STATE_JSON_BUFFER_SIZE 1024
#define CONTROL_STATE_JSON_FORMAT "{ " \
    "\"preset_count\": %u, " \
    "\"active_profile\": %u, " \
    "\"preset_en\": %u, " \
    "\"pseudo_color\": %u, " \
    "\"scene_mode\": %u, " \
    "\"contrast\": %u, " \
    "\"edge_enhancment_gear\": %u, " \
    "\"detail_enhancement_gear\": %u, " \
    "\"burn_protection_en\": %u, " \
    "\"auto_shutter_en\": %u, " \
    "\"flip_mode\": %u, " \
    "\"zoom\": %u, " \
    "\"zoom_x\": %u, " \
    "\"zoom_y\": %u, " \
    "\"av_format\": %u, " \
    "\"sensor_width\": %u, " \
    "\"sensor_height\": %u, " \
    "\"refresh_flip_mode\": %u, " \
    "\"crosshair_enabled\": %u, " \
    "\"wifi_next_boot\": %u, " \
    "\"firmware_version\": \"%s\", " \
    "\"boot_analog_video_initial_ok\": %u, " \
    "\"boot_analog_video_initial_status\": %d, " \
    "\"boot_analog_video_pending\": %u, " \
    "\"boot_analog_video_opposite_ok\": %u, " \
    "\"boot_analog_video_opposite_status\": %d, " \
    "\"boot_analog_video_restore_ok\": %u, " \
    "\"boot_analog_video_restore_status\": %d " \
    "}"

static inline bool control_point_zoom_is_valid(int x, int y, int zoom,
                                                uint16_t sensor_width,
                                                uint16_t sensor_height) {
    if (sensor_width == 0 || sensor_height == 0 || zoom < 10 || zoom > 80) {
        return false;
    }

    return x >= 0 && x < sensor_width && y >= 0 && y < sensor_height;
}
