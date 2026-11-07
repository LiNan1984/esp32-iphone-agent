// The BLE -> USB HID command bridge. See p4_cmd_bridge.h for why it is shaped
// this way (non-blocking NimBLE callbacks, replies posted back to the host task).
#include "p4_cmd_bridge.h"
#include "p4_ble.h"
#include "p4_hid_commands.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_npl_os.h"   // struct ble_npl_event, OS-specific

// ble_hs_evq_get() lives in NimBLE's private header (host/src/ble_hs_priv.h),
// which is not on the include path. Declare it: it returns the queue the host
// task drains, which is the supported way back onto that task.
struct ble_npl_eventq *ble_hs_evq_get(void);

static const char *TAG = "cmdbridge";

// The original firmware caps a command line at 512 bytes (ble_link.cpp CMD_MAX)
// and echoes a decimal id, which it picks from 100000..800000.
#define CMD_MAX 512
#define ID_MAX 16

typedef struct {
    p4_ble_session session;
    char id[ID_MAX];
    char line[CMD_MAX];
    bool via_cli;   // reply on the unencrypted CLI channel, not the App's
} cmd_item;

typedef struct {
    p4_ble_session session;
    char reply[CMD_MAX];
    bool via_cli;
} reply_item;

static QueueHandle_t cmd_q, reply_q;
static TaskHandle_t worker;
// Set by the worker when it posts reply_ev, cleared by reply_event. Re-posting an
// event that is still queued is not something the npl layer promises to tolerate,
// so coalesce: a second post while one is pending is redundant anyway, because
// reply_event drains the whole queue.
static volatile bool reply_pending;

// Posted to the NimBLE host task; drains reply_q there.
static void reply_event(struct ble_npl_event *ev) {
    (void)ev;
    reply_pending = false;
    reply_item r;
    while (xQueueReceive(reply_q, &r, 0) == pdPASS) {
        int rc = r.via_cli ? p4_ble_cli_out(r.session, r.reply, strlen(r.reply))
                           : p4_ble_reply(r.session, r.reply, strlen(r.reply));
        if (rc != 0) {
            // Not encrypted/bonded/subscribed any more: drop it. The App times the
            // request out on its side, which is the same as the original firmware.
            ESP_LOGW(TAG, "reply notify failed rc=%d (%.48s)", rc, r.reply);
            break;
        }
    }
}

static struct ble_npl_event reply_ev;   // filled by ble_npl_event_init below

static void worker_task(void *arg) {
    (void)arg;
    cmd_item c;
    for (;;) {
        if (xQueueReceive(cmd_q, &c, portMAX_DELAY) != pdPASS) continue;
        ESP_LOGI(TAG, "running id=%s cmd=%.32s", c.id, c.line);
        const char *r = p4_hid_command(c.line);
        ESP_LOGI(TAG, "reply id=%s -> %.48s", c.id, r ? r : "(null)");
        reply_item out = { .session = c.session, .via_cli = c.via_cli };
        snprintf(out.reply, sizeof out.reply, "%s %s", c.id, r ? r : "err no reply");
        if (xQueueSend(reply_q, &out, 0) != pdPASS) {
            ESP_LOGW(TAG, "reply queue full, dropping");
            continue;
        }
        if (!reply_pending) {
            reply_pending = true;
            ble_npl_eventq_put(ble_hs_evq_get(), &reply_ev);
        }
    }
}

// p4_ble command callback: copies the line and returns immediately.
static int enqueue(p4_ble_session session, const uint8_t *data, size_t len, bool via_cli) {
    if (!data || len == 0 || len >= CMD_MAX) return -1;

    char id[ID_MAX];
    const char *cmd;
    size_t cmd_len;
    if (!p4_cmd_split((const char *)data, len, id, sizeof id, &cmd, &cmd_len)) return -1;

    cmd_item c = { .session = session, .via_cli = via_cli };
    memcpy(c.id, id, strlen(id) + 1);
    memcpy(c.line, cmd, cmd_len);
    c.line[cmd_len] = '\0';

    if (xQueueSend(cmd_q, &c, 0) != pdPASS) {
        ESP_LOGW(TAG, "command queue full");
        return -1;
    }
    ESP_LOGI(TAG, "queued id=%s cmd=%.32s via_cli=%d", c.id, c.line, (int)via_cli);
    return 0;
}

int p4_cmd_bridge_command(p4_ble_session session, const uint8_t *data, size_t len) {
    return enqueue(session, data, len, false);
}

// The unencrypted CLI service: replies go back on cli_out, not the App's reply.
int p4_cmd_bridge_command_cli(p4_ble_session session, const uint8_t *data, size_t len) {
    return enqueue(session, data, len, true);
}

// Releasing held buttons/keys is cheap and must not be deferred: if the App drops
// while the button is down, the phone would keep receiving clicks.
void p4_cmd_bridge_disconnected(p4_ble_session session) {
    (void)session;
    p4_hid_release_all();
}

esp_err_t p4_cmd_bridge_start(void) {
    if (cmd_q) return ESP_OK;  // idempotent
    cmd_q = xQueueCreate(8, sizeof(cmd_item));
    reply_q = xQueueCreate(8, sizeof(reply_item));
    if (!cmd_q || !reply_q) return ESP_ERR_NO_MEM;
    // Initialize the event before the task exists, so the worker can never post
    // an uninitialized one.
    ble_npl_event_init(&reply_ev, reply_event, NULL);
    // A generous stack: p4_hid_command() reaches pace_t, snprintf and TinyUSB.
    if (xTaskCreatePinnedToCore(worker_task, "cmdbridge", 4096, NULL, 5, &worker, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "command bridge up (cmd_q=8 reply_q=8)");
    return ESP_OK;
}
