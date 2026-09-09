/**
 * board.h — fixed pin map for this assembled Seeed XIAO ESP32-S3 unit.
 * Wiring is already done; these are not configurable.
 */
#pragma once

/* INMP441 microphone (I2S RX, controller I2S0) */
#define BOARD_MIC_SD    GPIO_NUM_1   /* D0  — data out of mic  */
#define BOARD_MIC_SCK   GPIO_NUM_44  /* D7  — bit clock        */
#define BOARD_MIC_WS    GPIO_NUM_9   /* D10 — word select      */
/* Mic L/R pin tied to GND -> left-channel slot */

/* MAX98357A amplifier (I2S TX, controller I2S1) */
#define BOARD_AMP_DIN   GPIO_NUM_2   /* D1 — data in of amp */
#define BOARD_AMP_LRC   GPIO_NUM_4   /* D3 — word select    */
#define BOARD_AMP_BCLK  GPIO_NUM_7   /* D8 — bit clock      */
/* SD/GAIN floating -> always on, ~9 dB gain. No shutdown GPIO in this wiring. */

/* SSD1309 OLED 128x64 (I2C port 0) */
#define BOARD_OLED_SDA  GPIO_NUM_5   /* D4 */
#define BOARD_OLED_SCL  GPIO_NUM_6   /* D5 */
#define BOARD_OLED_ADDR 0x3C

/* On-board BOOT button (on the XIAO module itself, not part of external wiring) */
#define BOARD_BUTTON_BOOT GPIO_NUM_0

/* Optional external push button (momentary, to GND).
   Wired between the D2 pad and GND; internal pull-up is enabled in code.
   Same behaviour as BOOT: short press = wake/barge-in, long press = face. */
#define BOARD_BUTTON_EXT  GPIO_NUM_3   /* D2 */

/* USB D-/D+ are GPIO19/20 internally — never repurpose. */