/**
 * display.c — SSD1309 128x64 I2C OLED status screens.
 *
 * The SSD1309 is command-compatible with the SSD1306, so the panel is driven
 * with the in-tree esp_lcd SSD1306 panel driver over the I2C bus (GPIO5/6).
 */
#include "app.h"
#include "board.h"
#include "font.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

static const char *TAG = "display";

#define DISPLAY_W 128
#define DISPLAY_H 64
#define LINE_H 15         /* font cell height (font.h) — spans 2 RAM pages */
#define MAX_LINES (DISPLAY_H / LINE_H)

static i2c_master_bus_handle_t s_bus;
static esp_lcd_panel_handle_t s_panel;
/* Frame buffer in host byte order: bit0..7 of page byte = rows y+0..y+7 */
static uint8_t s_fb[DISPLAY_W * DISPLAY_H / 8];
static int s_cursor_x, s_cursor_y;

static inline void put_pixel(int x, int y)
{
    if (x < 0 || x >= DISPLAY_W || y < 0 || y >= DISPLAY_H) return;
    s_fb[y / 8 * DISPLAY_W + x] |= 1 << (y % 8);
}

static void flush(void)
{
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_W, DISPLAY_H, s_fb);
}

/* Expose buffer + flush for the face display module */
uint8_t *display_fb(void) { return s_fb; }
void display_flush_now(void) { flush(); }

static void put_char(char c, bool invert)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    const font_glyph_t *g = &FONT_GLYPHS[c - FONT_FIRST];
    int x = s_cursor_x;
    for (int cx = 0; cx < g->w && x < DISPLAY_W; cx++, x++) {
        for (int row = 0; row < LINE_H; row++) {
            int y = s_cursor_y + row;
            if (y >= DISPLAY_H) break;
            uint8_t bits = g->col[row >> 3][cx];   /* one byte per RAM page */
            if (invert) bits = ~bits;
            uint8_t *b = &s_fb[y / 8 * DISPLAY_W + x];
            uint8_t mask = 1 << (y % 8);
            if (bits & (1 << (row & 7))) *b |= mask; else *b &= ~mask;
        }
    }
    s_cursor_x = x + 1;    /* 1 px inter-glyph spacing */
}

static void newline(void)
{
    s_cursor_x = 0;
    s_cursor_y += LINE_H;
    if (s_cursor_y + LINE_H > DISPLAY_H) {
        /* scroll the framebuffer up one line (8 rows — one page per line,
           but shift whole columns for generality) */
        for (int x = 0; x < DISPLAY_W; x++) {
            uint64_t col = 0;
            for (int yb = 0; yb < DISPLAY_H / 8; yb++)
                col |= (uint64_t)s_fb[yb * DISPLAY_W + x] << (8 * yb);
            col >>= LINE_H;
            for (int yb = 0; yb < DISPLAY_H / 8; yb++)
                s_fb[yb * DISPLAY_W + x] = (uint8_t)(col >> (8 * yb));
        }
        s_cursor_y -= LINE_H;
    }
}

static int glyph_width(char c)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    return FONT_GLYPHS[c - FONT_FIRST].w + 1;   /* ink + spacing */
}

/* Draw with word-aware wrapping: a word that would not fit on the current
   line moves to the next line instead of being split. */
static void draw_text_at(int x, int y, const char *text)
{
    s_cursor_x = x;
    s_cursor_y = y;
    const char *p = text;
    while (*p) {
        if (*p == '\n') { newline(); p++; continue; }
        if (*p == ' ') { put_char(' ', false); p++; continue; }
        /* measure the next word */
        const char *w = p;
        int ww = 0;
        while (*w && *w != ' ' && *w != '\n') ww += glyph_width(*w++);
        if (ww > DISPLAY_W) ww = DISPLAY_W;   /* very long token: split anyway */
        if (s_cursor_x > x && s_cursor_x + ww > DISPLAY_W) newline();
        while (p < w) {
            put_char(*p++, false);
            if (s_cursor_x >= DISPLAY_W) newline();   /* split over-long token */
        }
    }
}

static void clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
    s_cursor_x = 0;
    s_cursor_y = 0;
}

void display_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = BOARD_OLED_SDA,
        .scl_io_num = BOARD_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = BOARD_OLED_ADDR,
        .scl_speed_hz = 400 * 1000,
        .control_phase_bytes = 1,          /* SSD1306 uses 1 control byte */
        .dc_bit_offset = 6,                /* bit 6 = data/command */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,              /* no reset line in this wiring */
        .bits_per_pixel = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io, &panel_cfg, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    /* Panel orientation: this unit's SSD1309 is mounted rotated vs the
       default scan direction, so flip vertically (menuconfig-adjustable). */
    esp_lcd_panel_mirror(s_panel,
#ifdef CONFIG_DISPLAY_FLIP_X
                         CONFIG_DISPLAY_FLIP_X,
#else
                         false,
#endif
                         CONFIG_DISPLAY_FLIP_Y);

    clear();
    flush();
    ESP_LOGI(TAG, "SSD1309 display ready (128x64 @ 0x%02X)", BOARD_OLED_ADDR);
}

void display_status_line(const char *state_text, const char *detail)
{
    clear();
    char line[26];
    snprintf(line, sizeof(line), "%-20.20s", state_text ? state_text : "");
    draw_text_at(0, 0, line);
    /* divider under the state line */
    for (int x = 0; x < DISPLAY_W; x++)
        put_pixel(x, LINE_H);
    if (detail) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s", detail);
        draw_text_at(0, LINE_H + 2, buf);   /* up to 3 lines below divider */
    }
    flush();
}

void display_text(const char *text)
{
    clear();
    draw_text_at(0, 0, text);
    flush();
}

void display_activation(const char *code)
{
    clear();
    draw_text_at(0, 0, "Enter code at");
    draw_text_at(0, LINE_H, "xiaozhi.me to bind:");
    /* Big digits: 2x-scaled glyphs (32 px tall), fit under the heading */
    int bx = 0, by = LINE_H * 2;
    for (const char *p = code; *p; p++) {
        char c = *p;
        if (c < FONT_FIRST || c > FONT_LAST) c = '?';
        const font_glyph_t *g = &FONT_GLYPHS[c - FONT_FIRST];
        if (bx + g->w * 3 > DISPLAY_W) { bx = 0; by += LINE_H * 3; }
        for (int cx = 0; cx < g->w; cx++) {
            for (int row = 0; row < LINE_H; row++) {
                if (!(g->col[row >> 3][cx] & (1 << (row & 7)))) continue;
                for (int sy = 0; sy < 2; sy++)
                    for (int sx = 0; sx < 2; sx++)
                        put_pixel(bx + cx * 2 + sx, by + row * 2 + sy);
            }
        }
        bx += g->w * 2 + 4;
    }
    flush();
}

void display_emotion(const char *emotion)
{
    /* No face bitmap set in this build: show the emotion as text */
    char buf[32];
    snprintf(buf, sizeof(buf), "[%s]", emotion ? emotion : "neutral");
    int w = 0;
    for (const char *p = buf; *p; p++) w += glyph_width(*p);
    draw_text_at(DISPLAY_W - w, 0, buf);
    flush();
}
