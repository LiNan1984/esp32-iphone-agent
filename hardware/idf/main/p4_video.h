#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Forward the AirPlay GOP to the App over the BLE `video` characteristic.
//
// The board never decodes: src/airplay/mirror.c hands raw Annex-B access units
// to the GOP buffer, and this chunks them in the wire format the App's
// VideoAssembler expects — [seq u16 LE][flags u8][payload], with the START chunk
// carrying a u32 LE board timestamp before the Annex-B data (docs/ble-protocol.md).
//
// The App drives it with `stream on|off` (continuous) and `snap` (config + the
// current GOP, then done). The Wi-Fi TCP path on port 7201 is the primary
// transport when the App is on the board's hotspot; this is the fallback that
// works over BLE alone.

// Starts the forwarding task. The GOP must already be initialised.
esp_err_t p4_video_start(void);

// `stream on` / `stream off` / `snap`, from the command interpreter.
void p4_video_stream(bool on);
void p4_video_snap(void);
bool p4_video_streaming(void);
