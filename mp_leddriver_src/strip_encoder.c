#include "py/runtime.h"
#include "mp_leddriver.h"
#include <string.h>

// Forward declaration of strip type
const mp_obj_type_t mp_type_Strip;

// Constructor (Called by Bus.add_strip)
mp_obj_t strip_make_new(mp_obj_i8080_bus_t *bus, int pin_index, int length, int type) {
    mp_obj_strip_t *self = m_new_obj(mp_obj_strip_t);
    self->base.type = &mp_type_Strip;
    self->bus = bus;
    self->pin_index = pin_index;
    self->length = length;
    self->bpp = 3; // Default to RGB, TODO: Support RGBW
    
    // Allocate pixel buffer
    self->pixel_data = m_new(uint8_t, length * self->bpp);
    memset(self->pixel_data, 0, length * self->bpp);
    
    return MP_OBJ_FROM_PTR(self);
}

// __setitem__ implementation
STATIC mp_obj_t strip_subscr(mp_obj_t self_in, mp_obj_t index, mp_obj_t value) {
    mp_obj_strip_t *self = MP_OBJ_TO_PTR(self_in);
    
    if (value == MP_OBJ_SENTINEL) { // Load
        // Return tuple (r, g, b)
        int idx = mp_obj_get_int(index);
        if (idx < 0 || idx >= self->length) {
            mp_raise_ValueError("Index out of range");
        }
        mp_obj_t tuple[3];
        tuple[0] = mp_obj_new_int(self->pixel_data[idx*3 + 0]);
        tuple[1] = mp_obj_new_int(self->pixel_data[idx*3 + 1]);
        tuple[2] = mp_obj_new_int(self->pixel_data[idx*3 + 2]);
        return mp_obj_new_tuple(3, tuple);
    } else { // Store
        int idx = mp_obj_get_int(index);
        if (idx < 0 || idx >= self->length) {
            mp_raise_ValueError("Index out of range");
        }
        
        mp_obj_t *items;
        size_t len;
        mp_obj_get_array(value, &len, &items);
        if (len != 3) {
            mp_raise_ValueError("Color must be (r, g, b)");
        }
        
        self->pixel_data[idx*3 + 0] = mp_obj_get_int(items[0]);
        self->pixel_data[idx*3 + 1] = mp_obj_get_int(items[1]);
        self->pixel_data[idx*3 + 2] = mp_obj_get_int(items[2]);
        
        return mp_const_none;
    }
}

// Encode function for WS2812
// Assuming 10MHz clock -> 100ns per sample
// Bit 0: 400ns High (4 samples), 850ns Low (9 samples) -> Total 13 samples
// Bit 1: 800ns High (8 samples), 450ns Low (5 samples) -> Total 13 samples
void encode_strip_to_buffer(mp_obj_strip_t *strip, uint8_t *buffer) {
    // Check if buffer is valid
    if (buffer == NULL) return;
    
    // Iterate pixels
    for (int i = 0; i < strip->length; i++) {
        uint8_t r = strip->pixel_data[i*3 + 0];
        uint8_t g = strip->pixel_data[i*3 + 1];
        uint8_t b = strip->pixel_data[i*3 + 2];
        
        // GRB Order for WS2812
        uint32_t color = (g << 16) | (r << 8) | b;
        
        // For each bit (23 down to 0)
        for (int bit = 23; bit >= 0; bit--) {
            int is_high = (color >> bit) & 1;
            
            // Calculate base index in the DMA buffer
            // Each bit takes 13 bytes
            int bit_offset = (23 - bit);
            int pixel_offset = i * 24 * 13;
            int base_idx = pixel_offset + (bit_offset * 13);
            
            // Logic for WS2812 Timing at 10MHz
            int high_count = is_high ? 8 : 4;
            
            for (int s = 0; s < 13; s++) {
                if (s < high_count) {
                    // Set the bit corresponding to this strip's pin
                    buffer[base_idx + s] |= (1 << strip->pin_index);
                }
                // No else needed because we assume buffer is zeroed before encoding
            }
        }
    }
}

// Type Definition
const mp_obj_type_t mp_type_Strip = {
    { &mp_type_type },
    .name = MP_QSTR_Strip,
    .subscr = strip_subscr,
};
