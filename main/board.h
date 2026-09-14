/**
 * board.h — fixed pin map for this assembled Seeed XIAO ESP32-S3 unit.
 * Wiring is already done; these are not configurable.
 *
 * NOTE: this XIAO variant's silkscreen is NOT the classic XIAO mapping:
 *   D0-D5 = GPIO1-6, D6 = GPIO43 (UART0_TX), D7 = GPIO44 (UART0_RX),
 *   D8 = GPIO7, D9 = GPIO8, D10 = GPIO9.
 */
#pragma once

/* INMP441 microphone (I2S RX, controller I2S0) */
#define BOARD_MIC_SD    GPIO_NUM_1   /* D0 — data out of mic  */
#define BOARD_MIC_SCK   GPIO_NUM_3   /* D2 — bit clock        */
#define BOARD_MIC_WS    GPIO_NUM_2   /* D1 — word select      */
/* Mic L/R pin tied to GND -> left-channel slot.
   GPIO3 is a JTAG strapping pin — fine as a driven I2S clock: the
   strapping is only sampled at reset, and JTAG-on pins 39-42 are not
   broken out on the XIAO. */

/* MAX98357A amplifier (I2S TX, controller I2S1) */
#define BOARD_AMP_DIN   GPIO_NUM_44  /* D7 — data in of amp */
#define BOARD_AMP_LRC   GPIO_NUM_8   /* D9 — word select    */
#define BOARD_AMP_BCLK  GPIO_NUM_7   /* D8 — bit clock      */
/* SD/GAIN floating -> always on, ~9 dB gain. No shutdown GPIO in this wiring.
   D7/D9 are the UART0 RX/TX pads (GPIO44/43) — free because the console is
   USB Serial/JTAG. */

/* SSD1309 OLED 128x64 (I2C port 0) */
#define BOARD_OLED_SDA  GPIO_NUM_6   /* D5 */
#define BOARD_OLED_SCL  GPIO_NUM_43  /* D6 */
#define BOARD_OLED_ADDR 0x3C

/* On-board BOOT button (on the XIAO module itself, not part of external wiring) */
#define BOARD_BUTTON_BOOT GPIO_NUM_0

/* Optional external push button (momentary, to GND).
   Wired between the D4 pad and GND; internal pull-up is enabled in code.
   Same behaviour as BOOT: short press = wake/barge-in, long press = face. */
#define BOARD_BUTTON_EXT  GPIO_NUM_5   /* D4 */

/* USB D-/D+ are GPIO19/20 internally — never repurpose. */