// ESP-IDF host for the esp32-iphone-agent portable core.
//
// Milestone 1 of the Metalio Claw4 port: prove that the chip-agnostic C core in
// ../hid_bridge/src compiles and RUNS on ESP32-P4 unchanged. Those files were
// already host-compiled with ASan/UBSan by hardware/tests/Makefile, which is
// why they are the right thing to port first.
//
// Nothing here is the final firmware. It is a self-test: each module is driven
// with real data and prints PASS/FAIL, so "the core works on P4" is a claim
// backed by output rather than by a successful compile alone.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bplist/bplist.h"
#include "video/gop.h"
#include "cli/cli_proto.h"
#include "hid/arbiter.h"
#include "log/logring.h"
#include "esp_hosted.h"
#include "esp_hosted_bt_host_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "airplay/airplay.h"
#include "p4_usb_hid.h"
#include "p4_ble.h"
#include "p4_cmd_bridge.h"
#include "p4_serial_cmds.h"

static const char *TAG = "core";

// --- net characteristic (SoftAP credentials + vtok) -------------------------
// The App reads this over the encrypted link to learn the hotspot password, and
// writes it to switch AP/STA. Filled in once Wi-Fi is up; empty until then, which
// is the same as the original firmware before its first `net` read.
static bool net_ready = false;
static char net_json[192];

static int net_read(uint8_t *out, size_t cap, size_t *len) {
  if (!net_ready || !out || !len || cap == 0) return -1;
  size_t n = strlen(net_json);
  if (n > cap) n = cap;
  memcpy(out, net_json, n);
  *len = n;
  return 0;
}

static int net_write(const uint8_t *data, size_t len) {
  // Accepted but not acted on yet: switching AP/STA needs the Wi-Fi stack wired
  // to a persistent config, which is the next step. Say so rather than lying.
  ESP_LOGW(TAG, "net write (%u bytes) ignored: AP/STA switching not wired yet", (unsigned)len);
  (void)data;
  return -1;
}

static int status_read(uint8_t *out, size_t cap, size_t *len) {
  if (!out || !len || cap == 0) return -1;
  // The compact form: the full JSON does not fit a small MTU, and the App treats
  // a short status the same way the original firmware's reduced object does.
  int n = snprintf((char *)out, cap,
                   "{\"fw\":\"p4\",\"usb\":\"%s\",\"net\":\"%s\",\"psram\":%u}",
                   p4_usb_hid_ready() ? "ok" : "off",
                   net_ready ? "ap" : "none",
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  if (n < 0) return -1;
  *len = (size_t)n < cap ? (size_t)n : cap;
  return 0;
}

// cli_proto.c takes its randomness from the caller; on the board that is the RNG.
void cli_random(uint8_t *p, size_t n) { esp_fill_random(p, n); }

static int failures = 0;

// The GOP the AirPlay receiver feeds: 4 MB plus 8192 descriptors, the size the
// firmware asks for (video_link.cpp). Allocated from PSRAM in app_main, NOT as a
// static array — a 4 MB .bss lands in internal DRAM (768 KB on P4) and the link
// then fails with a misleading "--enable-non-contiguous-regions discards
// section" / "binary size has exceeded the limit". This board has 32 MB of PSRAM.
#define GOP_CAP_BYTES (4u * 1024 * 1024)
#define GOP_MAX_AUS   8192
static uint8_t *g_arena;
static gop_au *g_aus;
static gop_buf g_gop;
static uint32_t gop_fed = 0, gop_dropped = 0;
static void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "  %-46s %s", what, ok ? "PASS" : "FAIL");
  if (!ok) failures++;
}

// One Annex-B access unit: 00 00 00 01 | nal_header | payload
static size_t mk_au(uint8_t *out, uint8_t nal_type, size_t payload_len) {
  size_t n = 0;
  out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
  out[n++] = (uint8_t)((3u << 5) | nal_type);  // nal_ref_idc 3
  for (size_t i = 0; i < payload_len; i++) out[n++] = (uint8_t)(i & 0xFF);
  return n;
}

// ---------------------------------------------------------------- gop / video

static void test_gop(void) {
  ESP_LOGI(TAG, "gop / video chunker");
  static uint8_t arena[64 * 1024];
  static gop_au aus[16];
  gop_buf g;
  gop_init(&g, arena, sizeof arena, aus, 16);
  check("gop_init leaves the buffer empty", g.n == 0 && g.used == 0);

  // Scratch buffers are static on purpose: this runs once at boot, so there is no
  // reason to burn ~1.7 KB of the main task's 4 KB stack on them. (Doing so
  // overflowed it and tripped the P4 stack guard.)
  static uint8_t cfg[32];
  size_t cn = mk_au(cfg, 7 /* SPS */, 12);  // nal type 7 = SPS
  gop_set_config(&g, cfg, cn, 1000);
  check("gop_set_config stores SPS+PPS", g.cfg_len == cn);

  static uint8_t idr[512], p[512];
  size_t in = mk_au(idr, 5 /* IDR */, 400);
  size_t pn = mk_au(p, 1 /* non-IDR */, 200);

  int i0 = gop_push(&g, idr, in, true, 2000);
  int i1 = gop_push(&g, p, pn, false, 2033);
  check("gop_push accepts the IDR AU", i0 == 0);
  check("gop_push accepts the following P AU", i1 == 1);
  check("gop_push counts 2 AUs", g.n == 2);
  check("gop used bytes cover both AUs + config", g.used >= in + pn);

  check("annexb_has_idr sees the IDR AU", annexb_has_idr(idr, in));
  check("annexb_has_idr rejects the P AU", !annexb_has_idr(p, pn));
  check("gop_find locates a timestamp", gop_find(&g, 2033) == 1);
  check("gop_find misses an unknown timestamp", gop_find(&g, 9999) == -1);

  // Chunk the IDR AU the way the BLE/Wi-Fi video link does.
  vchunk c;
  vchunk_begin(&c, idr, in, VF_START | VF_END | VF_IDR | VF_CONFIG, 2000);
  static uint8_t out[600];
  uint16_t seq = 1;
  size_t total = 0;
  int chunks = 0;
  for (;;) {
    size_t n = vchunk_next(&c, out, sizeof out, seq);
    if (n == 0) break;
    total += n - VCHUNK_HDR;
    chunks++;
    seq++;
    if (chunks > 64) break;  // guard against a runaway
  }
  check("vchunk_next emits at least one chunk", chunks >= 1);
  // The START chunk carries a u32 LE board timestamp before the Annex-B data
  // (docs/ble-protocol.md), so the chunker emits 4 bytes more than the AU.
  check("vchunk carries the whole AU plus its timestamp", total == in + 4);
  check("vchunk advances seq per notify", seq == (uint16_t)(1 + chunks));

  // AVCC -> Annex-B in place. AVCC length prefixes are 4-byte BIG-endian; writing
  // them with memcpy() would emit native (little-endian) order, which
  // avcc_to_annexb correctly rejects as "lengths do not tile the buffer".
  static uint8_t avcc[64];
  size_t an = 0;
  static const uint8_t l0[4] = {0, 0, 0, 8};
  static const uint8_t l1[4] = {0, 0, 0, 6};
  memcpy(avcc + an, l0, 4); an += 4;
  avcc[an++] = 0x65; for (int i = 0; i < 7; i++) avcc[an++] = (uint8_t)i;
  memcpy(avcc + an, l1, 4); an += 4;
  avcc[an++] = 0x41; for (int i = 0; i < 5; i++) avcc[an++] = (uint8_t)(0x80 + i);
  bool conv = avcc_to_annexb(avcc, an);
  check("avcc_to_annexb rewrites the buffer", conv);
  check("avcc_to_annexb output is Annex-B", avcc[0] == 0 && avcc[1] == 0 && avcc[2] == 0 && avcc[3] == 1);
  check("avcc_to_annexb keeps the IDR NAL", annexb_has_idr(avcc, an));
}

// ------------------------------------------------------------------ cli crypto

static void test_cli(void) {
  ESP_LOGI(TAG, "cli link crypto (X25519 pairing, AES-128-GCM frames)");
  uint8_t b_priv[32], c_priv[32], c_pub[32], b_pub[32], k[CLI_KEY_LEN];
  cli_random(b_priv, sizeof b_priv);
  cli_random(c_priv, sizeof c_priv);
  cli_random(c_pub, sizeof c_pub);
  bool paired = cli_pair(b_priv, c_pub, b_pub, k);
  check("cli_pair derives a board key", paired);
  bool distinct = memcmp(b_pub, b_priv, 32) != 0;
  check("cli_pair publishes a different public key", distinct);

  uint8_t c_nonce[16], b_nonce[16], k_c2b[16], k_b2c[16];
  cli_random(c_nonce, sizeof c_nonce);
  cli_random(b_nonce, sizeof b_nonce);
  bool ok = cli_session(k, c_nonce, b_nonce, k_c2b, k_b2c);
  check("cli_session derives both directions", ok);
  check("cli_session keys differ per direction", memcmp(k_c2b, k_b2c, 16) != 0);

  const char *msg = "12 click";
  static uint8_t frame[128], plain[128];
  size_t flen = cli_seal(k_c2b, CLI_DATA, 'C', 1, (const uint8_t *)msg, strlen(msg), frame);
  check("cli_seal produces a frame with overhead", flen == strlen(msg) + CLI_OVERHEAD);

  uint32_t ctr = 0;
  int plen = cli_open(k_c2b, 'C', frame, flen, &ctr, plain);
  check("cli_open verifies and decrypts", plen == (int)strlen(msg));
  check("cli_open returns the plaintext", plen > 0 && memcmp(plain, msg, (size_t)plen) == 0);
  check("cli_open reports the counter", ctr == 1);

  // A tampered tag must not verify.
  frame[flen - 1] ^= 0xFF;
  int bad = cli_open(k_c2b, 'C', frame, flen, &ctr, plain);
  check("cli_open rejects a tampered frame", bad == -1);

  // The wrong direction key must not verify either.
  frame[flen - 1] ^= 0xFF;  // undo
  int wrong = cli_open(k_b2c, 'C', frame, flen, &ctr, plain);
  check("cli_open rejects the wrong direction key", wrong == -1);
}

// -------------------------------------------------------------------- arbiter

static void test_arbiter(void) {
  ESP_LOGI(TAG, "hid arbiter (who drives the phone)");
  arb_state s;
  arb_init(&s);
  check("arb_init has nobody pressing", s.pressed == ARB_NONE);
  check("app alone owns the board", arb_owner(true, false) == ARB_APP);
  check("cli alone owns the board", arb_owner(false, true) == ARB_CLI);
  check("nobody owns it", arb_owner(false, false) == ARB_NONE);
  check("the app wins over the cli", arb_owner(true, true) == ARB_APP);
  check("cli is refused while the app is up", !arb_allowed(ARB_CLI, true));
  check("cli is allowed when the app is down", arb_allowed(ARB_CLI, false));

  arb_after(&s, ARB_APP, "press", true);
  check("arb_after records the press", s.pressed == ARB_APP);
  bool held = arb_gone(&s, ARB_APP);
  check("arb_gone reports the held button", held);
  check("arb_gone clears the press", s.pressed == ARB_NONE);
}

// --------------------------------------------------------------------- logring

static void test_logring(void) {
  ESP_LOGI(TAG, "ram log ring");
  static char buf[256];
  logring r;
  logring_init(&r, buf, sizeof buf);
  logring_add(&r, "first line");
  logring_add(&r, "second line");
  static char out[256];
  uint32_t next = logring_read(&r, 0, out, sizeof out);
  check("logring_read returns the kept bytes", strlen(out) > 0);
  check("logring_read escapes newlines", strstr(out, "\\n") != NULL);
  check("logring_read has both lines", strstr(out, "first line") != NULL && strstr(out, "second line") != NULL);
  check("logring_read advances the offset", next > 0);
}

// ---------------------------------------------------------------------- bplist

static void test_bplist(void) {
  ESP_LOGI(TAG, "binary plist");
  // A minimal valid bplist: header, one ASCII string "hi", a 1-byte offset table
  // and the 32-byte trailer. Built by hand so the test has no hidden deps.
  // Marker 0x5N: high nibble 5 = ASCII string, LOW nibble = length, so "hi" is 0x52.
  static const uint8_t plist[] = {
      'b','p','l','i','s','t','0','0',          // 0-7   header
      0x52, 'h', 'i',                           // 8-10  ASCII string, length 2
      0x08,                                     // 11    offset table: object 0 @ 8
      0,0,0,0,0,                                // 12-16 unused
      0,                                        // 17    sortVersion
      1,                                        // 18    offsetIntSize
      1,                                        // 19    objectRefSize
      0,0,0,0,0,0,0,1,                          // 20-27 numObjects = 1
      0,0,0,0,0,0,0,0,                          // 28-35 topObject  = 0
      0,0,0,0,0,0,0,11,                         // 36-43 offsetTableOffset = 11
  };
  bp_reader r;
  int root = bp_open(&r, plist, sizeof plist);
  check("bp_open accepts the plist", root >= 0);
  check("bp_open reports a string root", root >= 0 && bp_type(&r, root) == BP_STRING);
  static char out[64];
  bool got = bp_get_string(&r, root, out, sizeof out);
  check("bp_get_string reads the text", got && strcmp(out, "hi") == 0);
}

// ------------------------------------------------------- hosted C5 over SDIO

// The P4 has no radio. The board pairs it with an ESP32-C5 over 4-bit SDIO, and
// esp_hosted bridges esp_wifi (and BLE, once enabled on both sides) to it. This
// is the one thing that cannot be unit-tested on the host: it needs the real
// slave on the other end of the bus.
static void test_hosted(void) {
  ESP_LOGI(TAG, "esp-hosted / ESP32-C5 over SDIO");
  // Do NOT call esp_hosted_connect_to_slave() here: the auto-init
  // (CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN) has already brought the
  // link up before app_main, and calling it again false-fails — the same trap the
  // board's own firmware documents ("public getter may false-fail on
  // transport_up flag"). Query the slave instead, which reflects the real state.
  // The auto-init is asynchronous: the SDIO transport is up ~1.7 s after boot,
  // while app_main runs at ~1.3 s. Poll rather than sampling once.
  uint32_t chip_id = 0;
  char name[32] = {0};
  int rc = -1;
  for (int i = 0; i < 40 && rc != 0; i++) {
    rc = esp_hosted_get_cp_info(&chip_id, name, sizeof name);
    if (rc != 0) vTaskDelay(pdMS_TO_TICKS(250));
  }
  check("esp_hosted_get_cp_info reports the slave", rc == 0);
  if (rc == 0) {
    ESP_LOGI(TAG, "  slave chip_id=0x%08x target=%s", (unsigned)chip_id, name);
    check("the slave is the expected ESP32-C5", strcmp(name, "esp32c5") == 0);
  } else {
    ESP_LOGW(TAG, "  get_cp_info failed (%d) — link not up yet", rc);
  }
}

// ---------------------------------------------------------- hosted BLE (NimBLE)

// BLE rides the same SDIO bus as Wi-Fi: the C5's firmware already advertises
// "HCI over SDIO", so only the host side has to be enabled. The controller is
// on the C5, so BT_CONTROLLER_DISABLED is set and this just binds the NimBLE
// host stack to the hosted HCI pipe.
static void test_hosted_bt(void) {
  ESP_LOGI(TAG, "hosted BLE / NimBLE over SDIO HCI");
  esp_hosted_bt_host_stack_cfg_t cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
  cfg.stack = ESP_HOSTED_BT_HOST_STACK_NIMBLE;
  cfg.bring_up_controller = true;   // init + enable the controller on the C5
  esp_err_t r = esp_hosted_bt_host_stack_setup(&cfg);
  check("esp_hosted_bt_host_stack_setup succeeds", r == ESP_OK);
  if (r != ESP_OK) {
    ESP_LOGE(TAG, "  hosted BT setup failed: 0x%x (%s)", r, esp_err_to_name(r));
    return;
  }
  // The controller lives on the C5, so there is no local BT MAC to read here —
  // esp_read_mac(ESP_MAC_WIFI_STA) is all zeroes until Wi-Fi is brought up. The
  // meaningful signal is the bind itself, which the adapter logs as
  // "NimBLE bound to hosted HCI" only after the controller answered over HCI.
  ESP_LOGI(TAG, "  NimBLE host stack is bound to the C5's HCI over SDIO");
}


// ------------------------------------------------------- AirPlay receiver

// The board mirrors the iPhone's screen over AirPlay. This wires the receiver's
// output into the GOP buffer that the video link already chunks and ships to the
// app, so the two halves meet here for the first time.
static void on_config(const uint8_t *sps_pps, size_t len) {
  ESP_LOGI(TAG, "airplay: SPS/PPS (%u bytes)", (unsigned)len);
  gop_set_config(&g_gop, sps_pps, len, (uint32_t)(esp_timer_get_time() / 1000));
}

static void on_au(const uint8_t *au, size_t len, bool idr) {
  int idx = gop_push(&g_gop, au, len, idr, (uint32_t)(esp_timer_get_time() / 1000));
  if (idx < 0) { gop_dropped++; return; }   // wait_idr or a full buffer: dropped
  gop_fed++;
}

// Keep the receiver's pairing identity across reboots, as the S3 firmware does.
// Never erase NVS on an error: it can also contain network and pairing settings.
static esp_err_t load_airplay_seed(uint8_t seed[32]) {
  esp_err_t e = nvs_flash_init();
  if (e != ESP_OK) return e;
  nvs_handle_t nvs;
  e = nvs_open("airplay", NVS_READWRITE, &nvs);
  if (e != ESP_OK) return e;
  size_t len = 32;
  e = nvs_get_blob(nvs, "ed_seed", seed, &len);
  if (e == ESP_ERR_NVS_NOT_FOUND) {
    esp_fill_random(seed, 32);
    e = nvs_set_blob(nvs, "ed_seed", seed, 32);
    if (e == ESP_OK) e = nvs_commit(nvs);
  } else if (e == ESP_OK && len != 32) {
    e = ESP_ERR_INVALID_SIZE;
  }
  nvs_close(nvs);
  return e;
}

static void test_airplay(void) {
  ESP_LOGI(TAG, "airplay receiver (RTSP + mDNS + mirror)");

  // AirPlay needs an IP: the iPhone joins this SoftAP and mirrors to it. Without
  // a netif the mDNS init inside airplay_start() dereferences NULL, so bring the
  // AP up first. The radio is on the C5 — esp_wifi_remote drives it over SDIO.
  esp_err_t e = esp_netif_init();
  check("esp_netif_init succeeds", e == ESP_OK || e == ESP_ERR_INVALID_STATE);
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
  e = esp_wifi_init(&wic);
  check("esp_wifi_init succeeds", e == ESP_OK || e == ESP_ERR_INVALID_STATE);

  wifi_config_t ap = {};
  strcpy((char *)ap.ap.ssid, "ESP32 Agent");
  ap.ap.ssid_len = strlen("ESP32 Agent");
  strcpy((char *)ap.ap.password, "agent12345");
  ap.ap.max_connection = 4;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  e = esp_wifi_set_mode(WIFI_MODE_AP);
  check("esp_wifi_set_mode(AP) succeeds", e == ESP_OK);
  e = esp_wifi_set_config(WIFI_IF_AP, &ap);
  check("esp_wifi_set_config(AP) succeeds", e == ESP_OK);
  e = esp_wifi_start();
  check("esp_wifi_start succeeds", e == ESP_OK);
  if (e != ESP_OK) return;

  // Every field is required: ap_mdns_start() dereferences hwaddr unconditionally
  // (snprintf "%02X%02X...", m[0], ...) with no NULL check, so a zeroed cfg
  // crashes with a Load access fault that looks like an mDNS bug.
  airplay_cfg ac = {};
  ac.name = "ESP32 Agent";
  {
    // The advertised deviceid. Read the real MAC once Wi-Fi is up; fall back to a
    // fixed value so the receiver still has a stable identity if that fails.
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) memcpy(ac.hwaddr, mac, 6);
    else memcpy(ac.hwaddr, (uint8_t[]){0x80, 0xF1, 0xB2, 0xD2, 0xEC, 0x63}, 6);
  }
  e = load_airplay_seed(ac.ed25519_seed);
  check("AirPlay pairing identity loaded", e == ESP_OK);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "Cannot load AirPlay identity: %s", esp_err_to_name(e));
    return;
  }
  ac.width = 480; ac.height = 1040; ac.max_fps = 5;   // the firmware's defaults
  ac.on_config = on_config;
  ac.on_au = on_au;
  ac.log = NULL;
  bool ok = airplay_start(&ac);
  check("airplay_start succeeds", ok);
  if (ok) {
    check("airplay_state is IDLE before mirroring", airplay_state() == AIRPLAY_IDLE);
    ESP_LOGI(TAG, "  RTSP :7000  mirror :7100  mDNS _airplay/_raop  ready for the iPhone");
  }
}

// ----------------------------------------------------------------------- main

void app_main(void) {
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  ESP_LOGI(TAG, "=== esp32-iphone-agent portable core on ESP32-P4 ===");
  ESP_LOGI(TAG, "chip model=%d rev=%d cores=%d psram=%uKB free_spiram=%uKB",
           (int)ci.model, ci.revision, ci.cores,
           heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024,
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

#if CONFIG_P4_BLE_GATT_SERVICE
  // The service needs NVS (bond storage) and the hosted C5 link before it can
  // advertise, so bring those up first. It does NOT return early any more: the
  // diagnostic that this block grew out of had to avoid touching Wi-Fi, but the
  // real service wants AirPlay and the SoftAP running underneath it.
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err != ESP_OK) ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_err));

  test_hosted();
  test_hosted_bt();

  // Order matters: p4_ble_start() runs nimble_port_init(), which is what fills
  // the npl_funcs table that every ble_npl_* call goes through. Starting the
  // bridge first dereferences that table while it is still NULL.
  const p4_ble_ops ble_ops = {
    .command = p4_cmd_bridge_command, .net_read = net_read, .net_write = net_write,
    .status_read = status_read, .disconnected = p4_cmd_bridge_disconnected,
  };
  esp_err_t ble_err = p4_ble_start(&ble_ops);
  if (ble_err != ESP_OK) {
    ESP_LOGE(TAG, "BLE GATT start failed: %s", esp_err_to_name(ble_err));
  } else {
    esp_err_t bridge_err = p4_cmd_bridge_start();
    if (bridge_err != ESP_OK) ESP_LOGE(TAG, "command bridge failed: %s", esp_err_to_name(bridge_err));
    // The serial path is independent of BLE, so it works even when no central
    // will complete pairing.
    if (p4_serial_cmds_start() != ESP_OK) ESP_LOGE(TAG, "serial commands failed to start");
  }
#endif

  // The GOP lives in PSRAM; without it the AirPlay receiver has nowhere to put
  // frames, so say so plainly rather than crashing later.
  g_arena = heap_caps_malloc(GOP_CAP_BYTES, MALLOC_CAP_SPIRAM);
  g_aus = heap_caps_malloc(sizeof(gop_au) * GOP_MAX_AUS, MALLOC_CAP_SPIRAM);
  check("4 MB GOP arena allocated in PSRAM", g_arena != NULL);
  check("GOP descriptor array allocated", g_aus != NULL);
  if (g_arena && g_aus) gop_init(&g_gop, g_arena, GOP_CAP_BYTES, g_aus, GOP_MAX_AUS);

  test_gop();
  test_cli();
  test_arbiter();
  test_logring();
  test_bplist();
  test_hosted();
  test_hosted_bt();
  test_airplay();

  if (gop_fed || gop_dropped) {
    ESP_LOGI(TAG, "airplay fed %u AUs into the GOP (%u dropped)", (unsigned)gop_fed, (unsigned)gop_dropped);
  }
  ESP_LOGI(TAG, "=== %s (%d failure%s) ===",
           failures == 0 ? "ALL PASS" : "FAILURES", failures, failures == 1 ? "" : "s");
#if CONFIG_P4_USB_HID_DIAGNOSTIC
  ESP_LOGI(TAG, "Starting USB HID for 30 seconds: USB Serial/JTAG will disconnect");
  esp_err_t hid_err = p4_usb_hid_start();
  if (hid_err == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(30000));
    hid_err = p4_usb_hid_stop();
  }
  if (hid_err != ESP_OK) ESP_LOGE(TAG, "USB HID diagnostic failed: %s", esp_err_to_name(hid_err));
#endif
}
