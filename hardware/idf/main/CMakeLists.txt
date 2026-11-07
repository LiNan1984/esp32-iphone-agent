idf_component_register(
    SRCS "main.c"
     "p4_cmd_bridge.c" "p4_cmd_split.c" "p4_serial_cmds.c" "p4_video.c" "p4_usb_hid.c" "p4_hid_commands.c" "p4_ble.c"
    INCLUDE_DIRS "."
    REQUIRES agent_core
             esp_timer
             esp_hw_support
             bt
             nvs_flash
             usb
             espressif__esp_tinyusb
             esp_driver_usb_serial_jtag
)

