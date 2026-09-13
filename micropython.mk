# micropython.mk — mp_LEDdriver build rules (legacy make build system)
#
# 對應 micropython.cmake 的 SOURCES 清單；兩者必須保持同步。

MOD_DIR := $(USERMOD_DIR)

CFLAGS_USERMOD += -I$(MOD_DIR)
CFLAGS_USERMOD += -DLED_BUS_VERSION=\"0.1.0-C0\"

SRC_USERMOD_C += $(MOD_DIR)/led_alloc.c
SRC_USERMOD_C += $(MOD_DIR)/modled_bus.c
