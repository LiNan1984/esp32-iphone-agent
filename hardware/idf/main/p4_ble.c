// NimBLE GATT transport for docs/ble-protocol.md; do not call application work
// in GATT callbacks. This module does not start USB or expose credentials itself.
#include "p4_ble.h"
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define PAIR_WINDOW_US (180LL * 1000000LL)
#define MAX_BONDS 8
#define MAX_VALUE 512
static const char *TAG = "p4_ble";
static p4_ble_ops app;
static uint8_t own_addr_type;
static uint16_t reply_handle, status_handle, video_handle;
static uint16_t cli_in_handle, cli_out_handle;
static ble_addr_t known[MAX_BONDS];
static int known_count;
static portMUX_TYPE known_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t pair_opened;
static bool started;
static p4_ble_session active = {.conn = BLE_HS_CONN_HANDLE_NONE};
static unsigned subscriptions;
void ble_store_config_init(void);

// BLE_UUID128_INIT is little-endian: service ends ...c0000, then 0001..0005.
#define P4_UUID(lo) BLE_UUID128_INIT((lo),0x00,0x3c,0x8a,0x6f,0x6d,0x39,0x9c,0x1b,0x4b,0xe8,0xa9,0x01,0x00,0x40,0x6e)
static const ble_uuid128_t service_uuid = P4_UUID(0x00);
static const ble_uuid128_t cmd_uuid = P4_UUID(0x01);
static const ble_uuid128_t reply_uuid = P4_UUID(0x02);
static const ble_uuid128_t status_uuid = P4_UUID(0x03);
static const ble_uuid128_t video_uuid = P4_UUID(0x04);
static const ble_uuid128_t net_uuid = P4_UUID(0x05);
// The CLI service (...0010). Its characteristics carry no *_ENC flag on purpose:
// a computer reaching the board over it never forms an OS bond, which is why the
// repository's own Go CLI can talk to a board without any pairing prompt. The
// application-layer session crypto (X25519 + AES-GCM, already ported as
// src/cli/cli_proto.c) is what authenticates it instead — not yet wired here.
static const ble_uuid128_t cli_svc_uuid = P4_UUID(0x10);
static const ble_uuid128_t cli_in_uuid = P4_UUID(0x11);
static const ble_uuid128_t cli_out_uuid = P4_UUID(0x12);

enum { CH_CMD, CH_STATUS, CH_NET, CH_CLI_IN };
static bool known_peer(const ble_addr_t *peer) {
    bool found = false;
    portENTER_CRITICAL(&known_lock);
    for (int i = 0; i < known_count; ++i) {
        if (peer->type == known[i].type && !memcmp(peer->val, known[i].val, sizeof(peer->val))) { found = true; break; }
    }
    portEXIT_CRITICAL(&known_lock);
    return found;
}
static bool authorized(uint16_t conn) {
    struct ble_gap_conn_desc d;
    return ble_gap_conn_find(conn, &d) == 0 && d.sec_state.encrypted && d.sec_state.bonded && known_peer(&d.peer_id_addr);
}
// Ask the central to pair when it touches an encrypted characteristic over a
// plain link. The original firmware relies on iOS doing this on its own (its
// onWrite just returns early when !isEncrypted), which works for the iPhone but
// leaves every other central stuck at "Insufficient Encryption" with no prompt.
// Requesting it explicitly is what makes macOS/Android/Linux usable too.
static void request_pairing_if_plain(uint16_t conn) {
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0 || d.sec_state.encrypted) return;
    if (ble_gap_security_initiate(conn) != 0) return;   // already in progress
    ESP_LOGI(TAG, "requested pairing for conn %u", conn);
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr;
    const uintptr_t kind = (uintptr_t)arg;
    if (conn != active.conn) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    // The CLI service is unencrypted by design, so it is handled before the
    // authorization gate rather than through it. Everything else needs the link
    // encrypted and bonded — and note that an encrypted characteristic is refused
    // by the ATT layer before this callback even runs, so the gate below is a
    // second line of defence, not the only one.
    if (kind == CH_CLI_IN) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (!len || len > MAX_VALUE) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t bytes[MAX_VALUE];
        if (os_mbuf_copydata(ctxt->om, 0, len, bytes) != 0) return BLE_ATT_ERR_UNLIKELY;
        if (!app.command_cli) return BLE_ATT_ERR_UNLIKELY;
        return app.command_cli(active, bytes, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (!authorized(conn)) {
        request_pairing_if_plain(conn);
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (!len || len > MAX_VALUE) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t bytes[MAX_VALUE];
        if (os_mbuf_copydata(ctxt->om, 0, len, bytes) != 0) return BLE_ATT_ERR_UNLIKELY;
        int rc = kind == CH_CMD ? app.command(active, bytes, len) : app.net_write(bytes, len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && (kind == CH_NET || kind == CH_STATUS)) {
        uint8_t bytes[MAX_VALUE];
        size_t len = 0;
        int rc = kind == CH_NET ? app.net_read(bytes, sizeof(bytes), &len) : app.status_read(bytes, sizeof(bytes), &len);
        if (rc || len > sizeof(bytes)) return BLE_ATT_ERR_UNLIKELY;
        return os_mbuf_append(ctxt->om, bytes, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def services[] = {{
    .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &service_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[]) {
        {.uuid = &cmd_uuid.u, .access_cb = access_cb, .arg = (void *)CH_CMD,
         .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
        {.uuid = &reply_uuid.u, .access_cb = access_cb, .val_handle = &reply_handle,
         .flags = BLE_GATT_CHR_F_NOTIFY},
        {.uuid = &status_uuid.u, .access_cb = access_cb, .arg = (void *)CH_STATUS, .val_handle = &status_handle,
         .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY},
        {.uuid = &video_uuid.u, .access_cb = access_cb, .val_handle = &video_handle,
         .flags = BLE_GATT_CHR_F_NOTIFY},
        {.uuid = &net_uuid.u, .access_cb = access_cb, .arg = (void *)CH_NET,
         .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC},
        {0}
    },
},
{
    // The CLI service, for a computer. Deliberately unencrypted: no *_ENC flags,
    // so no OS bond forms and no pairing prompt appears. This is the channel the
    // repository's own Go CLI uses. It is also what makes the board testable from
    // a desk, since the App's service refuses everything at the ATT layer until
    // the link is encrypted.
    .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &cli_svc_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[]) {
        {.uuid = &cli_in_uuid.u, .access_cb = access_cb, .arg = (void *)CH_CLI_IN, .val_handle = &cli_in_handle,
         .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid = &cli_out_uuid.u, .access_cb = access_cb, .val_handle = &cli_out_handle,
         .flags = BLE_GATT_CHR_F_NOTIFY},
        {0}
    },
}, {0}};
static int gap_event(struct ble_gap_event *event, void *arg);
// Alternate the advertised service between the App's (...0000) and the CLI's
// (...0010). Both clients filter their scan by service UUID — the iOS App looks
// for ...0000, the repository's Go CLI for ...0010 — and two 128-bit UUIDs do
// not fit in the 31-byte advertisement payload. Putting both there fails with
// BLE_HS_EMSGSIZE and the board becomes invisible to everyone. The original
// firmware alternates for exactly this reason; the timings match it. One
// address throughout, so no bond ever goes stale.
#define ADV_APP_MS 750
#define ADV_CLI_MS 250
static bool adv_cli_phase;

static void advertise(void) {
    struct ble_hs_adv_fields fields = {0}, scan = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = adv_cli_phase ? &cli_svc_uuid : &service_uuid;
    fields.num_uuids128 = 1; fields.uuids128_is_complete = 1;
    const char *name = ble_svc_gap_device_name();
    scan.name = (const uint8_t *)name; scan.name_len = strlen(name); scan.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (!rc) rc = ble_gap_adv_rsp_set_fields(&scan);
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND; params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // A bounded duration, so ADV_COMPLETE fires and the service can be switched.
    if (!rc) rc = ble_gap_adv_start(own_addr_type, NULL,
                                   adv_cli_phase ? ADV_CLI_MS : ADV_APP_MS,
                                   &params, gap_event, NULL);
    if (rc) ESP_LOGE(TAG, "advertising failed: %d", rc);
    else ESP_LOGI(TAG, "advertising %s service", adv_cli_phase ? "CLI" : "App");
}
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) { ESP_LOGW(TAG, "connect failed: %d", event->connect.status); advertise(); }
        else {
            ESP_LOGI(TAG, "conn %u up", event->connect.conn_handle);
            if (active.conn != event->connect.conn_handle) {
                active.conn = event->connect.conn_handle; ++active.generation; subscriptions = 0;
            }
            // Ask for encryption as soon as the link is up, NOT when an
            // encrypted characteristic is touched. An *_ENC characteristic is
            // refused by the ATT layer before access_cb runs, so initiating
            // pairing there never happens and the central is left with
            // "Encryption is insufficient" and no pairing prompt. That is
            // exactly what the iPhone reported.
            if (active.conn != BLE_HS_CONN_HANDLE_NONE) request_pairing_if_plain(active.conn);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        app.disconnected(active);
        active.conn = BLE_HS_CONN_HANDLE_NONE;
        ++active.generation;
        subscriptions = 0;
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        if (active.conn == BLE_HS_CONN_HANDLE_NONE) {
            active.conn = event->subscribe.conn_handle; ++active.generation; subscriptions = 0;
        }
        if (event->subscribe.conn_handle != active.conn) break;
        uint16_t h = event->subscribe.attr_handle;
        unsigned bit = h == reply_handle ? 1u : h == status_handle ? 2u
                     : h == video_handle ? 4u : h == cli_out_handle ? 8u : 0;
        if (event->subscribe.cur_notify) subscriptions |= bit; else subscriptions &= ~bit;
        break;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_cli_phase = !adv_cli_phase;   // hand the slot to the other client
        advertise();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        uint16_t conn = event->enc_change.conn_handle;
        if (event->enc_change.status || ble_gap_conn_find(conn, &d) || !d.sec_state.encrypted || !d.sec_state.bonded) break;
        if (known_peer(&d.peer_id_addr)) break;
        bool added = false;
        bool in_window = esp_timer_get_time() - pair_opened < PAIR_WINDOW_US;
        portENTER_CRITICAL(&known_lock);
        if (in_window && known_count < MAX_BONDS) {
            known[known_count++] = d.peer_id_addr;
            added = true;
        }
        portEXIT_CRITICAL(&known_lock);
        if (!added) {
            ble_store_util_delete_peer(&d.peer_id_addr);
            // Say why, or a failed pairing attempt is invisible in the log and
            // looks like the board simply not being there.
            int64_t left_ms = (PAIR_WINDOW_US - (esp_timer_get_time() - pair_opened)) / 1000;
            if (!in_window) ESP_LOGW(TAG, "pairing refused: window closed %llds ago, power-cycle to reopen", -left_ms / 1000);
            else if (known_count >= MAX_BONDS) ESP_LOGW(TAG, "pairing refused: %d bonds stored, `unpair` to make room", MAX_BONDS);
            else ESP_LOGW(TAG, "pairing refused");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            ESP_LOGI(TAG, "peer bonded and accepted");
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: return BLE_GAP_REPEAT_PAIRING_IGNORE;
    default: break;
    }
    return 0;
}
static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (!rc) rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc) { ESP_LOGE(TAG, "BLE address setup failed: %d", rc); return; }
    advertise();
}
static void on_reset(int reason) { ESP_LOGE(TAG, "NimBLE reset: %d", reason); }
static void host_task(void *arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }
static int store_status(struct ble_store_status_event *event, void *arg) {
    (void)arg;
    // Never evict an existing bond to make room for a new peer.
    return event->event_code == BLE_STORE_EVENT_FULL ? 0 : BLE_HS_ESTORE_CAP;
}

esp_err_t p4_ble_unpair(void) {
    portENTER_CRITICAL(&known_lock);
    known_count = 0;
    portEXIT_CRITICAL(&known_lock);
    int rc = ble_store_clear();
    pair_opened = esp_timer_get_time();   // reopen the window immediately
    if (active.conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(active.conn, BLE_ERR_REM_USER_CONN_TERM);
        active.conn = BLE_HS_CONN_HANDLE_NONE;
    }
    ESP_LOGI(TAG, "bonds cleared, pairing window reopened");
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t p4_ble_start(const p4_ble_ops *ops) {
    if (started) return ESP_ERR_INVALID_STATE;
    if (!ops || !ops->command || !ops->net_read || !ops->net_write || !ops->status_read || !ops->disconnected) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    app = *ops;
    ble_hs_cfg.sync_cb = on_sync; ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1; ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = store_status;
    ble_svc_gap_init(); ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set("ESP32 Agent P4");
    if (!rc) rc = ble_gatts_count_cfg(services);
    if (!rc) rc = ble_gatts_add_svcs(services);
    if (rc) { nimble_port_deinit(); return ESP_FAIL; }
    ble_store_config_init();
    known_count = 0;
    rc = ble_store_util_bonded_peers(known, &known_count, MAX_BONDS);
    if (rc) { nimble_port_deinit(); return ESP_FAIL; }
    pair_opened = esp_timer_get_time();
    started = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
static int notify(p4_ble_session session, uint16_t handle, const void *data, size_t len) {
    unsigned bit = handle == reply_handle ? 1u : handle == status_handle ? 2u
                 : handle == video_handle ? 4u : handle == cli_out_handle ? 8u : 0;
    // The CLI channel is unencrypted by design, so it is exempt from the auth gate.
    bool cli = handle == cli_out_handle;
    if (!data || !len || len > UINT16_MAX || session.conn != active.conn ||
        session.generation != active.generation || !(subscriptions & bit) ||
        (!cli && !authorized(session.conn))) {
        ESP_LOGW(TAG, "notify refused h=%u cli=%d conn=%u/%u gen=%u/%u sub=%u",
                 handle, (int)cli, session.conn, active.conn,
                 session.generation, active.generation, subscriptions);
        return BLE_HS_EINVAL;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    return om ? ble_gatts_notify_custom(session.conn, handle, om) : BLE_HS_ENOMEM;
}
p4_ble_session p4_ble_active_session(void) { return active; }

int p4_ble_reply(p4_ble_session session, const void *data, size_t len) { return notify(session, reply_handle, data, len); }
int p4_ble_status(p4_ble_session session, const void *data, size_t len) { return notify(session, status_handle, data, len); }
int p4_ble_video(p4_ble_session session, const void *data, size_t len) { return notify(session, video_handle, data, len); }
// Replies on the unencrypted CLI channel.
int p4_ble_cli_out(p4_ble_session session, const void *data, size_t len) { return notify(session, cli_out_handle, data, len); }
