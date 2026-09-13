# Create an INTERFACE library for our C module.
#
# micropython.cmake — mp_LEDdriver (module name: led_bus)
#
# 目標:
#   ESP-IDF 原生多 LED 總線驅動（WS2812 / APA102 / PCA9685）。
#   技術棧優先序：ESP-IDF 原生 > MicroPython 原生 > 自寫 C > 不用 Arduino。
#
# 掛載（mp_Make-Tools 的 exmod.list）：
#   "/mp_LEDdriver/mp_led_bus/micropython.cmake"

add_library(usermod_mp_led_bus INTERFACE)

set(INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}
)

set(SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/led_alloc.c
    ${CMAKE_CURRENT_LIST_DIR}/modled_bus.c
)

# 拉進 ESP-IDF 內建元件的 include 路徑（照 mp_jpeg / mp_rs485_hd 的手法）。
# 這些都是 IDF 內建元件，不需要 idf_component.yml。
if(ESP_PLATFORM)
    set(LED_IDF_COMPONENTS
        esp_lcd              # I80 (WS2812) + RGB panel
        esp_driver_spi       # APA102
        esp_driver_i2c       # PCA9685
        esp_driver_rmt       # WS2812 備援（≤4 lane）
        esp_driver_ledc      # PWM（若日後需要）
        esp_mm               # esp_cache / heap
        esp_common           # esp_heap_caps 等
        soc                  # soc_caps.h
        hal                  # hal/lcd_ll.h 等
    )
    foreach(comp ${LED_IDF_COMPONENTS})
        idf_component_get_property(_inc ${comp} INCLUDE_DIRS)
        idf_component_get_property(_dir ${comp} COMPONENT_DIR)
        if(_inc)
            list(TRANSFORM _inc PREPEND ${_dir}/)
            list(APPEND INCLUDES ${_inc})
        endif()
    endforeach()

    # PARLIO 只在 P4/C6/H2/C5 存在 —— 條件式拉進來，避免 S3 編譯失敗
    if(CONFIG_IDF_TARGET_ESP32P4 OR CONFIG_IDF_TARGET_ESP32C6
       OR CONFIG_IDF_TARGET_ESP32H2 OR CONFIG_IDF_TARGET_ESP32C5)
        idf_component_get_property(_inc esp_driver_parlio INCLUDE_DIRS)
        idf_component_get_property(_dir esp_driver_parlio COMPONENT_DIR)
        if(_inc)
            list(TRANSFORM _inc PREPEND ${_dir}/)
            list(APPEND INCLUDES ${_inc})
        endif()
        target_compile_definitions(usermod_mp_led_bus INTERFACE LED_BUS_HAS_PARLIO=1)
    endif()
endif(ESP_PLATFORM)

# Add our source files to the lib
target_sources(usermod_mp_led_bus INTERFACE ${SOURCES})

# Add include directories.
target_include_directories(usermod_mp_led_bus INTERFACE ${INCLUDES})

# 模組字串不適合壓縮，且壓縮會讓版本字串膨脹（照 mp_jpeg 的做法）
target_compile_definitions(usermod_mp_led_bus INTERFACE
    MICROPY_ROM_TEXT_COMPRESSION=0
    LED_BUS_VERSION="0.1.0-C0"
)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_mp_led_bus)

# Gather target properties for MicroPython build system
micropy_gather_target_properties(usermod_mp_led_bus)
