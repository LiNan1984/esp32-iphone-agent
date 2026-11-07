#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct { uint16_t conn; uint32_t generation; } p4_ble_session;

// All callbacks execute on the NimBLE host task and must not block. The command
// and net-write callbacks must copy their input before returning. The session
// generation prevents a queued reply from reaching a later connection that
// happens to reuse the same connection handle.
typedef struct {
    int (*command)(p4_ble_session session, const uint8_t *data, size_t len);
    // The unencrypted CLI service (...0010). Optional: when NULL the CLI service
    // is not registered. Replies for it go to cli_out.
    int (*command_cli)(p4_ble_session session, const uint8_t *data, size_t len);
    int (*net_read)(uint8_t *out, size_t cap, size_t *len);
    int (*net_write)(const uint8_t *data, size_t len);
    int (*status_read)(uint8_t *out, size_t cap, size_t *len);
    void (*disconnected)(p4_ble_session session);
} p4_ble_ops;

// Forget every bond and restart advertising. Reopens the pairing window without
// a power cycle, which matters because the window is only 3 minutes.
esp_err_t p4_ble_unpair(void);

// NVS and the ESP-Hosted HCI binding must be initialized by the caller first.
esp_err_t p4_ble_start(const p4_ble_ops *ops);
// The session currently allowed to receive notifications, or a zeroed session
// (conn == BLE_HS_CONN_HANDLE_NONE) when there is none.
p4_ble_session p4_ble_active_session(void);

// Call notification functions on the NimBLE host task only: a worker must post
// an event to that task. Accepted only for the same encrypted, bonded session.
int p4_ble_reply(p4_ble_session session, const void *data, size_t len);
int p4_ble_status(p4_ble_session session, const void *data, size_t len);
int p4_ble_video(p4_ble_session session, const void *data, size_t len);
// The unencrypted CLI channel: no auth gate, no bond, no pairing prompt.
int p4_ble_cli_out(p4_ble_session session, const void *data, size_t len);
