// Copyright (c) 2024 - 2025 Kevin G. Schlosser

#ifndef _ESP32_I80_LED_BUS_H_
    #define _ESP32_I80_LED_BUS_H_

    //local_includes
    #include "lcd_types.h"

    // micropython includes
    #include "mphalport.h"
    #include "py/obj.h"
    #include "py/objarray.h"
    #include "py/objlist.h"

    // esp-idf includes
    #include "soc/soc_caps.h"

    #if SOC_LCD_I80_SUPPORTED
        // esp-idf includes
        #include "esp_lcd_panel_io.h"

        typedef struct _mp_led_i80_bus_obj_t {
            mp_obj_base_t base;

            mp_obj_t callback;

            mp_obj_array_t *view1; // DMA buffer
            mp_obj_array_t *view2; // Double buffer if needed

            uint32_t buffer_flags;
            uint32_t buffer_size;

            bool trans_done;
            
            lcd_panel_io_t panel_io_handle; // Function pointers

            esp_lcd_panel_io_i80_config_t panel_io_config;
            esp_lcd_i80_bus_config_t bus_config;
            esp_lcd_i80_bus_handle_t bus_handle;
            esp_lcd_panel_io_handle_t io_handle;

            mp_obj_t strips; // List of strips
        } mp_led_i80_bus_obj_t;

        extern const mp_obj_type_t mp_led_i80_bus_type;

        // Strip object
        typedef struct _mp_led_strip_obj_t {
            mp_obj_base_t base;
            uint8_t pin_index;
            uint16_t length;
            uint8_t type;
            mp_obj_array_t *buf; // The pixel buffer
            struct _mp_led_i80_bus_obj_t *bus; // Reference to bus
        } mp_led_strip_obj_t;

        extern const mp_obj_type_t mp_led_strip_type;

    #endif /* SOC_LCD_I80_SUPPORTED */
#endif /* _ESP32_I80_LED_BUS_H_ */
