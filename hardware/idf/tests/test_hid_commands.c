#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "p4_hid_commands.h"
#include "p4_usb_hid.h"

static bool connected = true;
static int mouse_calls, keyboard_calls, consumer_calls, sum_x, sum_y;
static uint8_t last_buttons, last_mod, last_key;
static uint8_t pressed_mod[16], pressed_key[16];
static int pressed_count;
static uint16_t last_usage;
static bool fail_mouse;
bool p4_usb_hid_wake(void) { return connected; }
bool p4_usb_hid_mouse(uint8_t b, int8_t x, int8_t y, int8_t wheel, int8_t pan) {
    (void)wheel; (void)pan; ++mouse_calls; last_buttons = b; sum_x += x; sum_y += y; return !fail_mouse;
}
bool p4_usb_hid_keyboard(uint8_t mod, const uint8_t keys[6]) {
    ++keyboard_calls; last_mod = mod; last_key = keys ? keys[0] : 0;
    if (last_key && pressed_count < 16) { pressed_mod[pressed_count] = mod; pressed_key[pressed_count++] = last_key; }
    return true;
}
bool p4_usb_hid_consumer(uint16_t usage) { ++consumer_calls; last_usage = usage; return true; }
void vTaskDelay(unsigned int ticks) { (void)ticks; }

int main(void) {
    assert(!strcmp(p4_hid_command("ping"), "ok"));
    assert(!strcmp(p4_hid_command("pace"), "ok 3"));
    assert(!strcmp(p4_hid_command("move 33 -17 10"), "ok"));
    assert(sum_x == 33 && sum_y == -17 && mouse_calls > 1);
    int before = mouse_calls;
    assert(!strcmp(p4_hid_command("move 1 junk"), "err usage: move dx dy [speed]"));
    assert(before == mouse_calls);
    assert(!strcmp(p4_hid_command("click"), "ok") && last_buttons == 0);
    assert(!strcmp(p4_hid_command("press"), "ok") && last_buttons == 1);
    p4_hid_release_all();
    assert(last_buttons == 0 && last_mod == 0 && last_key == 0 && last_usage == 0);
    before = keyboard_calls;
    assert(!strcmp(p4_hid_command("type A!"), "ok") && keyboard_calls == before + 4 && last_key == 0);
    assert(pressed_count == 2 && pressed_key[0] == 4 && pressed_key[1] == 30 && pressed_mod[0] == 2 && pressed_mod[1] == 2);
    before = keyboard_calls;
    assert(!strcmp(p4_hid_command("type \xc3\xa9"), "err unsupported character") && keyboard_calls == before);
    assert(!strcmp(p4_hid_command("key cmd+h"), "ok") && last_mod == 0 && last_key == 0);
    assert(pressed_count == 3 && pressed_mod[2] == 8 && pressed_key[2] == 11);
    before = keyboard_calls;
    assert(!strcmp(p4_hid_command("key cmd+bad"), "err unknown key") && before == keyboard_calls);
    assert(!strcmp(p4_hid_command("homescreen"), "ok") && consumer_calls >= 2 && last_usage == 0);
    connected = false;
    assert(!strcmp(p4_hid_command("click"), "err phone usb asleep (unlock the phone)"));
    connected = true; fail_mouse = true;
    assert(!strcmp(p4_hid_command("press"), "err phone usb asleep (unlock the phone)"));
    return 0;
}
