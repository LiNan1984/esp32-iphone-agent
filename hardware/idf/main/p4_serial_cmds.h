#pragma once
#include "esp_err.h"

// A command line reader on the USB Serial/JTAG console: "<id> <command>" in,
// "<id> <reply>" out.
//
// This is the escape hatch the original firmware has too (hardware/tools/
// board_test.py --serial uses it "when the Mac isn't bonded over BLE"). It
// matters here because BLE pairing needs a central that completes SMP, which
// macOS will not do for a background process — so without this there is no way
// to exercise the command path from a desk at all.
//
// It deliberately bypasses the command bridge's queue: this task may block (the
// move pacing does), and nothing here is on the NimBLE host task.
esp_err_t p4_serial_cmds_start(void);
