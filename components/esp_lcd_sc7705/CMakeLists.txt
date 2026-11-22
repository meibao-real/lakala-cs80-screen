idf_component_register(
        SRC_DIRS
        "."


        INCLUDE_DIRS
        "."


        REQUIRES
        esp_lcd
)

# 版本宏由 CMake 直接定义
target_compile_definitions(${COMPONENT_LIB} PUBLIC
        ESP_LCD_SC7705_VER_MAJOR=1
        ESP_LCD_SC7705_VER_MINOR=0
        ESP_LCD_SC7705_VER_PATCH=0)
