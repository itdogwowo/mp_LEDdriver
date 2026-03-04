// Copyright (c) 2024 - 2025 Kevin G. Schlosser

// local includes
#include "lcd_types.h"
#include "modmp_led.h"
#include "esp32_include/i80_led_bus.h"

// micropython includes
#include "mphalport.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/binary.h"
#include "py/objarray.h"

// stdlib includes
#include <string.h>

// esp-idf includes
#include "soc/soc_caps.h"

#if SOC_LCD_I80_SUPPORTED
    // esp-idf includes
    #include "esp_lcd_panel_io.h"
    #include "esp_heap_caps.h"
    #include "hal/lcd_types.h"
    #include "driver/gpio.h"

    // WS2812B Constants (at 10MHz)
    // T0H: 0.4us -> 4 ticks
    // T0L: 0.85us -> 9 ticks (Total 13)
    // T1H: 0.8us -> 8 ticks
    // T1L: 0.45us -> 5 ticks (Total 13)
    #define WS2812_T0H 4
    #define WS2812_T1H 8
    #define WS2812_PERIOD 13
    #define WS2812_RESET_TICKS 500 // >50us

    mp_lcd_err_t i80_led_del(mp_obj_t obj);
    mp_lcd_err_t i80_led_init(mp_obj_t obj, uint16_t width, uint16_t height, uint8_t bpp, uint32_t buffer_size, bool rgb565_byte_swap, uint8_t cmd_bits, uint8_t param_bits);
    mp_lcd_err_t i80_led_get_lane_count(mp_obj_t obj, uint8_t *lane_count);
    
    // Forward declarations
    static uint8_t i80_bus_count = 0;
    static mp_led_i80_bus_obj_t **i80_bus_objs = NULL;

    static bool i80_led_bus_trans_done_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
    {
        mp_led_i80_bus_obj_t *self = (mp_led_i80_bus_obj_t *)user_ctx;
        self->trans_done = true;
        if (self->callback != mp_const_none) {
            mp_sched_schedule(self->callback, MP_OBJ_FROM_PTR(self));
        }
        return false;
    }

    // Strip object implementation

    static mp_obj_t led_strip_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
        // This is not called directly usually, but via bus.add_strip
        mp_arg_check_num(n_args, n_kw, 0, 0, false);
        return mp_const_none;
    }
    
    static mp_obj_t led_strip_subscr(mp_obj_t self_in, mp_obj_t index, mp_obj_t value) {
        mp_led_strip_obj_t *self = MP_OBJ_TO_PTR(self_in);
        if (value == MP_OBJ_SENTINEL) {
            // Load
            size_t idx = mp_get_index(self->base.type, self->length, index, false);
            uint8_t *data = (uint8_t *)self->buf->items;
            mp_obj_t tuple[3];
            tuple[0] = mp_obj_new_int(data[idx * 3]);
            tuple[1] = mp_obj_new_int(data[idx * 3 + 1]);
            tuple[2] = mp_obj_new_int(data[idx * 3 + 2]);
            return mp_obj_new_tuple(3, tuple);
        } else {
            // Store
            size_t idx = mp_get_index(self->base.type, self->length, index, false);
            mp_obj_t *items;
            mp_obj_get_array_fixed_n(value, 3, &items);
            uint8_t *data = (uint8_t *)self->buf->items;
            data[idx * 3] = mp_obj_get_int(items[0]);
            data[idx * 3 + 1] = mp_obj_get_int(items[1]);
            data[idx * 3 + 2] = mp_obj_get_int(items[2]);
            return mp_const_none;
        }
    }

    static void led_strip_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
        mp_led_strip_obj_t *self = MP_OBJ_TO_PTR(self_in);
        if (dest[0] == MP_OBJ_NULL) {
            // Load
            if (attr == MP_QSTR_buf) {
                dest[0] = MP_OBJ_FROM_PTR(self->buf);
            }
        }
    }

    MP_DEFINE_CONST_OBJ_TYPE(
        mp_led_strip_type,
        MP_QSTR_Strip,
        MP_TYPE_FLAG_NONE,
        make_new, led_strip_make_new,
        subscr, led_strip_subscr,
        attr, led_strip_attr
    );

    // I8080 Bus Implementation

    static mp_obj_t mp_led_i80_bus_add_strip(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
        enum { ARG_pin_index, ARG_length, ARG_type };
        static const mp_arg_t allowed_args[] = {
            { MP_QSTR_pin_index, MP_ARG_INT | MP_ARG_REQUIRED },
            { MP_QSTR_length,    MP_ARG_INT | MP_ARG_REQUIRED },
            { MP_QSTR_type,      MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 0 } },
        };
        mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
        mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

        mp_led_i80_bus_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
        
        mp_led_strip_obj_t *strip = m_new_obj(mp_led_strip_obj_t);
        strip->base.type = &mp_led_strip_type;
        strip->pin_index = args[ARG_pin_index].u_int;
        strip->length = args[ARG_length].u_int;
        strip->type = args[ARG_type].u_int;
        strip->bus = self;

        // Allocate buffer for strip (R,G,B per pixel)
        mp_obj_array_t *bytearray = MP_OBJ_TO_PTR(mp_obj_new_bytearray(strip->length * 3, NULL));
        memset(bytearray->items, 0, strip->length * 3);
        strip->buf = bytearray;
        
        mp_obj_list_append(self->strips, MP_OBJ_FROM_PTR(strip));

        return MP_OBJ_FROM_PTR(strip);
    }
    static MP_DEFINE_CONST_FUN_OBJ_KW(mp_led_i80_bus_add_strip_obj, 1, mp_led_i80_bus_add_strip);

    static mp_obj_t mp_led_i80_bus_show(mp_obj_t self_in) {
        mp_led_i80_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
        
        // 1. Calculate max length
        size_t max_len = 0;
        size_t num_strips = ((mp_obj_list_t *)self->strips)->len;
        mp_obj_t *strip_objs = ((mp_obj_list_t *)self->strips)->items;
        
        for (size_t i = 0; i < num_strips; i++) {
            mp_led_strip_obj_t *strip = MP_OBJ_TO_PTR(strip_objs[i]);
            if (strip->length > max_len) max_len = strip->length;
        }

        if (max_len == 0) return mp_const_none;

        // 2. Allocate DMA buffer if needed
        // Size = (max_len * 24 * WS2812_PERIOD + WS2812_RESET_TICKS) * (bus_width / 8)
        // Assume 8-bit bus width for now (or 16-bit if configured)
        size_t bus_width_bytes = (self->bus_config.bus_width > 8) ? 2 : 1;
        size_t needed_size = (max_len * 24 * WS2812_PERIOD + WS2812_RESET_TICKS) * bus_width_bytes;

        if (self->view1 == NULL || self->buffer_size < needed_size) {
            if (self->view1 != NULL) {
                heap_caps_free(self->view1->items);
            }
            void *buf = heap_caps_calloc(1, needed_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (!buf) {
                 mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate DMA buffer"));
            }
            self->view1 = MP_OBJ_TO_PTR(mp_obj_new_memoryview(BYTEARRAY_TYPECODE, needed_size, buf));
            self->buffer_size = needed_size;
        }

        uint8_t *dma_buf = (uint8_t *)self->view1->items;
        memset(dma_buf, 0, needed_size); // Clear buffer (also handles reset code at end)

        // 3. Fill DMA buffer
        // Pre-calc patterns
        // We iterate pixel by pixel, bit by bit
        for (size_t p = 0; p < max_len; p++) {
            for (int b = 0; b < 24; b++) { // 24 bits: G7...G0, R7...R0, B7...B0 (GRB)
                // Wait, WS2812 is usually GRB.
                // We need to iterate 24 bits.
                // The pattern offset in DMA buffer:
                // offset = (p * 24 + b) * WS2812_PERIOD * bus_width_bytes
                
                size_t base_offset = (p * 24 + b) * WS2812_PERIOD * bus_width_bytes;
                
                for (size_t t = 0; t < WS2812_PERIOD; t++) {
                    uint16_t val = 0;
                    
                    for (size_t s = 0; s < num_strips; s++) {
                        mp_led_strip_obj_t *strip = MP_OBJ_TO_PTR(strip_objs[s]);
                        if (p >= strip->length) continue;
                        
                        uint8_t *pixels = (uint8_t *)strip->buf->items;
                        // Get GRB color
                        uint8_t r = pixels[p * 3];
                        uint8_t g = pixels[p * 3 + 1];
                        uint8_t b_val = pixels[p * 3 + 2];
                        
                        uint32_t color = (g << 16) | (r << 8) | b_val;
                        // Bit order: MSB first (bit 23 down to 0)
                        bool bit_set = (color >> (23 - b)) & 1;
                        
                        // Determine if High or Low at this tick t
                        bool is_high = false;
                        if (bit_set) {
                            if (t < WS2812_T1H) is_high = true;
                        } else {
                            if (t < WS2812_T0H) is_high = true;
                        }
                        
                        if (is_high) {
                            val |= (1 << strip->pin_index);
                        }
                    }
                    
                    if (bus_width_bytes == 1) {
                        dma_buf[base_offset + t] = (uint8_t)val;
                    } else {
                        ((uint16_t *)dma_buf)[(base_offset / 2) + t] = val;
                    }
                }
            }
        }
        
        // 4. Send
        self->trans_done = false;
        esp_err_t ret = esp_lcd_panel_io_tx_color(self->io_handle, -1, dma_buf, needed_size);
        if (ret != ESP_OK) {
             mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("tx_color failed: %d"), ret);
        }
        
        // Wait for completion? User might want async.
        // But for now let's block or use callback.
        // The Python API in README implies synchronous or async with callback.
        // "bus.show() triggers... DMA transfer".
        
        return mp_const_none;
    }
    static MP_DEFINE_CONST_FUN_OBJ_1(mp_led_i80_bus_show_obj, mp_led_i80_bus_show);

    // Constructor
    static mp_obj_t mp_led_i80_bus_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args)
    {
        enum { ARG_data_pins, ARG_clk, ARG_dc, ARG_freq, ARG_dma_size };
        static const mp_arg_t allowed_args[] = {
            { MP_QSTR_data_pins, MP_ARG_OBJ | MP_ARG_REQUIRED },
            { MP_QSTR_clk,       MP_ARG_INT | MP_ARG_REQUIRED },
            { MP_QSTR_dc,        MP_ARG_INT | MP_ARG_REQUIRED },
            { MP_QSTR_freq,      MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 10000000 } },
            { MP_QSTR_dma_size,  MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 64000 } },
        };
        mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
        mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

        // Check if we need to clean up old buses (Soft Reboot handling)
        // If we are creating a new bus, and there are existing ones, it's safer to clear them
        // to avoid pin conflicts or resource exhaustion, especially during development.
        if (i80_bus_count > 0) {
            mp_led_i80_bus_deinit_all();
        }

        mp_led_i80_bus_obj_t *self = m_new_obj(mp_led_i80_bus_obj_t);
        self->base.type = &mp_led_i80_bus_type;
        self->strips = MP_OBJ_TO_PTR(mp_obj_new_list(0, NULL));
        self->callback = mp_const_none;
        self->buffer_size = 0;
        self->view1 = NULL;

        // Extract pins
        size_t num_pins;
        mp_obj_t *pins;
        mp_obj_get_array(args[ARG_data_pins].u_obj, &num_pins, &pins);
        
        if (num_pins > 16) {
            mp_raise_ValueError(MP_ERROR_TEXT("Max 16 data pins"));
        }

        // Configure Bus
        memset(&self->bus_config, 0, sizeof(self->bus_config));
        self->bus_config.dc_gpio_num = args[ARG_dc].u_int;
        self->bus_config.wr_gpio_num = args[ARG_clk].u_int;
        self->bus_config.clk_src = LCD_CLK_SRC_PLL160M; // or DEFAULT
        self->bus_config.bus_width = num_pins;
        self->bus_config.max_transfer_bytes = args[ARG_dma_size].u_int;
        
        for (int i = 0; i < num_pins; i++) {
            self->bus_config.data_gpio_nums[i] = mp_obj_get_int(pins[i]);
        }
        for (int i = num_pins; i < 16; i++) {
             self->bus_config.data_gpio_nums[i] = -1;
        }

        esp_err_t ret = esp_lcd_new_i80_bus(&self->bus_config, &self->bus_handle);
        if (ret != ESP_OK) {
            mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("esp_lcd_new_i80_bus failed: %d"), ret);
        }

        // Configure Panel IO
        memset(&self->panel_io_config, 0, sizeof(self->panel_io_config));
        self->panel_io_config.cs_gpio_num = -1;
        self->panel_io_config.pclk_hz = args[ARG_freq].u_int;
        self->panel_io_config.trans_queue_depth = 4;
        self->panel_io_config.on_color_trans_done = i80_led_bus_trans_done_cb;
        self->panel_io_config.user_ctx = self;
        self->panel_io_config.lcd_cmd_bits = 0;
        self->panel_io_config.lcd_param_bits = 0;
        
        // We need to set dc_levels etc?
        // For raw transfer, we don't care about DC.
        // But esp_lcd might toggle it. Set to -1.
        
        ret = esp_lcd_new_panel_io_i80(self->bus_handle, &self->panel_io_config, &self->io_handle);
        if (ret != ESP_OK) {
            esp_lcd_del_i80_bus(self->bus_handle);
             mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("esp_lcd_new_panel_io_i80 failed: %d"), ret);
        }

        // Add to global list
        i80_bus_count++;
        i80_bus_objs = m_realloc(i80_bus_objs, i80_bus_count * sizeof(mp_led_i80_bus_obj_t *));
        i80_bus_objs[i80_bus_count - 1] = self;

        return MP_OBJ_FROM_PTR(self);
    }
    
    // Deinit
    static mp_obj_t mp_led_i80_bus_deinit(mp_obj_t self_in) {
        mp_led_i80_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
        if (self->io_handle) {
            esp_lcd_panel_io_del(self->io_handle);
            self->io_handle = NULL;
        }
        if (self->bus_handle) {
            esp_lcd_del_i80_bus(self->bus_handle);
            self->bus_handle = NULL;
        }
        
        // Remove from global list
        if (i80_bus_objs) {
            for (int i = 0; i < i80_bus_count; i++) {
                if (i80_bus_objs[i] == self) {
                    // Shift remaining
                    for (int j = i; j < i80_bus_count - 1; j++) {
                        i80_bus_objs[j] = i80_bus_objs[j+1];
                    }
                    i80_bus_count--;
                    if (i80_bus_count == 0) {
                        m_free(i80_bus_objs);
                        i80_bus_objs = NULL;
                    } else {
                        i80_bus_objs = m_realloc(i80_bus_objs, i80_bus_count * sizeof(mp_led_i80_bus_obj_t *));
                    }
                    break;
                }
            }
        }
        
        return mp_const_none;
    }

    static void mp_led_i80_bus_deinit_all(void) {
        while (i80_bus_count > 0) {
            mp_led_i80_bus_deinit(MP_OBJ_FROM_PTR(i80_bus_objs[0]));
        }
    }
    
    static MP_DEFINE_CONST_FUN_OBJ_1(mp_led_i80_bus_deinit_obj, mp_led_i80_bus_deinit);
    
    // Locals
    static const mp_rom_map_elem_t mp_led_i80_bus_locals_dict_table[] = {
        { MP_ROM_QSTR(MP_QSTR_add_strip), MP_ROM_PTR(&mp_led_i80_bus_add_strip_obj) },
        { MP_ROM_QSTR(MP_QSTR_show),      MP_ROM_PTR(&mp_led_i80_bus_show_obj) },
        { MP_ROM_QSTR(MP_QSTR_deinit),    MP_ROM_PTR(&mp_led_i80_bus_deinit_obj) },
    };
    static MP_DEFINE_CONST_DICT(mp_led_i80_bus_locals_dict, mp_led_i80_bus_locals_dict_table);

    MP_DEFINE_CONST_OBJ_TYPE(
        mp_led_i80_bus_type,
        MP_QSTR_I8080_Bus,
        MP_TYPE_FLAG_NONE,
        make_new, mp_led_i80_bus_make_new,
        locals_dict, &mp_led_i80_bus_locals_dict
    );

#endif
