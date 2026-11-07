#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "p4_ble.h"   // p4_ble_session

// The BLE -> USB HID command bridge.
//
// The App writes "<id> <command>" to the `cmd` characteristic. The board executes
// the command over USB HID and notifies "<id> ok" / "<id> err ..." on `reply`.
//
// Two constraints shape the design:
//
//  1. p4_ble's callbacks run on the NimBLE host task and must not block — but the
//     original firmware paces a long drag deliberately (pace_start/pace_next, ~10
//     units per step). Hundreds of milliseconds of vTaskDelay on the host task
//     would stall every other BLE event, so commands are copied, queued and run
//     on a worker task.
//  2. The reply must be notified from the NimBLE host task (ble_gatts_notify_custom
//     allocates mbufs and is not safe from another task). The worker therefore
//     posts a ble_npl_event back to that task via ble_hs_evq_get() +
//     ble_npl_eventq_put() rather than calling the notify itself.
//     (ble_hs_event_enqueue() would read better but is unusable: its
//     struct os_event is not defined anywhere in IDF's bt component.)

// Owns the queues and the worker task. Safe to call before p4_ble_start().
esp_err_t p4_cmd_bridge_start(void);

// The p4_ble_ops implementations. `command` copies its input and returns
// immediately (it runs on the NimBLE host task); `disconnected` releases any
// held buttons and keys.
int p4_cmd_bridge_command(p4_ble_session session, const uint8_t *data, size_t len);
void p4_cmd_bridge_disconnected(p4_ble_session session);

// Splits "<id> <command>". Pure and host-testable; see tests/test_cmd_split.c.
bool p4_cmd_split(const char *line, size_t len, char *id, size_t id_cap,
                  const char **cmd, size_t *cmd_len);
