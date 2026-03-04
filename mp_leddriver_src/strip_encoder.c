#include "py/runtime.h"
#include "mp_leddriver.h"
#include <string.h>

// Forward declaration of strip type
const mp_obj_type_t mp_type_Strip;

// Locals Dict (Empty for now, but good to have)
static const mp_rom_map_elem_t strip_locals_dict_table[] = {
    // Add methods here if needed
};
static MP_DEFINE_CONST_DICT(strip_locals_dict, strip_locals_dict_table);

// Constructor (Called by Bus.add_strip)
mp_obj_t strip_make_new(mp_obj_i8080_bus_t *bus, int pin_index, int length, int type) {
    mp_obj_strip_t *self = m_new_obj(mp_obj_strip_t);
    self->base.type = &mp_type_Strip;
    self->bus = bus;
    self->pin_index = pin_index;
    self->length = length;
    self->bpp = 3; // Default to RGB, TODO: Support RGBW
    
    // Allocate pixel buffer as bytearray
    // Use mp_obj_new_bytearray(size, items). If items is NULL, it allocates but contents are undefined.
    // So we allocate then memset.
    self->pixel_buf = mp_obj_new_bytearray(length * self->bpp, NULL);
    mp_buffer_info_t bufinfo;
    if (mp_get_buffer(self->pixel_buf, &bufinfo, MP_BUFFER_WRITE)) {
        memset(bufinfo.buf, 0, bufinfo.len);
    }
    
    return MP_OBJ_FROM_PTR(self);
}

// Attribute Access (.buf)
static void strip_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        // Load attribute
        mp_obj_strip_t *self = MP_OBJ_TO_PTR(self_in);
        if (attr == MP_QSTR_buf) {
            dest[0] = self->pixel_buf;
        } else {
            // Check locals_dict for methods
            // Access the dictionary directly since we defined it statically
            mp_map_elem_t *elem = mp_map_lookup((mp_map_t*)&strip_locals_dict.map, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
            if (elem != NULL) {
                mp_convert_member_lookup(self_in, self->base.type, elem->value, dest);
            }
        }
    } else {
        // Store/Delete attribute
        if (attr == MP_QSTR_buf && dest[1] != MP_OBJ_NULL) {
             mp_obj_strip_t *self = MP_OBJ_TO_PTR(self_in);
             // Verify type and size
             mp_buffer_info_t bufinfo;
             mp_get_buffer_raise(dest[1], &bufinfo, MP_BUFFER_READ);
             if (bufinfo.len != self->length * self->bpp) {
                 mp_raise_ValueError("Buffer size mismatch");
             }
             self->pixel_buf = dest[1];
             dest[0] = MP_OBJ_NULL; // Success
        }
    }
}

// __setitem__ implementation
static mp_obj_t strip_subscr(mp_obj_t self_in, mp_obj_t index, mp_obj_t value) {
    mp_obj_strip_t *self = MP_OBJ_TO_PTR(self_in);
    
    // Get raw pointer from bytearray
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->pixel_buf, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *pixel_data = bufinfo.buf;
    
    if (value == MP_OBJ_SENTINEL) { // Load
        // Return tuple (r, g, b)
        int idx = mp_obj_get_int(index);
        if (idx < 0 || idx >= self->length) {
            mp_raise_ValueError("Index out of range");
        }
        mp_obj_t tuple[3];
        tuple[0] = mp_obj_new_int(pixel_data[idx*3 + 0]);
        tuple[1] = mp_obj_new_int(pixel_data[idx*3 + 1]);
        tuple[2] = mp_obj_new_int(pixel_data[idx*3 + 2]);
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
        
        pixel_data[idx*3 + 0] = mp_obj_get_int(items[0]);
        pixel_data[idx*3 + 1] = mp_obj_get_int(items[1]);
        pixel_data[idx*3 + 2] = mp_obj_get_int(items[2]);
        
        return mp_const_none;
    }
}

// Encode function for WS2812
// Assuming 10MHz clock -> 100ns per sample
// Bit 0: 400ns High (4 samples), 850ns Low (9 samples) -> Total 13 samples
// Bit 1: 800ns High (8 samples), 450ns Low (5 samples) -> Total 13 samples
void encode_strip_to_buffer(mp_obj_strip_t *strip, uint8_t *buffer, size_t buffer_size) {
    // Check if buffer is valid
    if (buffer == NULL) return;
    
    // Get raw pointer from bytearray
    mp_buffer_info_t bufinfo;
    if (!mp_get_buffer(strip->pixel_buf, &bufinfo, MP_BUFFER_READ)) return;
    uint8_t *pixel_data = bufinfo.buf;
    
    // Iterate pixels
    for (int i = 0; i < strip->length; i++) {
        // Safety check: ensure we don't write past buffer end
        // Each pixel takes 312 bytes (24 * 13)
        if ((i + 1) * 312 > buffer_size) {
            break; // Stop encoding to prevent overflow
        }

        uint8_t r = pixel_data[i*3 + 0];
        uint8_t g = pixel_data[i*3 + 1];
        uint8_t b = pixel_data[i*3 + 2];
        
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
// Type Definition using modern macro
MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_Strip,
    MP_QSTR_Strip,
    MP_TYPE_FLAG_NONE,
    attr, strip_attr,
    subscr, strip_subscr,
    locals_dict, &strip_locals_dict
);
