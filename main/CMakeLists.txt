idf_component_register(
    SRCS "app_main.c"
         "focus_logic.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl nvs_flash esp_timer esp_hw_support
)
