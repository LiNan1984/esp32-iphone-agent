// P4 USB device transport. The USB-Serial/JTAG port shares this PHY.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Start and stop must be called from one owner task. Stop restores USB-Serial/JTAG.
esp_err_t p4_usb_hid_start(void);
esp_err_t p4_usb_hid_stop(void);
bool p4_usb_hid_ready(void);
// Request remote wakeup when suspended and wait up to 500 ms for a report slot.
bool p4_usb_hid_wake(void);
// These functions enqueue one report, returning false if the host is unavailable
// or the USB endpoint does not become ready within 500 ms.
bool p4_usb_hid_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel, int8_t pan);
bool p4_usb_hid_keyboard(uint8_t modifiers, const uint8_t keys[6]);
bool p4_usb_hid_consumer(uint16_t usage);
