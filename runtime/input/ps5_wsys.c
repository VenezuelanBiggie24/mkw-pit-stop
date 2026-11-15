// Keyboard and mouse stubs for PS5 (no keyboard/mouse window backend).
#include "input.h"
#include "input_wsys.h"

int ikb_foreground(void) { return 0; }
int ikb_down(int key) { (void)key; return 0; }
int ikb_escape(void) { return 0; }
int ikb_captured(void) { return 0; }

void ikb_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = 0;
    *dy = 0;
}

void ikb_cursor_update(void) {}
void ikb_menu_closed(void) {}
void input_cursor_suspend(int on) { (void)on; }
int input_cursor_released(void) { return 0; }
uint32_t input_key_char(int key) { (void)key; return 0; }
int input_flight_key(void) { return 0; }
int input_hack_key(int which) { (void)which; return 0; }
