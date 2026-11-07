// Forward the AirPlay GOP to the App over the BLE video characteristic.
// See p4_video.h for the wire format and why this is the fallback transport.
#include "p4_video.h"
#include "p4_ble.h"
#include "video/gop.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_npl_os.h"

struct ble_npl_eventq *ble_hs_evq_get(void);

static const char *TAG = "video";

// The GOP the AirPlay receiver feeds. Owned by main.c.
extern gop_buf g_gop;

// A framed notify, ready to hand to p4_ble_video(). One AU can be several
// notifies, so this is a chunk, not an AU.
typedef struct {
    uint8_t bytes[512];
    size_t len;
} vchunk_item;

#define Q_DEPTH 16
static QueueHandle_t chunk_q;
static TaskHandle_t worker;
static struct ble_npl_event drain_ev;
static volatile bool drain_pending;

static bool streaming = false;
static uint16_t seq = 0;
static int sent = 0;          // AUs already handed to the App
static uint32_t last_gen = 0; // GOP restarts bump this

// ---------------------------------------------------------------- drain side

// Runs on the NimBLE host task: p4_ble_video() allocates mbufs and is not safe
// from anywhere else.
static void drain_event(struct ble_npl_event *ev) {
    (void)ev;
    drain_pending = false;
    vchunk_item c;
    while (xQueueReceive(chunk_q, &c, 0) == pdPASS) {
        if (p4_ble_video(p4_ble_active_session(), c.bytes, c.len) != 0) {
            // Not subscribed or gone: stop rather than spin on a dead link.
            ESP_LOGW(TAG, "video notify refused, dropping the rest of the queue");
            break;
        }
    }
}

static void post_drain(void) {
    if (drain_pending) return;   // coalesce: one drain empties the whole queue
    drain_pending = true;
    ble_npl_eventq_put(ble_hs_evq_get(), &drain_ev);
}

// ---------------------------------------------------------------- send side

// Chunks one access unit and queues the notifies.
static void send_au(const uint8_t *data, size_t len, uint8_t flags, uint32_t ts) {
    vchunk c;
    vchunk_begin(&c, data, len, flags, ts);
    for (;;) {
        vchunk_item item;
        size_t n = vchunk_next(&c, item.bytes + VCHUNK_HDR, sizeof item.bytes - VCHUNK_HDR, seq);
        if (n == 0) break;
        item.bytes[0] = (uint8_t)(seq & 0xFF);
        item.bytes[1] = (uint8_t)(seq >> 8);
        item.bytes[2] = c.flags;
        item.len = n + VCHUNK_HDR;
        seq++;
        // A full queue means the App is not keeping up; drop the AU rather than
        // stall the AirPlay receive path. The App resyncs at the next IDR.
        if (xQueueSend(chunk_q, &item, 0) != pdPASS) {
            ESP_LOGW(TAG, "video queue full, dropping an AU");
            return;
        }
    }
    post_drain();
}

static void video_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));

        // The GOP is a ring that restarts on an IDR too big to fit; detect that
        // by generation and by the list shrinking under what we already sent.
        if (g_gop.gen != last_gen || g_gop.n < sent) { sent = 0; last_gen = g_gop.gen; }
        if (!streaming || g_gop.n == 0) continue;

        // The config (SPS/PPS) has to go first, or the decoder cannot start.
        if (sent == 0 && g_gop.cfg_len) {
            send_au(g_gop.cfg, g_gop.cfg_len, VF_CONFIG, g_gop.cfg_ts);
        }
        while (sent < g_gop.n) {
            const gop_au *a = &g_gop.aus[sent];
            uint8_t flags = (a->idr ? VF_IDR : 0) | VF_START | VF_END;
            if (sent == 0) flags |= VF_START;
            send_au(g_gop.arena + a->off, a->len, flags, a->ts);
            sent++;
        }
    }
}

// ------------------------------------------------------------------- commands

void p4_video_stream(bool on) {
    streaming = on;
    sent = 0;               // restart from the IDR, as the App expects
    last_gen = g_gop.gen;
    ESP_LOGI(TAG, "stream %s", on ? "on" : "off");
}

void p4_video_snap(void) {
    // Config + whatever is buffered, once. `sent` is left alone so a running
    // stream does not replay the same AUs.
    if (g_gop.cfg_len) send_au(g_gop.cfg, g_gop.cfg_len, VF_CONFIG, g_gop.cfg_ts);
    for (int i = 0; i < g_gop.n; i++) {
        const gop_au *a = &g_gop.aus[i];
        uint8_t flags = (a->idr ? VF_IDR : 0) | VF_START | VF_END | VF_SNAP;
        send_au(g_gop.arena + a->off, a->len, flags, a->ts);
    }
    ESP_LOGI(TAG, "snap: %d AUs", g_gop.n);
}

bool p4_video_streaming(void) { return streaming; }

esp_err_t p4_video_start(void) {
    if (chunk_q) return ESP_OK;
    chunk_q = xQueueCreate(Q_DEPTH, sizeof(vchunk_item));
    if (!chunk_q) return ESP_ERR_NO_MEM;
    ble_npl_event_init(&drain_ev, drain_event, NULL);
    if (xTaskCreatePinnedToCore(video_task, "video", 4096, NULL, 5, &worker, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "video forwarding up (BLE fallback transport)");
    return ESP_OK;
}
