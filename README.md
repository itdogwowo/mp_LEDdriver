# mp_led - ESP32-S3/P4 High Performance LED Driver

This project uses ESP32-S3/P4's **LCD_CAM I8080 Parallel Bus** with **DMA (Direct Memory Access)** to drive multiple LED strips (WS2812B, etc.) in parallel.

## Key Features
- **True Parallelism**: Drive up to 8 or 16 strips simultaneously with exact synchronization.
- **Zero CPU Usage**: Data transfer is handled by DMA; CPU is only used to encode the bits into the DMA buffer.
- **MicroPython Integration**: Easy-to-use Python API for creating strips and setting colors.
- **Customizable Timing**: Optimized for WS2812B at 10MHz (0.1us resolution).

## Hardware Support
- **ESP32-S3**
- **ESP32-P4**

## Installation
1. Copy `mp_led_src` into your MicroPython `user_modules` directory.
2. Compile MicroPython with the module:
   ```bash
   make USER_C_MODULES=../../user_modules/mp_led_src/micropython.mk BOARD=ESP32_GENERIC_S3
   ```

## Python API Usage
```python
import mp_led
import machine

# 1. Initialize Parallel Bus
bus = mp_led.I8080_Bus(
    data_pins=[1, 2, 3, 4, 5, 6, 7, 8], # D0 ~ D7
    clk=9,                              # WR (Write Clock)
    freq=10000000,                      # 10MHz (WS2812 recommended)
    dma_size=64000                      # DMA buffer size
)

# 2. Add Strips (Pin index maps to data_pins list)
strip1 = bus.add_strip(pin_index=0, length=100) # Pin 1
strip2 = bus.add_strip(pin_index=1, length=50)  # Pin 2

# 3. Control Colors
strip1[0] = (255, 0, 0) # Red
strip2[0] = (0, 255, 0) # Green

# 4. Show
bus.show()
```

## How it Works
- The driver creates a large DMA buffer.
- When `bus.show()` is called, it iterates through all bits of all pixels on all strips.
- It "encodes" these bits into the DMA buffer. Each bit of the 8-bit or 16-bit parallel bus represents one strip.
- The I8080 peripheral then blasts the entire DMA buffer out through the pins, generating the WS2812B waveforms in parallel.
