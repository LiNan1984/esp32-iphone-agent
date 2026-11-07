# ESP-IDF port of the esp32-iphone-agent firmware, for the Metalio Claw 4
# (ESP32-P4 + ESP32-C5 over ESP-Hosted SDIO).
#
# The Arduino sketch in ../hid_bridge cannot build for this board:
# arduino-esp32's prebuilt P4 libraries are hard-wired to an ESP32-C6 SDIO
# slave and abort on the C5 ("Identified slave [esp32c5] != Expected
# [esp32c6]"). So the firmware is being re-hosted on ESP-IDF, which is also
# what the board's own firmware uses.
#
# Design: the portable C core in ../hid_bridge/src is compiled unchanged as the
# `agent_core` component; the Arduino-specific layer (TinyUSB HID objects,
# NimBLE host internals, HWCDC logging, xTaskCreatePinnedToCore pinning) is
# re-implemented in main/ against ESP-IDF APIs.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Our own components live in components/ next to this file (agent_core, and the
# esp-hosted BT host-stack adapter copied from its examples).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
project(iphone_agent_p4)
