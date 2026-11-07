// USB PHY routing follows the Metalio Claw4 board's usb_virtual_disk driver.
#include "p4_usb_hid.h"
#include <stdatomic.h>
#include "driver/gpio.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"
#include "soc/usb_wrap_struct.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

enum { REPORT_KEYBOARD = 1, REPORT_MOUSE, REPORT_CONSUMER };
static const uint8_t report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_CONSUMER)),
};
static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(report_desc), 0x81, 16, 10),
};
static const char *strings[] = {
    (const char[]){0x09, 0x04}, "Handsy", "ESP32 Agent", "P4", "HID",
};
static usb_phy_handle_t phy;
static SemaphoreHandle_t report_lock;
static atomic_bool started;

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return report_desc;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t len) {
    (void)instance; (void)id; (void)type; (void)buffer; (void)len;
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                           uint8_t const *buffer, uint16_t len) {
    (void)instance; (void)id; (void)type; (void)buffer; (void)len;
}
static void route_serial(void) {
    usb_wrap_ll_phy_select(&USB_WRAP, 1);
    usb_serial_jtag_ll_phy_set_defaults();
    usb_serial_jtag_ll_phy_enable_pad(true);
}

esp_err_t p4_usb_hid_start(void) {
    if (started || phy) return ESP_ERR_INVALID_STATE;
    if (!report_lock) report_lock = xSemaphoreCreateMutex();
    if (!report_lock) return ESP_ERR_NO_MEM;
    usb_serial_jtag_ll_phy_enable_pad(false);
    usb_wrap_ll_phy_select(&USB_WRAP, 0);
    usb_phy_config_t cfg = {
        .controller = USB_PHY_CTRL_OTG, .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE, .otg_speed = USB_PHY_SPEED_FULL,
    };
    esp_err_t err = usb_new_phy(&cfg, &phy);
    if (err != ESP_OK) { route_serial(); return err; }
    gpio_set_drive_capability(GPIO_NUM_24, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(GPIO_NUM_25, GPIO_DRIVE_CAP_3);
    tinyusb_config_t usb = TINYUSB_CONFIG_FULL_SPEED(NULL, NULL);
    usb.phy.skip_setup = true;
    usb.descriptor.full_speed_config = config_desc;
    usb.descriptor.string = strings;
    usb.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    err = tinyusb_driver_install(&usb);
    if (err != ESP_OK) {
        esp_err_t cleanup = usb_del_phy(phy);
        if (cleanup == ESP_OK) { phy = NULL; route_serial(); }
        else return cleanup;
        return err;
    }
    started = true;
    return ESP_OK;
}

esp_err_t p4_usb_hid_stop(void) {
    if (!started && !phy) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(report_lock, portMAX_DELAY);
    esp_err_t err = started ? tinyusb_driver_uninstall() : ESP_OK;
    if (err == ESP_OK) {
        started = false;
        err = usb_del_phy(phy);
        if (err == ESP_OK) { phy = NULL; route_serial(); }
    }
    xSemaphoreGive(report_lock);
    return err;
}

bool p4_usb_hid_ready(void) { return started && tud_mounted() && !tud_suspended(); }
static bool acquire_report(void) {
    if (!report_lock || xSemaphoreTake(report_lock, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    if (!started) { xSemaphoreGive(report_lock); return false; }
    if (tud_suspended()) tud_remote_wakeup();
    for (int i = 0; i < 50; ++i) {
        if (p4_usb_hid_ready() && tud_hid_ready()) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xSemaphoreGive(report_lock);
    return false;
}
bool p4_usb_hid_wake(void) {
    if (!acquire_report()) return false;
    xSemaphoreGive(report_lock);
    return true;
}
bool p4_usb_hid_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel, int8_t pan) {
    if (!acquire_report()) return false;
    bool ok = tud_hid_mouse_report(REPORT_MOUSE, buttons, x, y, wheel, pan);
    xSemaphoreGive(report_lock);
    return ok;
}
bool p4_usb_hid_keyboard(uint8_t modifiers, const uint8_t keys[6]) {
    if (!acquire_report()) return false;
    bool ok = tud_hid_keyboard_report(REPORT_KEYBOARD, modifiers, keys);
    xSemaphoreGive(report_lock);
    return ok;
}
bool p4_usb_hid_consumer(uint16_t usage) {
    if (!acquire_report()) return false;
    uint8_t value[2] = {(uint8_t)usage, (uint8_t)(usage >> 8)};
    bool ok = tud_hid_report(REPORT_CONSUMER, value, sizeof(value));
    xSemaphoreGive(report_lock);
    return ok;
}
