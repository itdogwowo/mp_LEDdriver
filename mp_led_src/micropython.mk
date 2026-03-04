# Copyright (c) 2024 - 2025 Kevin G. Schlosser

MOD_DIR := $(USERMOD_DIR)

CFLAGS_USERMOD += -I$(MOD_DIR)
CFLAGS_USERMOD += -I$(MOD_DIR)/esp32_include

ifneq (,$(findstring -Wno-missing-field-initializers, $(CFLAGS_USERMOD)))
    CFLAGS_USERMOD += -Wno-missing-field-initializers
endif

SRC_USERMOD_C += $(MOD_DIR)/modmp_led.c
SRC_USERMOD_C += $(MOD_DIR)/lcd_types.c
SRC_USERMOD_C += $(MOD_DIR)/esp32_src/i80_led_bus.c
SRC_USERMOD_C += $(MOD_DIR)/esp32_src/led_bus.c
SRC_USERMOD_C += $(MOD_DIR)/esp32_src/dsi_bus.c
SRC_USERMOD_C += $(MOD_DIR)/esp32_src/rgb_bus.c
