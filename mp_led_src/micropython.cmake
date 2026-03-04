# Copyright (c) 2024 - 2025 Kevin G. Schlosser

# Create an INTERFACE library for our C module.

add_library(usermod_mp_led INTERFACE)

if(ESP_PLATFORM)
    set(LCD_INCLUDES
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/esp32_include
    )

    set(LCD_SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/modmp_led.c
        ${CMAKE_CURRENT_LIST_DIR}/lcd_types.c
        ${CMAKE_CURRENT_LIST_DIR}/esp32_src/i80_led_bus.c
    )

    # gets esp_lcd include paths
    idf_component_get_property(ESP_LCD_INCLUDES esp_lcd INCLUDE_DIRS)
    idf_component_get_property(ESP_LCD_DIR esp_lcd COMPONENT_DIR)

    # sets the include paths into INCLUDES variable
    if(ESP_LCD_INCLUDES)
        list(TRANSFORM ESP_LCD_INCLUDES PREPEND ${ESP_LCD_DIR}/)
        list(APPEND LCD_INCLUDES ${ESP_LCD_INCLUDES})
    endif()

else()
    # Non-ESP platforms not supported for this LED driver
    message(FATAL_ERROR "mp_led module only supports ESP32 platform")

endif(ESP_PLATFORM)


# Add our source files to the lib
target_sources(usermod_mp_led INTERFACE ${LCD_SOURCES})

# Add include directories.
target_include_directories(usermod_mp_led INTERFACE ${LCD_INCLUDES})

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_mp_led)
