#include "py/obj.h"
#include "py/runtime.h"
#include "mp_leddriver.h"

// Forward declaration of I8080_Bus type
extern const mp_obj_type_t mp_type_I8080_Bus;

// Global function to free all resources (if needed)
STATIC mp_obj_t mp_leddriver_deinit_all(void) {
    // TODO: Iterate over all created buses and deinit them
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mp_leddriver_deinit_all_obj, mp_leddriver_deinit_all);

STATIC const mp_rom_map_elem_t mp_leddriver_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mp_leddriver) },
    { MP_ROM_QSTR(MP_QSTR_I8080_Bus), MP_ROM_PTR(&mp_type_I8080_Bus) },
    { MP_ROM_QSTR(MP_QSTR_deinit_all), MP_ROM_PTR(&mp_leddriver_deinit_all_obj) },
};
STATIC MP_DEFINE_CONST_DICT(mp_leddriver_module_globals, mp_leddriver_module_globals_table);

const mp_obj_module_t mp_module_mp_leddriver = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_leddriver_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mp_leddriver, mp_module_mp_leddriver);
