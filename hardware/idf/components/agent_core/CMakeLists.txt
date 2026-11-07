# agent_core — the chip-agnostic C core of the esp32-iphone-agent firmware.
#
# These files are taken UNCHANGED from ../hid_bridge/src. They were already
# host-compiled and unit-tested by hardware/tests/Makefile (ASan/UBSan), which
# is the evidence that they carry no ESP32-S3 or Arduino dependency: they only
# use lwIP, FreeRTOS, mbedTLS, esp_timer and the IDF mdns component.
#
# Deliberately NOT here yet: the AirPlay server (fairplay/handlers/mirror/
# pairing/rtsp + the GPL playfair tree) — those need lwIP socket servers and
# libsodium, and are added once the core links.

# agent_core/ -> components/ -> idf/ -> hardware/  (then the shared src tree)
set(CORE "${CMAKE_CURRENT_LIST_DIR}/../../../hid_bridge/src")

# --- the AirPlay mirroring receiver (a port of UxPlay/RPiPlay, GPL-3.0) -----
# These need lwIP socket servers, mbedTLS, libsodium and the IDF mdns component.
# src/airplay/LICENSE-GPL.md keeps the original GPL notice; playfair/ is vendored
# GPL code from the same upstream.
set(AIRPLAY
        "${CORE}/airplay/rtsp.c"
        "${CORE}/airplay/handlers.c"
        "${CORE}/airplay/pairing.c"
        "${CORE}/airplay/fairplay.c"
        "${CORE}/airplay/mirror.c"
        "${CORE}/airplay/playfair/hand_garble.c"
        "${CORE}/airplay/playfair/modified_md5.c"
        "${CORE}/airplay/playfair/omg_hax.c"
        "${CORE}/airplay/playfair/playfair.c"
        "${CORE}/airplay/playfair/sap_hash.c")

idf_component_register(
    SRCS
        # --- proven portable by hardware/tests/Makefile ---
        "${CORE}/bplist/bplist.c"
        "${CORE}/video/gop.c"
        "${CORE}/airplay/keystream.c"
        "${CORE}/cli/cli_proto.c"
        "${CORE}/hid/arbiter.c"
        "${CORE}/log/logring.c"
        "${CORE}/airplay/mdns.c"
        ${AIRPLAY}
    INCLUDE_DIRS
        "${CORE}"
    REQUIRES
        lwip
        mbedtls
        esp_timer
        esp_wifi
        mdns
        libsodium
)

# The vendored GPL playfair tree is third-party code we do not own: it predates
# modern GCC and trips -Werror=parentheses (a - b & c) and friends under
# -Werror=all. Warnings must not fail the build for code we are not maintaining.
set_source_files_properties(${AIRPLAY} PROPERTIES COMPILE_OPTIONS "-Wno-error;-Wno-parentheses")
