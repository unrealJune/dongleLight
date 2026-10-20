#pragma once
#include <stdint.h>

typedef enum {
    LED_BOOT,       // dim white
    LED_SCANNING,   // blue
    LED_CONNECTED,  // green
    LED_FRAME,      // brief white flash: valid frame heard
    LED_BAD_FRAME,  // brief magenta flash: frame-shaped audio, bad CRC
    LED_ERROR,      // red: write failed / no lamp
} led_state_t;

void status_led_init(void);
void status_led_set(led_state_t s);
// Show s for ms, then return to the steady state.
void status_led_flash(led_state_t s, int ms);
