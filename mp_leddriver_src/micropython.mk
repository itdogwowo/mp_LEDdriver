MP_LEDDRIVER_MOD_DIR := $(USERMOD_DIR)

# Add all C files to SRC_USERMOD.
SRC_USERMOD += $(MP_LEDDRIVER_MOD_DIR)/mp_leddriver.c
SRC_USERMOD += $(MP_LEDDRIVER_MOD_DIR)/bus_i8080.c
# SRC_USERMOD += $(MP_LEDDRIVER_MOD_DIR)/bus_rgb.c
# SRC_USERMOD += $(MP_LEDDRIVER_MOD_DIR)/strip_encoder.c

# We can add our module folder to include paths if needed
CFLAGS_USERMOD += -I$(MP_LEDDRIVER_MOD_DIR)
