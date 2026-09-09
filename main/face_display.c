/**
 * face_display.c — animated face for the 128x64 monochrome OLED.
 *
 * Own low-priority FreeRTOS task at ~12 fps.  When enabled, it clears
 * and redraws the face every frame, reflecting the current app state:
 *
 *   Idle       — SLEEPING: closed eyes, gentle smile, floating "z z"
 *   Listening  — AWAKE: wide eyes, raised eyebrows, "o" mouth, blinks
 *   Thinking   — CONNECTING: pupils glance around, asymmetric brows
 *   Speaking   — TALKING: mouth opens/closes in sync with audio
 *
 * Drawing is done directly into the shared frame buffer (display_fb())
 * and flushed via display_flush_now().  When disabled, the task sleeps
 * and the info display (display.c) owns the screen.
 */
#include "face_display.h"
#include "app.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "face";

#define DISP_W  128
#define DISP_H  64
#define FB_SIZE (DISP_W * DISP_H / 8)
#define FRAME_MS 80          /* ~12.5 fps */

/* Face geometry — elliptical eyes, centered on the 128×64 display */
#define EYE_RX_OPEN    10
#define EYE_RY_OPEN     7
#define EYE_RY_WIDE     9   /* listening: taller eyes */
#define EYE_CY         18
#define LEFT_CX        42
#define RIGHT_CX       86
#define PUPIL_RX        3
#define PUPIL_RY        3
#define MOUTH_CX       64
#define MOUTH_CY       48
#define MOUTH_RX_OPEN  14   /* speaking: max mouth half-width */

/* Internal face state */
typedef enum { FACE_IDLE = 0, FACE_LISTEN, FACE_THINK, FACE_SPEAK } face_state_t;

static volatile bool      s_enabled;
static volatile face_state_t s_face_state = FACE_IDLE;
static int  s_frame;
static int  s_blink_cnt;          /* frames since last blink / in blink */
static bool s_blinking;
static int  s_think_cnt;          /* frames since last pupil shift */
static int  s_think_dir;          /* -1 left, 0 center, 1 right */
static int  s_mouth_h = 1;        /* smoothed mouth height for SPEAK */
static int  s_z_pos;              /* sleeping "z" animation offset */

/* ── pixel primitives on the raw buffer ────────────────────────── */
static inline void set_px(int x, int y)
{
    if (x < 0 || x >= DISP_W || y < 0 || y >= DISP_H) return;
    display_fb()[y / 8 * DISP_W + x] |= 1 << (y % 8);
}

static inline void clr_px(int x, int y)
{
    if (x < 0 || x >= DISP_W || y < 0 || y >= DISP_H) return;
    display_fb()[y / 8 * DISP_W + x] &= ~(1 << (y % 8));
}

static void fill_rect(int x, int y, int w, int h)
{
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            set_px(x + dx, y + dy);
}

/* filled ellipse — integer-only, O(rx*ry) */
static void fill_ellipse(int cx, int cy, int rx, int ry)
{
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++)
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry)
                set_px(cx + dx, cy + dy);
}

/* clear an ellipse (cut a hole — used for pupils) */
static void clr_ellipse(int cx, int cy, int rx, int ry)
{
    for (int dy = -ry; dy <= ry; dy++)
        for (int dx = -rx; dx <= rx; dx++)
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry)
                clr_px(cx + dx, cy + dy);
}

/* thick line (Bresenham + square brush) */
static void draw_thick_line(int x0, int y0, int x1, int y1, int t)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int h = t / 2;
    while (true) {
        for (int ty = -h; ty <= h; ty++)
            for (int tx = -h; tx <= h; tx++)
                set_px(x0 + tx, y0 + ty);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* ── face drawing ────────────────────────────────────────────────── */

/* tiny "z" for sleeping indicator — 4×4 pixels */
static void draw_z(int x, int y)
{
    set_px(x,     y);     set_px(x + 1, y);     set_px(x + 2, y);
    set_px(x + 2, y + 1);
    set_px(x + 1, y + 2);
    set_px(x,     y + 3); set_px(x + 1, y + 3); set_px(x + 2, y + 3);
}

static void draw_eye(int cx, int rx, int ry, int pupil_dx, int pupil_dy)
{
    fill_ellipse(cx, EYE_CY, rx, ry);
    if (ry > 4)   /* no pupil when blinking (ry ≤ 2) */
        clr_ellipse(cx + pupil_dx, EYE_CY + pupil_dy, PUPIL_RX, PUPIL_RY);
}

static void draw_eyebrow(int cx, int inner_dy, int outer_dy)
{
    /* eyebrow spans 24 px, centred on the eye */
    int x_in  = cx - 12;
    int x_out = cx + 12;
    draw_thick_line(x_in,  EYE_CY + inner_dy,
                    x_out, EYE_CY + outer_dy, 2);
}

static void draw_mouth_shape(face_state_t state, int speak_h)
{
    if (state == FACE_SPEAK) {
        /* open ellipse whose height tracks audio amplitude */
        int ry = speak_h / 2;
        if (ry < 1) ry = 1;
        fill_ellipse(MOUTH_CX, MOUTH_CY, MOUTH_RX_OPEN, ry);
    } else if (state == FACE_LISTEN) {
        /* small round "o" — attentive */
        fill_ellipse(MOUTH_CX, MOUTH_CY, 5, 5);
    } else if (state == FACE_THINK) {
        /* flat slightly-off-centre line — pursed */
        draw_thick_line(MOUTH_CX - 10, MOUTH_CY,
                        MOUTH_CX + 10, MOUTH_CY + 1, 2);
    } else {
        /* idle: gentle smile — wide thin ellipse, taller at edges */
        fill_ellipse(MOUTH_CX, MOUTH_CY, 12, 2);
        /* lift the corners slightly for a smile */
        set_px(MOUTH_CX - 12, MOUTH_CY - 1);
        set_px(MOUTH_CX + 12, MOUTH_CY - 1);
    }
}

static void draw_face(void)
{
    memset(display_fb(), 0, FB_SIZE);

    int eye_rx = EYE_RX_OPEN;
    int eye_ry = EYE_RY_OPEN;
    int pupil_dx = 0, pupil_dy = 0;
    int brow_inner = -6, brow_outer = -6;   /* relative to EYE_CY */

    switch (s_face_state) {
    case FACE_IDLE:
        /* SLEEPING: closed eyes (thin lines), relaxed brows, gentle smile,
           and floating "z z" marks in the top-right corner. */
        eye_ry = 1;               /* closed */
        brow_inner = -5; brow_outer = -4;   /* relaxed, slightly lowered */
        break;

    case FACE_LISTEN:
        /* AWAKE / LISTENING: wide eyes, raised brows, "o" mouth.
           Blink occasionally. */
        eye_ry = s_blinking ? 1 : EYE_RY_WIDE;
        brow_inner = -9; brow_outer = -10;
        break;

    case FACE_THINK:
        /* THINKING / CONNECTING: eyes open, pupils shift, asymmetric brows */
        eye_ry = s_blinking ? 1 : EYE_RY_OPEN;
        pupil_dx = s_think_dir * 4;
        pupil_dy = -1;
        brow_inner = -8; brow_outer = -4;
        break;

    case FACE_SPEAK: {
        /* TALKING: eyes open, mouth tracks real audio amplitude */
        eye_ry = EYE_RY_OPEN;
        brow_inner = -6; brow_outer = -5;
        int lvl = audio_play_level();
        int target;
        if      (lvl > 6000) target = 16;
        else if (lvl > 3000) target = 12;
        else if (lvl > 1200) target = 8;
        else if (lvl >  300) target = 4;
        else                 target = 1;
        s_mouth_h += (target - s_mouth_h) / 2;
        if (s_mouth_h < 1) s_mouth_h = 1;
        break;
    }
    }

    /* sleeping "z" marks — only when idle */
    if (s_face_state == FACE_IDLE) {
        int z1y = 24 - (s_z_pos % 28);
        int z2y = 24 - ((s_z_pos + 14) % 28);
        if (z1y >= 2 && z1y < 24) draw_z(108, z1y);
        if (z2y >= 2 && z2y < 24) draw_z(113, z2y);
    }

    /* eyebrows — angled, drawn before eyes so eye overlaps look clean */
    draw_eyebrow(LEFT_CX,  brow_inner, brow_outer);
    draw_eyebrow(RIGHT_CX, brow_inner, brow_outer);

    draw_eye(LEFT_CX,  eye_rx, eye_ry, pupil_dx, pupil_dy);
    draw_eye(RIGHT_CX, eye_rx, eye_ry, pupil_dx, pupil_dy);

    draw_mouth_shape(s_face_state, s_mouth_h);
}

/* ── animation task ──────────────────────────────────────────────── */
static void face_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
        if (!s_enabled) continue;

        draw_face();
        display_flush_now();
        s_frame++;

        /* blink timer — only when awake (LISTEN / THINK), not sleeping */
        if (s_face_state == FACE_LISTEN || s_face_state == FACE_THINK) {
            if (s_blinking) {
                if (++s_blink_cnt >= 2) { s_blinking = false; s_blink_cnt = 0; }
            } else {
                /* next blink in 3–6 seconds */
                if (++s_blink_cnt >= 36 + (s_frame % 38)) {
                    s_blinking = true; s_blink_cnt = 0;
                }
            }
        }

        /* sleeping "z" animation — only when idle */
        if (s_face_state == FACE_IDLE)
            s_z_pos++;

        /* thinking pupil shift (~1 second per direction) */
        if (s_face_state == FACE_THINK) {
            if (++s_think_cnt >= 12) {
                s_think_cnt = 0;
                s_think_dir++;
                if (s_think_dir > 1) s_think_dir = -1;
            }
        }
    }
}

/* ── public API ──────────────────────────────────────────────────── */
void face_display_init(void)
{
    s_enabled = false;
    s_face_state = FACE_IDLE;
    s_frame = 0;
    s_blink_cnt = 0;
    s_think_cnt = 0;
    s_think_dir = 0;
    s_z_pos = 0;
    xTaskCreate(face_task, "face", 4096, NULL, 1, NULL);
    ESP_LOGI(TAG, "face display task ready (disabled by default)");
}

void face_display_set_enabled(bool on)
{
    s_enabled = on;
    ESP_LOGI(TAG, "face mode %s", on ? "ON" : "OFF");
}

bool face_display_is_enabled(void) { return s_enabled; }

void face_display_set_state(app_state_t s)
{
    face_state_t prev = s_face_state;
    switch (s) {
    case APP_STATE_IDLE:        s_face_state = FACE_IDLE;    break;
    case APP_STATE_CONNECTING:  s_face_state = FACE_THINK;  break;
    case APP_STATE_LISTENING:   s_face_state = FACE_LISTEN; break;
    case APP_STATE_SPEAKING:    s_face_state = FACE_SPEAK;  break;
    default:                    s_face_state = FACE_IDLE;    break;
    }
    if (prev != s_face_state) {
        s_blink_cnt = 0;
        s_blinking = false;
        s_think_cnt = 0;
        s_think_dir = 0;
        s_mouth_h = 1;
    }
}