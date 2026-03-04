// Copyright (c) 2024 - 2025 Kevin G. Schlosser

// local includes
#include "modmp_led.h"
#include "esp32_include/i80_led_bus.h"

// micropython includes
#include "py/obj.h"
#include "py/runtime.h"

// esp-idf includes
#ifdef ESP_IDF_VERSION
    #include "esp_heap_caps.h"
#endif

// Define module globals
static const mp_rom_map_elem_t mp_module_mp_led_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),           MP_OBJ_NEW_QSTR(MP_QSTR_mp_led)        },
    
    #if SOC_LCD_I80_SUPPORTED
        { MP_ROM_QSTR(MP_QSTR_I8080_Bus),      MP_ROM_PTR(&mp_led_i80_bus_type)       },
    #endif

    // Memory constants
    #ifdef ESP_IDF_VERSION
        { MP_ROM_QSTR(MP_QSTR_MEMORY_32BIT),    MP_ROM_INT(MALLOC_CAP_32BIT)     },
        { MP_ROM_QSTR(MP_QSTR_MEMORY_8BIT),     MP_ROM_INT(MALLOC_CAP_8BIT)      },
        { MP_ROM_QSTR(MP_QSTR_MEMORY_DMA),      MP_ROM_INT(MALLOC_CAP_DMA)       },
        { MP_ROM_QSTR(MP_QSTR_MEMORY_SPIRAM),   MP_ROM_INT(MALLOC_CAP_SPIRAM)    },
        { MP_ROM_QSTR(MP_QSTR_MEMORY_INTERNAL), MP_ROM_INT(MALLOC_CAP_INTERNAL)  },
        { MP_ROM_QSTR(MP_QSTR_MEMORY_DEFAULT),  MP_ROM_INT(MALLOC_CAP_DEFAULT)   },
    #endif
};

static MP_DEFINE_CONST_DICT(mp_module_mp_led_globals, mp_module_mp_led_globals_table);

const mp_obj_module_t mp_module_mp_led = {
    .base    = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&mp_module_mp_led_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mp_led, mp_module_mp_led);
