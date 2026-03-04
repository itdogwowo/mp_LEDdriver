#include "py/runtime.h"
#include "py/mphal.h"
#include "mp_leddriver.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_memory_utils.h"
#include <string.h> // For memset

static const char *TAG = "I8080_BUS";

// Constructor
static mp_obj_t i8080_bus_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_data_pins, ARG_clk, ARG_freq, ARG_dma_size };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data_pins, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_clk, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_freq, MP_ARG_INT, {.u_int = 10000000} }, // Default 10MHz
        { MP_QSTR_dma_size, MP_ARG_INT, {.u_int = 64000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_i8080_bus_t *self = m_new_obj(mp_obj_i8080_bus_t);
    self->base.type = &mp_type_I8080_Bus;
    
    // Parse pins
    mp_obj_t *pins;
    mp_obj_get_array(args[ARG_data_pins].u_obj, (size_t *)&self->data_width, &pins);
    
    // Configure Bus
    esp_lcd_i80_bus_config_t bus_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = -1,
        .wr_gpio_num = args[ARG_clk].u_int,
        .data_width = self->data_width,
        .max_transfer_bytes = args[ARG_dma_size].u_int,
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    
    // Assign pins to config
    for (int i = 0; i < self->data_width; i++) {
        bus_config.data_gpio_nums[i] = mp_obj_get_int(pins[i]);
    }

    // Initialize Bus
    esp_err_t ret = esp_lcd_new_i80_bus(&bus_config, &self->i80_bus);
    if (ret != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("Failed to init I8080 bus: %d"), ret);
    }

    // Configure Panel IO
    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = -1,
        .pclk_hz = args[ARG_freq].u_int,
        .trans_queue_depth = 10,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 0,
        },
    };
    
    ret = esp_lcd_new_panel_io_i80(self->i80_bus, &io_config, &self->io_handle);
    if (ret != ESP_OK) {
        esp_lcd_del_i80_bus(self->i80_bus);
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("Failed to init Panel IO: %d"), ret);
    }
    
    // Allocate DMA Buffer
    // We assume max possible usage for now, or dynamic resize later.
    // For WS2812: 1 pixel = 24 bits * 13 samples = 312 bytes.
    // Let's allocate based on max_transfer_sz for now.
    self->dma_buffer_size = args[ARG_dma_size].u_int;
    self->dma_buffer = heap_caps_malloc(self->dma_buffer_size, MALLOC_CAP_DMA);
    if (!self->dma_buffer) {
        // Cleanup
        esp_lcd_panel_io_del(self->io_handle);
        esp_lcd_del_i80_bus(self->i80_bus);
        mp_raise_msg_varg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate DMA buffer"));
    }
    memset(self->dma_buffer, 0, self->dma_buffer_size);
    
    // Initialize Strip List
    self->strip_list = mp_obj_new_list(0, NULL);
    
    return MP_OBJ_FROM_PTR(self);
}

// Add Strip
static mp_obj_t i8080_bus_add_strip(size_t n_args, const mp_obj_t *args) {
    mp_obj_i8080_bus_t *self = MP_OBJ_TO_PTR(args[0]);
    int pin_index = mp_obj_get_int(args[1]);
    int length = mp_obj_get_int(args[2]);
    int type = mp_obj_get_int(args[3]);
    
    // Check pin index
    if (pin_index < 0 || pin_index >= self->data_width) {
        mp_raise_ValueError("Invalid pin index");
    }
    
    // Create Strip object
    mp_obj_t strip = strip_make_new(self, pin_index, length, type);
    
    // Add to list
    mp_obj_list_append(self->strip_list, strip);
    
    return strip;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i8080_bus_add_strip_obj, 4, 4, i8080_bus_add_strip);

// Show (Render and DMA Transfer)
static mp_obj_t i8080_bus_show(mp_obj_t self_in) {
    mp_obj_i8080_bus_t *self = MP_OBJ_TO_PTR(self_in);
    
    // 1. Clear DMA Buffer (memset 0)
    memset(self->dma_buffer, 0, self->dma_buffer_size);
    
    // 2. Iterate strips and encode data
    size_t num_strips;
    mp_obj_t *strips;
    mp_obj_list_get(self->strip_list, &num_strips, &strips);
    
    for (size_t i = 0; i < num_strips; i++) {
        mp_obj_strip_t *strip = MP_OBJ_TO_PTR(strips[i]);
        // TODO: Check buffer bounds
        encode_strip_to_buffer(strip, (uint8_t *)self->dma_buffer, self->dma_buffer_size);
    }
    
    // 3. Trigger DMA
    // We need to calculate the actual transmission size.
    // For now, we use the max size or a fixed size based on the longest strip.
    // TODO: Dynamic size calculation.
    
    esp_lcd_panel_io_tx_color(self->io_handle, -1, self->dma_buffer, self->dma_buffer_size);
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(i8080_bus_show_obj, i8080_bus_show);

// Write (Direct buffer write, for debugging or custom usage)
static mp_obj_t i8080_bus_write(size_t n_args, const mp_obj_t *args) {
    mp_obj_i8080_bus_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[1], &bufinfo, MP_BUFFER_READ);
    
    void *ptr = bufinfo.buf;
    size_t len = bufinfo.len;
    
    // Check for DMA capability and alignment (4-byte aligned)
    // S3's LCD peripheral typically requires alignment.
    if (esp_ptr_dma_capable(ptr) && ((uintptr_t)ptr & 3) == 0) {
        // Zero-copy path: The user buffer is directly usable by DMA
        esp_lcd_panel_io_tx_color(self->io_handle, -1, ptr, len);
    } else {
        // Copy path: Copy to our internal DMA-capable buffer
        if (len > self->dma_buffer_size) {
            len = self->dma_buffer_size;
        }
        memcpy(self->dma_buffer, ptr, len);
        esp_lcd_panel_io_tx_color(self->io_handle, -1, self->dma_buffer, len);
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i8080_bus_write_obj, 2, 2, i8080_bus_write);

// Deinit
static mp_obj_t i8080_bus_deinit(mp_obj_t self_in) {
    mp_obj_i8080_bus_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->io_handle) {
        esp_lcd_panel_io_del(self->io_handle);
        self->io_handle = NULL;
    }
    if (self->i80_bus) {
        esp_lcd_del_i80_bus(self->i80_bus);
        self->i80_bus = NULL;
    }
    if (self->dma_buffer) {
        heap_caps_free(self->dma_buffer);
        self->dma_buffer = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(i8080_bus_deinit_obj, i8080_bus_deinit);

// Local Dictionary
static const mp_rom_map_elem_t i8080_bus_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&i8080_bus_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_add_strip), MP_ROM_PTR(&i8080_bus_add_strip_obj) },
    { MP_ROM_QSTR(MP_QSTR_show), MP_ROM_PTR(&i8080_bus_show_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&i8080_bus_write_obj) },
};
static MP_DEFINE_CONST_DICT(i8080_bus_locals_dict, i8080_bus_locals_dict_table);

// Type Definition
const mp_obj_type_t mp_type_I8080_Bus = {
    { &mp_type_type },
    .name = MP_QSTR_I8080_Bus,
    .make_new = i8080_bus_make_new,
    .locals_dict = (mp_obj_dict_t *)&i8080_bus_locals_dict,
};
