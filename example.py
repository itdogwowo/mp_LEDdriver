# Example usage of mp_led module for ESP32-S3/P4

import mp_led
import machine
import time

# 1. Initialize Parallel Bus (I8080)
# We use 8 pins for 8 parallel strips.
# WR pin (clk) is GPIO 9 (adjust as needed).
# Frequency 10MHz is good for WS2812B (100ns resolution).
# WS2812B needs ~1.25us period. 10MHz = 0.1us. 13 ticks = 1.3us.
# T0H=4 (0.4us), T0L=9 (0.9us)
# T1H=8 (0.8us), T1L=5 (0.5us)

data_pins = [1, 2, 3, 4, 5, 6, 7, 8] # Adjust pins
clk_pin = 9 

bus = mp_led.I8080_Bus(
    data_pins=data_pins,
    clk=clk_pin,
    freq=10000000, 
    dma_size=64000
)

print("Bus initialized")

# 2. Add Strips
# Strip 0 on Pin index 0 (GPIO 1)
strip1 = bus.add_strip(pin_index=0, length=100, type=0)

# Strip 1 on Pin index 1 (GPIO 2)
strip2 = bus.add_strip(pin_index=1, length=50, type=0)

print("Strips added")

# 3. Set Colors
# Red on strip 1
strip1[0] = (255, 0, 0)
# Green on strip 2
strip2[0] = (0, 255, 0)

# 4. Show
bus.show()
print("Show called")

# Animation
while True:
    for i in range(100):
        strip1[i] = (0, 0, 255)
        if i > 0:
            strip1[i-1] = (0, 0, 0)
        bus.show()
        time.sleep(0.01)
