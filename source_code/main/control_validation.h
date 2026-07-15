#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool control_point_zoom_is_valid(int x, int y, int zoom,
                                                uint16_t sensor_width,
                                                uint16_t sensor_height) {
    if (sensor_width == 0 || sensor_height == 0 || zoom < 10 || zoom > 80) {
        return false;
    }

    return x >= 0 && x < sensor_width && y >= 0 && y < sensor_height;
}
