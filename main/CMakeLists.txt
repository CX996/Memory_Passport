idf_component_register(
    SRCS "main.c"
         "demo_memory.c"
         "memory_model.c"
         "memory_store.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_font_chinese_16.c"
         "ui_pixel_math.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_system esp_timer nvs_flash
)
