#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

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
//     posts an os_event back to that task via ble_hs_event_enqueue() rather than
//     calling the notify itself.

// Owns the queues and the worker task. Safe to call before p4_ble_start().
esp_err_t p4_cmd_bridge_start(void);
