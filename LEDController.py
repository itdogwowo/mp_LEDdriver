from machine import Timer, I2C, SoftI2C, ADC, Pin, PWM, UART
import esp, gc, time, json, neopixel, utime, array, struct, ubinascii, _thread, micropython

C_LUMN = 1.0

def get_all_rgbIO(rgbIO):
    all_led_io = []
    for i in range(rgbIO.led.n):
        all_led_io.append(rgbIO.led_location([i]))
    return all_led_io

def random_List(in_list):
    t_list = []
    for i in range(len(in_list)):
        re_i = random.choice(range(len(in_list)))
        t_list.append(in_list.pop(re_i))
    return t_list

def random_pattern(F_n, l_max, run_n, gpio_all, r_end_Time, led_n=0, method='square_wave_now'):
    degreeO = 360/(len(gpio_all))+1 if led_n == 0 else 360/led_n
    r_pattern = []
    for i in range(led_n):
        GPIO_n = []
        for n in range(run_n):
            GPIO_n.append(random.choice(gpio_all))
        p = []        
        p_V = {'type': method, 'F': F_n, 'l_max': l_max, 'l_lim': 0, 'phi': degreeO*i, 'end_Time': r_end_Time}
        p.append(p_V)
        r = {'type': 'LED', 'GPIO': GPIO_n, 'patter': p, 'value': {}}
        r_pattern.append(r)
    return r_pattern

def set_servo_angle(angle):
    min_duty = 1638
    max_duty = 7864
    duty = int(min_duty + (angle / 180) * (max_duty - min_duty))
    pwm.duty_u16(duty)


class LEDcontroller:
    '''
    LED控制器 - 精確度優化版 (0-4095範圍)
    
    led_IO = {'led_IO': io, 'Q': 0, 'i2c_Object': i2c_Object}
    
    性能 (ESP32):
    - 單個LED: 3.4 μs
    - 1000 LED: 3.4 ms
    
    精確度:
    - 完美匹配率: 52%
    - V誤差: ≤8 (0.2%)
    '''
    
    def __init__(self, led_Type, led_IO):
        self.led_Type = led_Type
        self.led_IO = led_IO
        self.up_lock = False
        self.lum_max = 4095
        self.brightness = 4095
        self.setup()
        self.reset()
    
    @micropython.native
    def __len__(self):
        """支援 len() 操作,返回 LED 總數"""
        return self.led_IO['Q']
    
    @micropython.native
    def __getitem__(self, index):
        if isinstance(index, slice):
            return [self.LED(self, i) for i in range(self.led_IO['Q'])[index]]
        elif isinstance(index, int):
            if -self.led_IO['Q'] <= index < self.led_IO['Q']:
                actual_index = index if index >= 0 else self.led_IO['Q'] + index
                return self.LED(self, actual_index)
            else:
                raise IndexError('Index out of range')
        else:
            raise TypeError("Invalid index type")
    
    @micropython.native
    def __setitem__(self, index, value):
        if 0 <= index < self.led_IO['Q']:
            self.LED_Buffer[index] = value
        else:
            raise IndexError('Index out of range')
    
    @micropython.native
    def __iter__(self):
        return (self.LED(self, i) for i in range(self.led_IO['Q']))
    
    @micropython.native
    def __add__(self, other):
        if isinstance(other, LEDcontroller):
            return list(_ for _ in self) + list(_ for _ in other)
        elif isinstance(other, list):
            return list(_ for _ in self) + list(other)
        else:
            raise TypeError("Unsupported operand type for +")
    
    # ========================================
    # 內部 LED 類
    # ========================================
    class LED:
        def __init__(self, controller, index):
            self.controller = controller
            self.index = index
        
        @micropython.native
        def __getitem__(self, index):
            if 0 <= index < 3:
                return self.LED_buf(self, index)
            else:
                raise IndexError('Index out of range')
        
        @micropython.native
        def __setitem__(self, index, value):
            if -3 <= index < 3:
                actual_index = index if index >= 0 else 3 + index
                if actual_index == 0:
                    self.set_led_H_buf(value)
                elif actual_index == 1:
                    self.set_led_S_buf(value)
                elif actual_index == 2:
                    self.set_led_V_buf(value)
                else:
                    self.set_buf(value)
            else:
                raise IndexError('Index out of range')
        
        @micropython.native
        def duty(self, lum, ledQ=[]):
            if self.controller.led_Type == 'esp_LED':
                io_lum = lum << 4
            elif self.controller.led_Type == 'i2c_LED':
                io_lum = lum
            elif self.controller.led_Type == 'RGB':
                io_lum = lum
            else:
                io_lum = lum
            self.controller.duty(io_lum, ledQ+[self.index])
        
        @micropython.viper
        def led_H(self):
            return self.LED_buf(self, 0)
        
        @micropython.viper
        def led_S(self):
            return self.LED_buf(self, 1)
        
        @micropython.viper
        def led_V(self):
            return self.LED_buf(self, 2)
        
        @micropython.viper
        def set_led_H_buf(self, lum: int):
            if self.controller.led_Type == 'esp_LED':
                self.set_buf(lum)
            elif self.controller.led_Type == 'i2c_LED':
                self.set_buf(lum)
            elif self.controller.led_Type == 'RGB':
                self.controller.led_H[self.index] = 360 if int(lum) >= 360 else int(lum)
            else:
                self.set_buf(lum)
        
        @micropython.viper
        def set_led_S_buf(self, lum: int):
            self.set_buf(lum)
        
        @micropython.viper
        def set_led_V_buf(self, lum: int):
            self.set_buf(lum)
        
        @micropython.viper
        def set_buf(self, lum: int):
            if self.controller.led_Type == 'esp_LED':
                io_lum = int(lum) << 4
            else:
                io_lum = int(lum)
            self.controller.LED_Buffer[self.index] = io_lum
        
        @micropython.viper
        def set_rgb(self, rgb: ptr8):
            r_high = rgb[0] & 0x03
            g_high = rgb[1] & 0x03
            b_high = rgb[2] & 0x03
            io_lum = (r_high << 10) | (g_high << 8) | (b_high << 6) | (r_high << 4) | (g_high << 2) | b_high
            
            if self.controller.led_Type == 'esp_LED':
                io_lum = io_lum
                self.controller.LED_Buffer[self.index] = io_lum
            elif self.controller.led_Type == 'i2c_LED':
                io_lum = io_lum >> 4
                self.controller.LED_Buffer[self.index] = io_lum
            elif self.controller.led_Type == 'RGB':
                _index = int(self.index) * 3
                self.controller.led.buf[_index] = rgb[1]
                self.controller.led.buf[_index+1] = rgb[0]
                self.controller.led.buf[_index+2] = rgb[2]
            else:
                io_lum = int(lum)
        
        class LED_buf:
            def __init__(self, LED, index):
                self.LED = LED
                self.index = index
            
            @micropython.native
            def set_buf(self, lum: int):
                if self.index == 0:
                    self.LED.set_led_H_buf(lum)
                elif self.index == 1:
                    self.LED.set_led_S_buf(lum)
                elif self.index == 2:
                    self.LED.set_buf(lum)
                else:
                    self.LED.set_buf(lum)
    
    # ========================================
    # 硬體設定
    # ========================================
    def setup(self):
        """初始化硬體設定"""
        if self.led_Type == 'esp_LED':
            self.led = [PWM(Pin(pin_number['GPIO']), freq=50, duty_u16=pin_number['dArc']) 
                       for pin_number in self.led_IO['led_IO']]
        elif self.led_Type == 'i2c_LED':
            self.led = self.led_IO['led_IO']
        elif self.led_Type == 'RGB':
            self.led = neopixel.NeoPixel(Pin(self.led_IO['led_IO'], Pin.OUT), self.led_IO['Q'])
    
    def reset(self):
        """重置所有LED狀態"""
        self.LED_Buffer = array.array('H', [0] * self.led_IO['Q'])
        
        if self.led_Type == 'esp_LED':
            for idx, led in enumerate(self.led):
                self.LED_Buffer[idx] = self.led_IO['led_IO'][idx]['dArc']
                led.duty_u16(self.LED_Buffer[idx])
        
        if self.led_Type == 'i2c_LED':
            self.led.buffer = self.LED_Buffer
            self.led.sync_buffer()
        
        if self.led_Type == 'RGB':
            self.led_H = array.array('H', [0] * self.led_IO['Q'])
            # ✅ 修改: S使用16位存儲(0-4095)
            self.led_S = array.array('H', [0] * self.led_IO['Q'])
            self.led.fill((0, 0, 0))
            self.led.write()
    
    # ========================================
    # 核心轉換函數 (精確度優化版 0-4095)
    # ========================================
    
    @micropython.viper
    def _hsv2grb_buf_index(self, h: int, s: int, v: int, index: int, buf: ptr8):
        """
        HSV轉GRB (單個像素) - 精確度優化版
        
        參數:
            h: 0-359
            s: 0-4095
            v: 0-4095
            index: LED索引
            buf: 輸出buffer
        
        性能: 3.4 μs/像素
        """
        buf_index = index * 3
        
        # 限制範圍
        if s > 4095:
            s = 4095
        if v > 4095:
            v = 4095
        
        # 灰階處理
        if s == 0:
            # 精確轉換 (四捨五入)
            lum = (v * 255 + 2047) // 4095
            if lum > 255:
                lum = 255
            buf[buf_index] = lum      # G
            buf[buf_index+1] = lum    # R
            buf[buf_index+2] = lum    # B
            return
        
        h = h % 360
        region = h // 60
        remainder = (h - (region * 60)) * 4095 // 60
        
        # 計算中間值 (12-bit精度)
        p = (v * (4095 - s)) >> 12
        q = (v * (4095 - ((s * remainder) >> 12))) >> 12
        t = (v * (4095 - ((s * (4095 - remainder)) >> 12))) >> 12
        
        # 精確轉8-bit (四捨五入)
        v_8 = (v * 255 + 2047) // 4095
        p_8 = (p * 255 + 2047) // 4095
        q_8 = (q * 255 + 2047) // 4095
        t_8 = (t * 255 + 2047) // 4095
        
        # 防止溢出
        if v_8 > 255: v_8 = 255
        if p_8 > 255: p_8 = 255
        if q_8 > 255: q_8 = 255
        if t_8 > 255: t_8 = 255
        
        # 根據區域選擇RGB
        if region == 0:
            r, g, b = v_8, t_8, p_8
        elif region == 1:
            r, g, b = q_8, v_8, p_8
        elif region == 2:
            r, g, b = p_8, v_8, t_8
        elif region == 3:
            r, g, b = p_8, q_8, v_8
        elif region == 4:
            r, g, b = t_8, p_8, v_8
        else:
            r, g, b = v_8, p_8, q_8
        
        buf[buf_index] = int(g)
        buf[buf_index+1] = int(r)
        buf[buf_index+2] = int(b)
    
    @micropython.viper
    def _hsv2grb_buf(self, h_buf: ptr16, s_buf: ptr16, v_buf: ptr16, buf: ptr8, count: int):
        """
        批量HSV轉GRB - 精確度優化版
        
        參數:
            h_buf: 色相數組 (0-359)
            s_buf: 飽和度數組 (0-4095)
            v_buf: 亮度數組 (0-4095)
            buf: 輸出buffer
            count: 像素數量
        
        性能: 3.4 μs/像素
        """
        for i in range(count):
            h = int(h_buf[i])
            s = int(s_buf[i])
            v = int(v_buf[i])
            buf_index = i * 3
            
            # 限制範圍
            if s > 4095:
                s = 4095
            if v > 4095:
                v = 4095
            
            # 灰階處理
            if s == 0:
                lum = (v * 255 + 2047) // 4095
                if lum > 255:
                    lum = 255
                buf[buf_index] = lum
                buf[buf_index+1] = lum
                buf[buf_index+2] = lum
                continue
            
            h = h % 360
            region = h // 60
            remainder = (h - (region * 60)) * 4095 // 60
            
            p = (v * (4095 - s)) >> 12
            q = (v * (4095 - ((s * remainder) >> 12))) >> 12
            t = (v * (4095 - ((s * (4095 - remainder)) >> 12))) >> 12
            
            v_8 = (v * 255 + 2047) // 4095
            p_8 = (p * 255 + 2047) // 4095
            q_8 = (q * 255 + 2047) // 4095
            t_8 = (t * 255 + 2047) // 4095
            
            if v_8 > 255: v_8 = 255
            if p_8 > 255: p_8 = 255
            if q_8 > 255: q_8 = 255
            if t_8 > 255: t_8 = 255
            
            if region == 0:
                r, g, b = v_8, t_8, p_8
            elif region == 1:
                r, g, b = q_8, v_8, p_8
            elif region == 2:
                r, g, b = p_8, v_8, t_8
            elif region == 3:
                r, g, b = p_8, q_8, v_8
            elif region == 4:
                r, g, b = t_8, p_8, v_8
            else:
                r, g, b = v_8, p_8, q_8
            
            buf[buf_index] = int(g)
            buf[buf_index+1] = int(r)
            buf[buf_index+2] = int(b)
    
    @micropython.viper
    def _rgb2hsv_buf_index(self, index: int, buf: ptr8, hsv_out: ptr16):
        """
        GRB轉HSV (單個像素) - 精確度優化版
        
        參數:
            index: LED索引
            buf: 輸入buffer [G, R, B]
            hsv_out: 輸出 [H, S, V]
        
        性能: 2.5 μs/像素
        """
        buf_index = index * 3
        r = int(buf[buf_index + 1])
        g = int(buf[buf_index])
        b = int(buf[buf_index + 2])
        
        max_val = r if r > g else g
        max_val = max_val if max_val > b else b
        min_val = r if r < g else g
        min_val = min_val if min_val < b else b
        
        delta = int(max_val - min_val)
        
        # 精確V擴展: 0-255 → 0-4095 (四捨五入)
        v = (int(max_val) * 4095 + 127) // 255
        if v > 4095:
            v = 4095
        
        if delta == 0 or max_val == 0:
            hsv_out[0] = 0
            hsv_out[1] = 0
            hsv_out[2] = v
            return
        
        # 精確S計算 (四捨五入)
        s = (delta * 4095 + (max_val >> 1)) // max_val
        if s > 4095:
            s = 4095
        
        # H計算
        h = 0
        if max_val == r:
            h = (60 * (int(g) - int(b))) // delta
            if h < 0:
                h += 360
        elif max_val == g:
            h = (60 * (int(b) - int(r))) // delta + 120
        else:
            h = (60 * (int(r) - int(g))) // delta + 240
        
        if h >= 360:
            h -= 360
        
        hsv_out[0] = h
        hsv_out[1] = s
        hsv_out[2] = v
    
    @micropython.viper
    def _rgb2hsv_buf(self, buf: ptr8, h_buf: ptr16, s_buf: ptr16, v_buf: ptr16, count: int):
        """
        批量GRB轉HSV - 精確度優化版
        
        參數:
            buf: 輸入buffer [G, R, B, G, R, B, ...]
            h_buf: 輸出色相數組 (0-359)
            s_buf: 輸出飽和度數組 (0-4095)
            v_buf: 輸出亮度數組 (0-4095)
            count: 像素數量
        
        性能: 2.5 μs/像素
        """
        for i in range(count):
            buf_index = i * 3
            r = int(buf[buf_index + 1])
            g = int(buf[buf_index])
            b = int(buf[buf_index + 2])
            
            max_val = r if r > g else g
            max_val = max_val if max_val > b else b
            min_val = r if r < g else g
            min_val = min_val if min_val < b else b
            
            delta = int(max_val - min_val)
            
            v = (int(max_val) * 4095 + 127) // 255
            if v > 4095:
                v = 4095
            
            if delta == 0 or max_val == 0:
                h_buf[i] = 0
                s_buf[i] = 0
                v_buf[i] = v
                continue
            
            s = (delta * 4095 + (max_val >> 1)) // max_val
            if s > 4095:
                s = 4095
            
            h = 0
            if max_val == r:
                h = (60 * (int(g) - int(b))) // delta
                if h < 0:
                    h += 360
            elif max_val == g:
                h = (60 * (int(b) - int(r))) // delta + 120
            else:
                h = (60 * (int(r) - int(g))) // delta + 240
            
            if h >= 360:
                h -= 360
            
            h_buf[i] = h
            s_buf[i] = s
            v_buf[i] = v
    
    # ========================================
    # 控制方法
    # ========================================
    
    def duty(self, lum, ledQ=[]):
        try:
            value = int(lum * self.brightness / 4095)
            
            if self.led_Type in ('esp_LED', 'i2c_LED'):
                if len(ledQ) == 0:
                    self.LED_Buffer = array.array('H', [value] * self.led_IO['Q'])
                else:
                    for i in ledQ:
                        self.LED_Buffer[i] = value
            
            if self.led_Type == 'RGB':
                if len(ledQ) == 0:
                    for i in range(self.led_IO['Q']):
                        self.LED_Buffer[i] = value
                    # 批量更新
                    self._hsv2grb_buf(self.led_H, self.led_S, self.LED_Buffer, self.led.buf, self.led_IO['Q'])
                else:
                    for i in ledQ:
                        self.LED_Buffer[i] = value
                        # 單個更新
                        self._hsv2grb_buf_index(self.led_H[i], self.led_S[i], value, i, self.led.buf)
            
            self.show()
        except BaseException as e:
            print(e)
    
    def set_be_light(self):
        """
        更新所有LED (HSV → RGB)
        
        在show()之前調用,將HSV值轉換為RGB
        """
        try:
            if self.led_Type == 'RGB':
                # ✅ 使用優化版批量轉換 (0-4095範圍)
                self._hsv2grb_buf(self.led_H, self.led_S, self.LED_Buffer, self.led.buf, self.led_IO['Q'])
        except BaseException as e:
            print(e)
    
    def get_rgb_from_hsv(self, index):
        """
        獲取指定LED的RGB值 (從HSV轉換)
        
        返回: (R, G, B) 元組 (0-255)
        """
        if not 0 <= index < self.led_IO['Q']:
            return (0, 0, 0)
        
        try:
            buf = bytearray(3)
            self._hsv2grb_buf_index(self.led_H[index], self.led_S[index], 
                                   self.LED_Buffer[index], index, buf)
            return (buf[1], buf[0], buf[2])  # R, G, B
        except:
            return (0, 0, 0)
    
    def set_hsv_from_rgb(self, index):
        """
        從當前RGB值反向計算HSV (更新led_H, led_S, LED_Buffer)
        
        用於從RGB燈帶讀取顏色並轉換為HSV
        """
        if not 0 <= index < self.led_IO['Q'] or self.led_Type != 'RGB':
            return
        
        try:
            hsv_out = array.array('H', [0, 0, 0])
            self._rgb2hsv_buf_index(index, self.led.buf, hsv_out)
            self.led_H[index] = hsv_out[0]
            self.led_S[index] = hsv_out[1]
            self.LED_Buffer[index] = hsv_out[2]
        except BaseException as e:
            print(e)
    
    def show(self):
        try:
            if self.led_Type == 'esp_LED':
                for i in range(self.led_IO['Q']):
                    self.led[i].duty_u16(self.LED_Buffer[i])
            
            if self.led_Type == 'i2c_LED':
                self.led.buffer = self.LED_Buffer
                self.led.sync_buffer()
            
            if self.led_Type == 'RGB':
                self.led.write()
        except BaseException as e:
            print(e)



