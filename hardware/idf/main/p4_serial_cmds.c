// A command line reader on the USB Serial/JTAG console. See p4_serial_cmds.h for
// why it exists: it is the only way to exercise the command path without a BLE
// central that completes SMP.
#include "p4_serial_cmds.h"
#include "p4_cmd_bridge.h"
#include "p4_hid_commands.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"

#include <string.h>

static const char *TAG = "serialcmd";

// Same limits as the BLE bridge: the original firmware caps a command line at
// 512 bytes (ble_link.cpp CMD_MAX).
#define LINE_MAX 512
#define ID_MAX 16

static void serial_task(void *arg) {
    (void)arg;
    char line[LINE_MAX];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        int rd = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100));
        if (rd != 1) continue;

        if (c == '\n' || c == '\r') {
            if (n == 0) continue;
            line[n] = '\0';
            n = 0;

            char id[ID_MAX];
            const char *cmd;
            size_t cmd_len;
            if (!p4_cmd_split(line, strlen(line), id, sizeof id, &cmd, &cmd_len)) {
                ESP_LOGW(TAG, "ignored (need \"<id> <command>\"): %s", line);
                continue;
            }
            char bare[LINE_MAX];
            memcpy(bare, cmd, cmd_len);
            bare[cmd_len] = '\0';

            const char *reply = p4_hid_command(bare);
            // Reply on the same channel, in the App's own format, so
            // hardware/tools/board_test.py --serial can drive this unchanged.
            printf("%s %s\n", id, reply ? reply : "err no reply");
            continue;
        }

        if (n < sizeof(line) - 1) line[n++] = (char)c;
        else n = 0;   // overrun: drop the line rather than split it
    }
}

esp_err_t p4_serial_cmds_start(void) {
    // Reading without installing the driver first faults (Load access fault on the
    // task's core) rather than returning an error.
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = 1024, .rx_buffer_size = 1024};
    usb_serial_jtag_driver_install(&cfg);
    // A generous stack: p4_hid_command() reaches pace_t, snprintf and TinyUSB.
    if (xTaskCreatePinnedToCore(serial_task, "serialcmd", 4096, NULL, 5, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial commands ready: \"<id> <command>\" over USB Serial/JTAG");
    return ESP_OK;
}
