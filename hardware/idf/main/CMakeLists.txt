idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES agent_core
             esp_timer
             esp_hw_support
             bt
             nvs_flash
)

