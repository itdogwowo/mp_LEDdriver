
import mp_led
import machine
import time

def test_i8080():
    print("正在測試 I8080_Bus (並行總線)...")
    if not hasattr(mp_led, 'I8080_Bus'):
        print("I8080_Bus 未啟用")
        return

    # 模擬引腳 (請根據實際硬件修改)
    data_pins = [1, 2, 3, 4, 5, 6, 7, 8]
    clk_pin = 9
    dc_pin = 10 # 虛擬 DC 引腳
    
    try:
        print("初始化 I8080_Bus...")
        bus = mp_led.I8080_Bus(
            data_pins=data_pins,
            clk=clk_pin,
            dc=dc_pin,
            freq=10000000,
            dma_size=64000
        )
        print("I8080_Bus 初始化成功")
        
        print("添加燈帶...")
        strip = bus.add_strip(pin_index=0, length=10, type=0)
        print("燈帶添加成功")
        
        # 測試寫入
        strip[0] = (255, 0, 0)
        print("設置像素成功")
        
        # 測試讀取
        val = strip[0]
        print(f"讀取像素: {val}")
        
        bus.show()
        print("Show() 調用成功")
        
        bus.deinit()
        print("I8080_Bus 反初始化成功")
        
    except Exception as e:
        print(f"I8080_Bus 測試失敗: {e}")

def test_led_bus():
    print("\n正在測試 LEDBus (RMT)...")
    if not hasattr(mp_led, 'LEDBus'):
        print("LEDBus 未啟用 (需要在編譯中開啟)")
        return

    # RMT 引腳
    pin = 10
    
    try:
        print("初始化 LEDBus...")
        bus = mp_led.LEDBus(
            data_pin=pin,
            freq=10000000,
            leds_per_pixel=3,
            byte_order=mp_led.GRB
        )
        print("LEDBus 初始化成功")
        
        # 測試內存分配
        # 10 像素, 每個 3 字節 = 30 字節
        buf = bus.allocate_framebuffer(30, mp_led.MEMORY_DEFAULT) 
        print("Framebuffer 分配成功")
        
        # 寫入顏色
        # tx_color(cmd, color, size, x_start, y_start, x_end, y_end, rotation, last_update)
        bus.tx_color(0, buf, 30, 0, 0, 10, 1, 0, True)
        print("tx_color 調用成功")
        
        bus.deinit()
        print("LEDBus 反初始化成功")

    except Exception as e:
        print(f"LEDBus 測試失敗: {e}")

def test_dsi_bus():
    print("\n正在測試 DSIBus (MIPI)...")
    if not hasattr(mp_led, 'DSIBus'):
        print("DSIBus 未啟用 (需要在編譯中開啟)")
        return
        
    try:
        # DSI 參數範例
        print("初始化 DSIBus...")
        bus = mp_led.DSIBus(
            bus_id=0,
            data_lanes=2,
            freq=10000000, # 10MHz
            virtual_channel=0
        )
        print("DSIBus 初始化成功")
        
        bus.deinit()
        print("DSIBus 反初始化成功")
    except Exception as e:
        print(f"DSIBus 測試失敗: {e}")

if __name__ == "__main__":
    test_i8080()
    test_led_bus()
    test_dsi_bus()
