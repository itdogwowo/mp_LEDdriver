#ifndef MP_LEDDRIVER_H
#define MP_LEDDRIVER_H

#include "py/obj.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

// Forward declaration
typedef struct _mp_obj_strip_t mp_obj_strip_t;

// I8080 Bus Structure
typedef struct _mp_obj_i8080_bus_t {
    mp_obj_base_t base;
    esp_lcd_i80_bus_handle_t i80_bus;
    esp_lcd_panel_io_handle_t io_handle;
    
    // Pin configuration
    int *data_pins;
    int data_width;
    int clk_pin;
    int freq;
    
    // DMA Buffer
    void *dma_buffer;
    size_t dma_buffer_size;
    
    // Strip management (List of attached strip objects)
    mp_obj_t strip_list; // Python list
} mp_obj_i8080_bus_t;

// Strip Object Structure (Exposed for encoder)
struct _mp_obj_strip_t {
    mp_obj_base_t base;
    mp_obj_i8080_bus_t *bus;
    int pin_index;
    int length;
    int bpp;
    mp_obj_t pixel_buf; // Python bytearray object
};

// Function declarations
extern const mp_obj_type_t mp_type_I8080_Bus;
extern const mp_obj_type_t mp_type_Strip;

mp_obj_t strip_make_new(mp_obj_i8080_bus_t *bus, int pin_index, int length, int type);
void encode_strip_to_buffer(mp_obj_strip_t *strip, uint8_t *buffer, size_t buffer_size);

#endif // MP_LEDDRIVER_H
