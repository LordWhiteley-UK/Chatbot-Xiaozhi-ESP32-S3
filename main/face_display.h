/**
 * face_display.h — animated face mode for the SSD1309 OLED.
 *
 * A second display mode that shows an animated face reflecting the
 * device state (idle / listening / thinking / speaking).  Toggled at
 * runtime via the BOOT button (long-press).  When disabled, the normal
 * info display (display.c) owns the screen.
 */
#pragma once
#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

void face_display_init(void);             /* create the animation task */
void face_display_set_enabled(bool on);   /* toggle face / info mode */
bool face_display_is_enabled(void);
void face_display_set_state(app_state_t s); /* push state → face animation */

#ifdef __cplusplus
}
#endif