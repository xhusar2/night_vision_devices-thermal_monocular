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
    "\"camera_state_authoritative\": %u, " \
    "\"camera_state_status\": %d, " \
    "\"persistence_authoritative\": %u, " \
    "\"persistence_status\": %d " \
    "}"

static inline bool control_point_zoom_is_valid(int x, int y, int zoom,
                                                uint16_t sensor_width,
                                                uint16_t sensor_height) {
    if (sensor_width == 0 || sensor_height == 0 || zoom < 11 || zoom > 80) {
        return false;
    }

    return x >= 0 && x < sensor_width && y >= 0 && y < sensor_height;
}

static inline bool control_percent_is_valid(int value) {
    return value >= 0 && value <= 100;
}

static inline bool control_edge_gear_is_valid(int value) {
    return value >= 0 && value <= 2;
}

static inline bool control_pseudo_color_is_valid(int value) {
    return value == 0 || value == 9;
}

static inline bool control_scene_mode_is_valid(int value) {
    return (value >= 0 && value <= 5) || value == 9;
}

static inline bool control_flip_mode_is_valid(int value) {
    return value >= 0 && value <= 3;
}

static inline int control_first_error(int current_result, int next_result) {
    return current_result == 0 ? next_result : current_result;
}

static inline bool control_rollback_required(bool device_mutated) {
    return device_mutated;
}

static inline bool control_commit_is_authoritative(int commit_result) {
    return commit_result == 0;
}

typedef enum {
    CONTROL_BUTTON_NONE,
    CONTROL_BUTTON_SHORT_PRESS,
    CONTROL_BUTTON_LONG_PRESS,
} control_button_action_t;

typedef struct {
    bool observed_pressed;
    bool accepted_pressed;
    bool long_press_handled;
    int64_t observed_since;
    int64_t pressed_since;
} control_button_state_t;

static inline void control_button_init(control_button_state_t *state,
                                       bool pressed, int64_t now) {
    *state = (control_button_state_t) {
        .observed_pressed = pressed,
        .accepted_pressed = pressed,
        .observed_since = now,
        .pressed_since = now,
    };
}

static inline control_button_action_t control_button_sample(
        control_button_state_t *state, bool pressed, int64_t now,
        int64_t debounce_us, int64_t long_press_us) {
    if (pressed != state->observed_pressed) {
        state->observed_pressed = pressed;
        state->observed_since = now;
    }

    if (state->observed_pressed != state->accepted_pressed &&
        now - state->observed_since >= debounce_us) {
        state->accepted_pressed = state->observed_pressed;
        if (state->accepted_pressed) {
            state->pressed_since = state->observed_since;
            state->long_press_handled = false;
        } else if (!state->long_press_handled) {
            return CONTROL_BUTTON_SHORT_PRESS;
        }
    }

    if (state->accepted_pressed && !state->long_press_handled &&
        now - state->pressed_since >= long_press_us) {
        state->long_press_handled = true;
        return CONTROL_BUTTON_LONG_PRESS;
    }

    return CONTROL_BUTTON_NONE;
}

typedef int (*control_apply_preset_fn)(void *context, uint8_t preset);
typedef int (*control_apply_crosshair_fn)(void *context, bool enabled);

static inline int control_apply_preset_transaction(
        void *context, uint8_t preset, bool crosshair_enabled,
        control_apply_preset_fn apply_preset,
        control_apply_crosshair_fn apply_crosshair) {
    int result = apply_preset(context, preset);
    if (result != 0) {
        return result;
    }
    return apply_crosshair(context, crosshair_enabled);
}

typedef int (*control_operation_fn)(void *context);

typedef struct {
    int result;
    int rollback_result;
    bool authoritative;
} control_transaction_result_t;

static inline int control_retry_operation(void *context,
                                          control_operation_fn operation,
                                          unsigned attempts) {
    int result = -1;
    for (unsigned attempt = 0; attempt < attempts; attempt++) {
        result = operation(context);
        if (result == 0) {
            break;
        }
    }
    return result;
}

static inline control_transaction_result_t control_run_transaction(
        void *context, control_operation_fn apply,
        control_operation_fn persist, control_operation_fn rollback,
        unsigned persist_attempts) {
    control_transaction_result_t result = {0, 0, true};
    result.result = apply(context);
    if (result.result == 0) {
        result.result = control_retry_operation(context, persist, persist_attempts);
    }
    if (result.result != 0) {
        result.rollback_result = rollback(context);
        result.authoritative = result.rollback_result == 0;
    }
    return result;
}
