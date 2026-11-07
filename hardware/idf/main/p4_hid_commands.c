// HID command semantics adapted from hardware/hid_bridge/hid_bridge.ino.
#include "p4_hid_commands.h"
#include "p4_usb_hid.h"
#include "hid/pace.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "class/hid/hid.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t buttons;
static const uint8_t ascii_keys[128][2] = { HID_ASCII_TO_KEYCODE };
static const char *const unavailable = "err phone usb asleep (unlock the phone)";

static bool mouse(int x, int y, int wheel, int pan) {
    return p4_usb_hid_mouse(buttons, (int8_t)x, (int8_t)y, (int8_t)wheel, (int8_t)pan);
}
static bool keyboard(uint8_t modifiers, const uint8_t keys[6]) {
    return p4_usb_hid_keyboard(modifiers, keys);
}
void p4_hid_release_all(void) {
    buttons = 0;
    (void)mouse(0, 0, 0, 0);
    (void)keyboard(0, NULL);
    (void)p4_usb_hid_consumer(0);
}
static bool parse_number(const char **p, long *out) {
    while (**p == ' ') ++*p;
    if (!**p) return false;
    errno = 0;
    char *end;
    long n = strtol(*p, &end, 10);
    if (end == *p || errno == ERANGE || (*end && *end != ' ')) return false;
    *out = n;
    *p = end;
    return true;
}
static int clamp(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}
static bool translate_char(unsigned char ch, uint8_t *mod, uint8_t *key) {
    if (ch >= 128 || !ascii_keys[ch][1]) return false;
    *mod = ascii_keys[ch][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
    *key = ascii_keys[ch][1];
    return true;
}
static bool translate_name(const char *name, uint8_t *mod, uint8_t *key) {
    struct special { const char *name; uint8_t mod, key; };
    static const struct special table[] = {
        {"enter",0,HID_KEY_ENTER}, {"esc",0,HID_KEY_ESCAPE}, {"tab",0,HID_KEY_TAB},
        {"backspace",0,HID_KEY_BACKSPACE}, {"space",0,HID_KEY_SPACE},
        {"up",0,HID_KEY_ARROW_UP}, {"down",0,HID_KEY_ARROW_DOWN},
        {"left",0,HID_KEY_ARROW_LEFT}, {"right",0,HID_KEY_ARROW_RIGHT},
        {"cmd",KEYBOARD_MODIFIER_LEFTGUI,0}, {"ctrl",KEYBOARD_MODIFIER_LEFTCTRL,0},
        {"alt",KEYBOARD_MODIFIER_LEFTALT,0}, {"shift",KEYBOARD_MODIFIER_LEFTSHIFT,0},
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); ++i) {
        if (!strcmp(name, table[i].name)) { *mod = table[i].mod; *key = table[i].key; return true; }
    }
    return name[0] && !name[1] && translate_char((unsigned char)name[0], mod, key);
}
static const char *send_combo(const char *arg) {
    char copy[128];
    size_t n = strlen(arg);
    if (!n || n >= sizeof(copy)) return "err unknown key";
    memcpy(copy, arg, n + 1);
    uint8_t mods = 0, keys[6] = {0};
    size_t count = 0;
    char *part = copy;
    for (;;) {
        char *sep = strchr(part, '+');
        if (sep) *sep = 0;
        uint8_t mod = 0, key = 0;
        if (!translate_name(part, &mod, &key) || (key && count == 6)) return "err unknown key";
        mods |= mod;
        if (key) keys[count++] = key;
        if (!sep) break;
        part = sep + 1;
    }
    if (!keyboard(mods, keys)) return unavailable;
    vTaskDelay(pdMS_TO_TICKS(30));
    return keyboard(0, NULL) ? "ok" : unavailable;
}
static const char *send_text(const char *arg) {
    uint8_t mod, key;
    for (const unsigned char *p = (const unsigned char *)arg; *p; ++p)
        if (!translate_char(*p, &mod, &key)) return "err unsupported character";
    for (const unsigned char *p = (const unsigned char *)arg; *p; ++p) {
        (void)translate_char(*p, &mod, &key);
        uint8_t keys[6] = {key};
        if (!keyboard(mod, keys)) return unavailable;
        if (!keyboard(0, NULL)) return unavailable;
    }
    return "ok";
}
const char *p4_hid_command(const char *line) {
    if (!line) return "err unknown command";
    const char *space = strchr(line, ' ');
    size_t len = space ? (size_t)(space - line) : strlen(line);
    const char *arg = space ? space + 1 : "";
#define CMD(s) (len == sizeof(s)-1 && !memcmp(line, s, sizeof(s)-1))
    if (CMD("ping")) return "ok";
    if (CMD("pace")) {
        static char version[24];
        snprintf(version, sizeof(version), "ok %d", PACE_VERSION);
        return version;
    }
    if (!p4_usb_hid_wake()) return unavailable;
    if (CMD("home")) {
        for (int axis = 0; axis < 2; ++axis) for (int i = 0; i < 25; ++i) {
            if (!mouse(axis ? 0 : -127, axis ? -127 : 0, 0, 0)) return unavailable;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        return "ok";
    }
    if (CMD("move")) {
        long dx, dy, speed = 10;
        const char *p = arg;
        if (!parse_number(&p, &dx) || !parse_number(&p, &dy)) return "err usage: move dx dy [speed]";
        while (*p == ' ') ++p;
        if (*p && !parse_number(&p, &speed)) return "err usage: move dx dy [speed]";
        while (*p == ' ') ++p;
        if (*p || dx < -32767 || dx > 32767 || dy < -32767 || dy > 32767) return "err invalid move";
        pace_t pace;
        pace_start(&pace, dx, dy, clamp((int)(speed < 1 ? 1 : speed > 40 ? 40 : speed), 1, 40));
        int x, y;
        while (pace_next(&pace, &x, &y)) {
            if (!mouse(x, y, 0, 0)) return unavailable;
            vTaskDelay(pdMS_TO_TICKS(8));
        }
        return "ok";
    }
    if (CMD("press") || CMD("release") || CMD("click")) {
        buttons = CMD("release") ? 0 : 1;
        if (!mouse(0, 0, 0, 0)) { buttons = 0; return unavailable; }
        if (CMD("click")) {
            vTaskDelay(pdMS_TO_TICKS(60));
            buttons = 0;
            if (!mouse(0, 0, 0, 0)) return unavailable;
        }
        return "ok";
    }
    if (CMD("scroll")) {
        long v, h;
        const char *p = arg;
        if (!parse_number(&p, &v) || !parse_number(&p, &h) || v < -127 || v > 127 || h < -127 || h > 127) return "err usage: scroll v h";
        return mouse(0, 0, (int)v, (int)h) ? "ok" : unavailable;
    }
    if (CMD("type")) return send_text(arg);
    if (CMD("key")) return send_combo(arg);
    if (CMD("homescreen")) {
        if (!p4_usb_hid_consumer(0x0223)) return unavailable;
        vTaskDelay(pdMS_TO_TICKS(40));
        return p4_usb_hid_consumer(0) ? "ok" : unavailable;
    }
    return "err unknown command";
#undef CMD
}
