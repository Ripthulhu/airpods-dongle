#ifndef AIRPODS_LED_H
#define AIRPODS_LED_H
#include <stdbool.h>
void set_led_mode_pairing(void);
void set_led_mode_off(void);
void set_led_mode_on(void);
void set_led_game_feedback(bool enabled);
#endif
