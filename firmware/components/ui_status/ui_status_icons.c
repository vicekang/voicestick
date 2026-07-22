#include "ui_status_icons.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio_feedback.h"
#include "stick_s3_board.h"

#define DICE_CANVAS_WIDTH 127
#define DICE_CANVAS_HEIGHT 138
#define DICE_FRAME_MS 25
#define DICE_CAMERA_DISTANCE 4.25f
#define DICE_PROJECT_SCALE 104.0f
#define DICE_BOUND_X 26.0f
#define DICE_BOUND_Y 28.0f
#define DICE_ACCEL_PX_S2 690.0f
#define DICE_RESTITUTION 0.53f
#define DICE_SETTLE_DELAY_US 430000
#define DICE_COLLISION_SOUND_GAP_US 70000

static const char *TAG = "dice_ui";

typedef struct {
    float x;
    float y;
    float z;
} vec3_t;

typedef struct {
    float w;
    float x;
    float y;
    float z;
} quat_t;

typedef struct {
    vec3_t center;
    vec3_t normal;
    vec3_t u;
    vec3_t v;
    uint8_t value;
} dice_face_t;

typedef struct {
    int x;
    int y;
    float depth;
} projected_t;

static const dice_face_t s_faces[] = {
    {{ 0,  0,  1}, { 0,  0,  1}, { 1, 0,  0}, {0, 1,  0}, 1},
    {{ 0,  0, -1}, { 0,  0, -1}, {-1, 0,  0}, {0, 1,  0}, 6},
    {{ 1,  0,  0}, { 1,  0,  0}, { 0, 0, -1}, {0, 1,  0}, 3},
    {{-1,  0,  0}, {-1,  0,  0}, { 0, 0,  1}, {0, 1,  0}, 4},
    {{ 0,  1,  0}, { 0,  1,  0}, { 1, 0,  0}, {0, 0, -1}, 2},
    {{ 0, -1,  0}, { 0, -1,  0}, { 1, 0,  0}, {0, 0,  1}, 5},
};

static ui_status_icons_t *s_active_icons;
static lv_timer_t *s_animation_timer;
static ui_status_icon_scene_t s_scene = UI_STATUS_ICON_BOOT;
static quat_t s_orientation = {0.91f, 0.18f, -0.31f, 0.19f};
static vec3_t s_angular_velocity;
static vec3_t s_previous_accel = {0, 0, 1};
static float s_offset_x;
static float s_offset_y;
static float s_velocity_x;
static float s_velocity_y;
static float s_rainbow_phase;
static float s_settle_progress = 1.0f;
static int64_t s_last_frame_us;
static int64_t s_last_motion_us;
static uint8_t s_resting_frame_skip;
static uint8_t s_motion_frames;
static uint8_t s_settled_face = 1;
static bool s_have_previous_accel;
static bool s_settled = true;
static int64_t s_last_roll_sound_us;
static uint32_t s_random_state = 0x7a4d39c1u;

static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

static float random_signed(void)
{
    s_random_state = s_random_state * 1664525u + 1013904223u;
    return ((s_random_state >> 8) / 8388607.5f) - 1.0f;
}

static vec3_t vec_add(vec3_t a, vec3_t b)
{
    return (vec3_t){a.x + b.x, a.y + b.y, a.z + b.z};
}

static vec3_t vec_scale(vec3_t value, float scale)
{
    return (vec3_t){value.x * scale, value.y * scale, value.z * scale};
}

static vec3_t vec_cross(vec3_t a, vec3_t b)
{
    return (vec3_t){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

static float vec_length(vec3_t value)
{
    return sqrtf(value.x * value.x + value.y * value.y + value.z * value.z);
}

static vec3_t rotate_vec(quat_t q, vec3_t value)
{
    const vec3_t qv = {q.x, q.y, q.z};
    const vec3_t t = vec_scale(vec_cross(qv, value), 2.0f);
    return vec_add(value, vec_add(vec_scale(t, q.w), vec_cross(qv, t)));
}

static uint8_t dominant_face_value(void)
{
    float best_depth = -2.0f;
    uint8_t best_value = s_settled_face;
    for (size_t i = 0; i < sizeof(s_faces) / sizeof(s_faces[0]); ++i) {
        const float depth = rotate_vec(s_orientation, s_faces[i].normal).z;
        if (depth > best_depth) {
            best_depth = depth;
            best_value = s_faces[i].value;
        }
    }
    return best_value;
}

static void settle_dice(int64_t now_us)
{
    s_settled_face = dominant_face_value();
    s_settled = true;
    s_settle_progress = 0.0f;
    s_velocity_x = 0;
    s_velocity_y = 0;
    s_angular_velocity = (vec3_t){0, 0, 0};
    s_last_motion_us = now_us;
    ESP_LOGI(TAG, "die settled on face %u", s_settled_face);
}

static void wake_dice(vec3_t jerk, vec3_t gyro, int64_t now_us)
{
    if (s_settled) {
        s_settled = false;
        s_settle_progress = 0;
        s_velocity_x += -jerk.x * 520.0f + random_signed() * 42.0f;
        s_velocity_y += jerk.y * 520.0f + random_signed() * 42.0f;
        s_angular_velocity.x += jerk.y * 5.5f + gyro.x * 0.8f + random_signed() * 2.0f;
        s_angular_velocity.y += -jerk.x * 5.5f + gyro.y * 0.8f + random_signed() * 2.0f;
        s_angular_velocity.z += (jerk.x - jerk.y) * 4.0f + gyro.z * 0.8f +
                                  random_signed() * 1.6f;
        ESP_LOGI(TAG, "die released into chamber");
    }
    s_last_motion_us = now_us;
}

static void normalize_orientation(void)
{
    const float length = sqrtf(s_orientation.w * s_orientation.w +
                               s_orientation.x * s_orientation.x +
                               s_orientation.y * s_orientation.y +
                               s_orientation.z * s_orientation.z);
    if (length < 0.0001f) {
        s_orientation = (quat_t){1, 0, 0, 0};
        return;
    }
    const float inv = 1.0f / length;
    s_orientation.w *= inv;
    s_orientation.x *= inv;
    s_orientation.y *= inv;
    s_orientation.z *= inv;
}

static void integrate_orientation(float dt)
{
    const quat_t q = s_orientation;
    const float half_dt = 0.5f * dt;
    s_orientation.w += (-q.x * s_angular_velocity.x -
                         q.y * s_angular_velocity.y -
                         q.z * s_angular_velocity.z) * half_dt;
    s_orientation.x += ( q.w * s_angular_velocity.x +
                         q.y * s_angular_velocity.z -
                         q.z * s_angular_velocity.y) * half_dt;
    s_orientation.y += ( q.w * s_angular_velocity.y -
                         q.x * s_angular_velocity.z +
                         q.z * s_angular_velocity.x) * half_dt;
    s_orientation.z += ( q.w * s_angular_velocity.z +
                         q.x * s_angular_velocity.y -
                         q.y * s_angular_velocity.x) * half_dt;
    normalize_orientation();
}

static projected_t project(vec3_t value)
{
    const float denominator = clampf(DICE_CAMERA_DISTANCE - value.z, 2.7f, 5.8f);
    const float scale = DICE_PROJECT_SCALE / denominator;
    return (projected_t){
        .x = (int)lroundf((DICE_CANVAS_WIDTH * 0.5f) + s_offset_x + value.x * scale),
        .y = (int)lroundf(64.0f + s_offset_y - value.y * scale),
        .depth = value.z,
    };
}

static uint32_t shade_hex(uint32_t rgb, float brightness)
{
    const uint8_t r = (uint8_t)clampf(((rgb >> 16) & 0xff) * brightness, 0, 255);
    const uint8_t g = (uint8_t)clampf(((rgb >> 8) & 0xff) * brightness, 0, 255);
    const uint8_t b = (uint8_t)clampf((rgb & 0xff) * brightness, 0, 255);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void draw_rect(lv_layer_t *layer, lv_area_t area, lv_color_t color,
                      lv_opa_t opacity, int radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = opacity;
    dsc.radius = radius;
    lv_draw_rect(layer, &dsc, &area);
}

static void draw_triangle(lv_layer_t *layer, projected_t a, projected_t b,
                          projected_t c, lv_color_t color)
{
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.p[0] = (lv_point_precise_t){a.x, a.y};
    dsc.p[1] = (lv_point_precise_t){b.x, b.y};
    dsc.p[2] = (lv_point_precise_t){c.x, c.y};
    lv_draw_triangle(layer, &dsc);
}

static void draw_line(lv_layer_t *layer, projected_t a, projected_t b,
                      lv_color_t color, int width)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1 = (lv_point_precise_t){a.x, a.y};
    dsc.p2 = (lv_point_precise_t){b.x, b.y};
    dsc.color = color;
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = true;
    dsc.round_end = true;
    lv_draw_line(layer, &dsc);
}

static size_t pip_positions(uint8_t value, float points[6][2])
{
    static const float left = -0.48f;
    static const float right = 0.48f;
    static const float top = 0.48f;
    static const float bottom = -0.48f;

    size_t count = 0;
    if (value == 1 || value == 3 || value == 5) {
        points[count][0] = 0;
        points[count++][1] = 0;
    }
    if (value >= 2) {
        points[count][0] = left;
        points[count++][1] = top;
        points[count][0] = right;
        points[count++][1] = bottom;
    }
    if (value >= 4) {
        points[count][0] = right;
        points[count++][1] = top;
        points[count][0] = left;
        points[count++][1] = bottom;
    }
    if (value == 6) {
        points[count][0] = left;
        points[count++][1] = 0;
        points[count][0] = right;
        points[count++][1] = 0;
    }
    return count;
}

static void draw_chamber(lv_layer_t *layer)
{
    lv_area_t outer = {1, 1, DICE_CANVAS_WIDTH - 2, DICE_CANVAS_HEIGHT - 2};
    lv_area_t inner = {4, 4, DICE_CANVAS_WIDTH - 5, DICE_CANVAS_HEIGHT - 5};
    lv_area_t floor = {7, DICE_CANVAS_HEIGHT - 20,
                       DICE_CANVAS_WIDTH - 8, DICE_CANVAS_HEIGHT - 8};
    draw_rect(layer, outer, lv_color_hex(0x302d28), LV_OPA_COVER, 15);
    draw_rect(layer, inner, lv_color_hex(0x0c0d0d), LV_OPA_COVER, 12);
    draw_rect(layer, floor, lv_color_hex(0x171715), LV_OPA_80, 7);
}

static void draw_flat_face(lv_layer_t *layer, bool recording)
{
    const float progress = clampf(s_settle_progress, 0, 1);
    const float reveal = 1.0f - powf(1.0f - progress, 4.0f);
    const int side = 80 + (int)lroundf(reveal * 16.0f);
    const int center_x = DICE_CANVAS_WIDTH / 2;
    const int center_y = 64;
    const int half = side / 2;

    lv_area_t shadow = {
        center_x - half + 5,
        center_y - half + 9,
        center_x + half + 7,
        center_y + half + 11,
    };
    draw_rect(layer, shadow,
              recording ? lv_color_hex(0x39275f) : lv_color_hex(0x000000),
              recording ? LV_OPA_40 : LV_OPA_60, 15);

    lv_area_t rim = {
        center_x - half - 2, center_y - half - 2,
        center_x + half + 2, center_y + half + 2,
    };
    lv_area_t face = {
        center_x - half, center_y - half,
        center_x + half, center_y + half,
    };

    if (recording) {
        const uint16_t base_hue = (uint16_t)fmodf(s_rainbow_phase, 360.0f);
        draw_rect(layer, rim, lv_color_hsv_to_rgb((base_hue + 205) % 360, 45, 100),
                  LV_OPA_COVER, 15);
        draw_rect(layer, face, lv_color_hsv_to_rgb(base_hue, 78, 96),
                  LV_OPA_COVER, 13);

        const int band_width = side / 4;
        for (int band = 0; band < 4; ++band) {
            const int x = face.x1 + band * band_width - 3;
            lv_area_t strip = {x, face.y1 + 3, x + band_width + 7, face.y2 - 3};
            const uint16_t hue = (base_hue + band * 63 + 32) % 360;
            draw_rect(layer, strip, lv_color_hsv_to_rgb(hue, 82, 100), LV_OPA_50, 10);
        }
    } else {
        draw_rect(layer, rim, lv_color_hex(0x9a9285), LV_OPA_COVER, 15);
        draw_rect(layer, face, lv_color_hex(0xf3eadb), LV_OPA_COVER, 13);
        lv_area_t highlight = {face.x1 + 7, face.y1 + 6, face.x2 - 7, face.y1 + 11};
        draw_rect(layer, highlight, lv_color_hex(0xfffbf1), LV_OPA_70, 4);
    }

    float pips[6][2];
    const size_t pip_count = pip_positions(s_settled_face, pips);
    const float pip_span = side * 0.52f;
    const int radius = side >= 92 ? 7 : 6;
    for (size_t i = 0; i < pip_count; ++i) {
        const int x = center_x + (int)lroundf(pips[i][0] * pip_span);
        const int y = center_y - (int)lroundf(pips[i][1] * pip_span);
        lv_area_t pip_shadow = {x - radius - 1, y - radius,
                                x + radius + 1, y + radius + 2};
        lv_area_t pip = {x - radius, y - radius, x + radius, y + radius};
        draw_rect(layer, pip_shadow,
                  recording ? lv_color_hex(0x21162e) : lv_color_hex(0x746d63),
                  recording ? LV_OPA_50 : LV_OPA_40, LV_RADIUS_CIRCLE);
        draw_rect(layer, pip,
                  recording ? lv_color_hex(0xfffbf4) : lv_color_hex(0x171717),
                  LV_OPA_COVER, LV_RADIUS_CIRCLE);
    }
}

static void draw_face(lv_layer_t *layer, const dice_face_t *face, int face_index,
                      bool recording)
{
    const vec3_t center = rotate_vec(s_orientation, face->center);
    const vec3_t normal = rotate_vec(s_orientation, face->normal);
    const vec3_t u = rotate_vec(s_orientation, face->u);
    const vec3_t v = rotate_vec(s_orientation, face->v);

    const projected_t corners[4] = {
        project(vec_add(center, vec_add(vec_scale(u, -1), vec_scale(v, -1)))),
        project(vec_add(center, vec_add(vec_scale(u,  1), vec_scale(v, -1)))),
        project(vec_add(center, vec_add(vec_scale(u,  1), vec_scale(v,  1)))),
        project(vec_add(center, vec_add(vec_scale(u, -1), vec_scale(v,  1)))),
    };

    lv_color_t face_color_a;
    lv_color_t face_color_b;
    lv_color_t edge_color;
    lv_color_t pip_color;
    lv_color_t pip_edge;
    if (recording) {
        const uint16_t hue_a = (uint16_t)fmodf(s_rainbow_phase + face_index * 57.0f, 360.0f);
        const uint16_t hue_b = (uint16_t)fmodf(s_rainbow_phase + face_index * 57.0f + 34.0f, 360.0f);
        face_color_a = lv_color_hsv_to_rgb(hue_a, 82, 100);
        face_color_b = lv_color_hsv_to_rgb(hue_b, 86, 88);
        edge_color = lv_color_hsv_to_rgb((hue_a + 185) % 360, 35, 100);
        pip_color = lv_color_white();
        pip_edge = lv_color_hex(0x1a1130);
    } else {
        const float brightness = 0.72f + clampf(normal.z, 0, 1) * 0.28f;
        face_color_a = lv_color_hex(shade_hex(0xfff7e8, brightness));
        face_color_b = lv_color_hex(shade_hex(0xe7edf7, brightness * 0.97f));
        edge_color = lv_color_hex(0x8290a8);
        pip_color = lv_color_hex(0x111827);
        pip_edge = lv_color_hex(0xffffff);
    }

    draw_triangle(layer, corners[0], corners[1], corners[2], face_color_a);
    draw_triangle(layer, corners[0], corners[2], corners[3], face_color_b);
    for (int edge = 0; edge < 4; ++edge) {
        draw_line(layer, corners[edge], corners[(edge + 1) % 4], edge_color, recording ? 3 : 2);
    }

    float pips[6][2];
    const size_t count = pip_positions(face->value, pips);
    for (size_t i = 0; i < count; ++i) {
        vec3_t pip = vec_add(center,
                             vec_add(vec_scale(u, pips[i][0]),
                                     vec_scale(v, pips[i][1])));
        pip = vec_add(pip, vec_scale(normal, 0.025f));
        const projected_t point = project(pip);
        const int radius = recording ? 5 : 4;
        lv_area_t outer = {
            point.x - radius - 1, point.y - radius - 1,
            point.x + radius + 1, point.y + radius + 1,
        };
        lv_area_t inner = {
            point.x - radius, point.y - radius,
            point.x + radius, point.y + radius,
        };
        draw_rect(layer, outer, pip_edge, recording ? LV_OPA_50 : LV_OPA_20,
                  LV_RADIUS_CIRCLE);
        draw_rect(layer, inner, pip_color, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    }
}

static void draw_dice(void)
{
    if (!s_active_icons || !s_active_icons->root) {
        return;
    }

    const bool recording = s_scene == UI_STATUS_ICON_RECORDING;
    lv_canvas_fill_bg(s_active_icons->root, lv_color_hex(0x080909), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(s_active_icons->root, &layer);
    draw_chamber(&layer);

    if (s_settled || recording) {
        draw_flat_face(&layer, recording);
        lv_canvas_finish_layer(s_active_icons->root, &layer);
        return;
    }

    const int shadow_width = 52 + (int)(fabsf(s_offset_y) * 0.35f);
    const int shadow_x = (DICE_CANVAS_WIDTH - shadow_width) / 2 + (int)s_offset_x + 4;
    const int shadow_y = 96 + (int)s_offset_y;
    lv_area_t shadow = {shadow_x, shadow_y, shadow_x + shadow_width, shadow_y + 11};
    draw_rect(&layer, shadow, lv_color_hex(0x000000), LV_OPA_50, LV_RADIUS_CIRCLE);

    int visible[6];
    float depth[6];
    int visible_count = 0;
    for (int i = 0; i < 6; ++i) {
        const vec3_t normal = rotate_vec(s_orientation, s_faces[i].normal);
        if (normal.z > 0.01f) {
            visible[visible_count] = i;
            depth[visible_count] = rotate_vec(s_orientation, s_faces[i].center).z;
            visible_count++;
        }
    }
    for (int i = 0; i < visible_count - 1; ++i) {
        for (int j = i + 1; j < visible_count; ++j) {
            if (depth[i] > depth[j]) {
                const float depth_tmp = depth[i];
                depth[i] = depth[j];
                depth[j] = depth_tmp;
                const int face_tmp = visible[i];
                visible[i] = visible[j];
                visible[j] = face_tmp;
            }
        }
    }
    for (int i = 0; i < visible_count; ++i) {
        draw_face(&layer, &s_faces[visible[i]], visible[i], false);
    }

    lv_canvas_finish_layer(s_active_icons->root, &layer);
}

static void add_collision_spin(float impact, float normal_x, float normal_y)
{
    if (normal_x != 0) {
        s_angular_velocity.y += normal_x * impact * 0.020f;
        s_angular_velocity.z -= normal_x * s_velocity_y * 0.013f;
    }
    if (normal_y != 0) {
        s_angular_velocity.x -= normal_y * impact * 0.020f;
        s_angular_velocity.z += normal_y * s_velocity_x * 0.013f;
    }
    s_angular_velocity.x += random_signed() * impact * 0.004f;
    s_angular_velocity.y += random_signed() * impact * 0.004f;
    s_angular_velocity.z += random_signed() * impact * 0.003f;
}

static void update_physics(float dt)
{
    stick_s3_imu_sample_t sample;
    if (stick_s3_imu_read(&sample) != ESP_OK) {
        return;
    }

    /* Map the sensor frame to the portrait display frame. Accelerometer values
     * are used as apparent force inside the moving chamber, not as a direct
     * rotation target for the die. */
    const vec3_t accel = {
        sample.accel_y_g,
        -sample.accel_x_g,
        sample.accel_z_g,
    };
    const vec3_t gyro = {
        sample.gyro_y_rad_s,
        -sample.gyro_x_rad_s,
        sample.gyro_z_rad_s,
    };
    if (!s_have_previous_accel) {
        s_previous_accel = accel;
        s_have_previous_accel = true;
        s_last_motion_us = esp_timer_get_time();
        return;
    }

    const vec3_t jerk = {
        accel.x - s_previous_accel.x,
        accel.y - s_previous_accel.y,
        accel.z - s_previous_accel.z,
    };
    s_previous_accel = accel;

    const int64_t now_us = esp_timer_get_time();
    const float jerk_strength = vec_length(jerk);
    const float gyro_strength = vec_length(gyro);
    const bool raw_motion = jerk_strength > 0.060f || gyro_strength > 0.34f;
    if (raw_motion) {
        if (s_motion_frames < 3) {
            s_motion_frames++;
        }
    } else {
        s_motion_frames = 0;
    }
    /* A sharp impulse wakes immediately; ordinary sensor noise must persist
     * for two frames before it is treated as a hand movement. */
    const bool hand_motion = jerk_strength > 0.16f || s_motion_frames >= 2;
    if (hand_motion) {
        wake_dice(jerk, gyro, now_us);
    }

    if (s_settled) {
        return;
    }

    /* The die lags behind the moving enclosure. Persistent acceleration also
     * acts as gravity when the display is tilted. */
    s_velocity_x += (-accel.x * DICE_ACCEL_PX_S2 - jerk.x * 260.0f) * dt;
    s_velocity_y += ( accel.y * DICE_ACCEL_PX_S2 + jerk.y * 260.0f) * dt;
    if (hand_motion) {
        s_angular_velocity.x += (jerk.y * 4.8f + gyro.x * 0.45f) * dt * 10.0f;
        s_angular_velocity.y += (-jerk.x * 4.8f + gyro.y * 0.45f) * dt * 10.0f;
        s_angular_velocity.z += ((jerk.x - jerk.y) * 3.2f + gyro.z * 0.4f) * dt * 10.0f;
    }

    const float air_drag = expf(-dt * 0.92f);
    s_velocity_x *= air_drag;
    s_velocity_y *= air_drag;
    s_offset_x += s_velocity_x * dt;
    s_offset_y += s_velocity_y * dt;

    float max_impact = 0;
    if (s_offset_x < -DICE_BOUND_X) {
        s_offset_x = -DICE_BOUND_X;
        if (s_velocity_x < 0) {
            const float impact = -s_velocity_x;
            max_impact = fmaxf(max_impact, impact);
            add_collision_spin(impact, 1, 0);
            s_velocity_x = impact > 34.0f ? impact * DICE_RESTITUTION : 0;
            s_velocity_y *= 0.86f;
        }
    } else if (s_offset_x > DICE_BOUND_X) {
        s_offset_x = DICE_BOUND_X;
        if (s_velocity_x > 0) {
            const float impact = s_velocity_x;
            max_impact = fmaxf(max_impact, impact);
            add_collision_spin(impact, -1, 0);
            s_velocity_x = impact > 34.0f ? -impact * DICE_RESTITUTION : 0;
            s_velocity_y *= 0.86f;
        }
    }

    if (s_offset_y < -DICE_BOUND_Y) {
        s_offset_y = -DICE_BOUND_Y;
        if (s_velocity_y < 0) {
            const float impact = -s_velocity_y;
            max_impact = fmaxf(max_impact, impact);
            add_collision_spin(impact, 0, 1);
            s_velocity_y = impact > 34.0f ? impact * DICE_RESTITUTION : 0;
            s_velocity_x *= 0.86f;
        }
    } else if (s_offset_y > DICE_BOUND_Y) {
        s_offset_y = DICE_BOUND_Y;
        if (s_velocity_y > 0) {
            const float impact = s_velocity_y;
            max_impact = fmaxf(max_impact, impact);
            add_collision_spin(impact, 0, -1);
            s_velocity_y = impact > 34.0f ? -impact * DICE_RESTITUTION : 0;
            s_velocity_x *= 0.86f;
        }
    }

    if (max_impact > 52.0f) {
        if (now_us - s_last_roll_sound_us > DICE_COLLISION_SOUND_GAP_US) {
            const float intensity = clampf((max_impact - 38.0f) / 240.0f, 0.30f, 1.0f);
            s_last_roll_sound_us = now_us;
            audio_feedback_play_roll(intensity);
            ESP_LOGI(TAG, "chamber impact speed=%.1f intensity=%.2f", max_impact, intensity);
        }
    }

    const float angular_drag = expf(-dt * 1.45f);
    s_angular_velocity = vec_scale(s_angular_velocity, angular_drag);
    s_angular_velocity.x = clampf(s_angular_velocity.x, -18, 18);
    s_angular_velocity.y = clampf(s_angular_velocity.y, -18, 18);
    s_angular_velocity.z = clampf(s_angular_velocity.z, -18, 18);
    integrate_orientation(dt);

    const int64_t quiet_us = now_us - s_last_motion_us;
    if (!hand_motion && quiet_us > 120000) {
        const float contact_drag = expf(-dt * 7.5f);
        s_velocity_x *= contact_drag;
        s_velocity_y *= contact_drag;
        s_angular_velocity = vec_scale(s_angular_velocity, contact_drag);
    }
    if (!hand_motion && quiet_us > DICE_SETTLE_DELAY_US) {
        settle_dice(now_us);
    }
}

static void animation_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_active_icons) {
        return;
    }

    if (s_scene == UI_STATUS_ICON_RESTING && (++s_resting_frame_skip % 4) != 0) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    float dt = s_last_frame_us > 0 ? (now_us - s_last_frame_us) / 1000000.0f : 0.025f;
    s_last_frame_us = now_us;
    dt = clampf(dt, 0.008f, 0.1f);

    const bool recording = s_scene == UI_STATUS_ICON_RECORDING;
    if (recording) {
        s_rainbow_phase = fmodf(s_rainbow_phase + dt * 155.0f, 360.0f);
    } else {
        update_physics(dt);
        if (s_settled && s_settle_progress < 1.0f) {
            s_settle_progress = fminf(1.0f, s_settle_progress + dt / 0.20f);
        }
    }
    draw_dice();
}

void ui_status_icons_create(ui_status_icons_t *icons, lv_obj_t *screen)
{
    memset(icons, 0, sizeof(*icons));

    icons->buffer = heap_caps_malloc(DICE_CANVAS_WIDTH * DICE_CANVAS_HEIGHT * sizeof(lv_color16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!icons->buffer) {
        icons->buffer = heap_caps_malloc(DICE_CANVAS_WIDTH * DICE_CANVAS_HEIGHT * sizeof(lv_color16_t),
                                        MALLOC_CAP_8BIT);
    }
    if (!icons->buffer) {
        ESP_LOGE(TAG, "dice canvas allocation failed");
        return;
    }

    icons->root = lv_canvas_create(screen);
    lv_canvas_set_buffer(icons->root, icons->buffer, DICE_CANVAS_WIDTH,
                         DICE_CANVAS_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(icons->root, LV_ALIGN_TOP_MID, 0, 25);
    lv_obj_remove_flag(icons->root, LV_OBJ_FLAG_SCROLLABLE);
    s_active_icons = icons;
    draw_dice();

    s_animation_timer = lv_timer_create(animation_timer_cb, DICE_FRAME_MS, NULL);
    ESP_LOGI(TAG, "procedural IMU dice ready");
}

void ui_status_icons_stop_anim(ui_status_icons_t *icons)
{
    (void)icons;
}

void ui_status_icons_apply(ui_status_icons_t *icons, ui_status_icon_scene_t scene)
{
    (void)icons;
    s_scene = scene;
    s_last_frame_us = esp_timer_get_time();
    if (scene == UI_STATUS_ICON_RECORDING && !s_settled) {
        s_settled_face = dominant_face_value();
        s_settled = true;
        s_settle_progress = 1.0f;
        s_velocity_x = 0;
        s_velocity_y = 0;
        s_angular_velocity = (vec3_t){0, 0, 0};
    }
    draw_dice();
}

void ui_status_icons_start_anim(ui_status_icons_t *icons, ui_status_icon_scene_t scene)
{
    (void)icons;
    (void)scene;
    if (s_animation_timer) {
        lv_timer_resume(s_animation_timer);
    }
}
