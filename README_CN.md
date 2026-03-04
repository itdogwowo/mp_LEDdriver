# mp_leddriver - ESP32-S3/P4 High Performance LED Driver

本項目利用 ESP32-S3/P4 的 **LCD_CAM I8080 並行接口** 與 **DMA (Direct Memory Access)** 技術，實現多路並行 LED 燈帶驅動。

相比傳統的 RMT 或 SPI 驅動，本驅動具有以下優勢：
*   **極致並行**：支持 8 路或 16 路並行數據輸出，所有通道嚴格同步。
*   **零 CPU 佔用**：數據傳輸完全由 DMA 負責，CPU 僅需在繪圖時介入。
*   **靈活架構**：Bus (總線) 與 Strip (燈帶) 分離，支持混合負載與協議。

---

## 1. 編譯與燒錄

本模塊為 MicroPython User C Module，需要重新編譯固件。

1.  將 `mp_leddriver_src` 目錄複製到 MicroPython 源碼目錄中（例如 `micropython/user_modules/mp_leddriver_src`）。
2.  編譯 ESP32-S3 固件：
    ```bash
    cd micropython/ports/esp32
    make USER_C_MODULES=../../user_modules/mp_leddriver_src/micropython.mk BOARD=ESP32_GENERIC_S3
    ```
3.  燒錄固件到開發板。

---

## 2. 快速上手 (Python API)

### 2.1 初始化並行總線 (I8080 Bus)

首先創建一個 Bus 對象，這相當於定義了物理傳輸通道。

```python
import mp_leddriver
import machine

# 初始化 8 位並行總線
# 注意：引腳號碼需根據實際電路修改
bus = mp_leddriver.I8080_Bus(
    data_pins=[1, 2, 3, 4, 5, 6, 7, 8],  # D0 ~ D7
    clk=9,                               # WR (Write Clock) 引腳
    freq=10000000,                       # 10MHz (WS2812 推薦頻率)
    max_transfer_sz=64000                # 最大 DMA 緩衝區大小 (字節)
)

print("Bus initialized")
```

### 2.2 添加燈帶 (Add Strips)

在 Bus 上掛載燈帶對象。每個 Strip 對應 Bus 上的一個數據位（Pin）。

```python
# Strip 1: 接在 Pin 1 (data_pins[0])，長度 100 顆，WS2812
strip1 = bus.add_strip(
    pin_index=0, 
    length=100, 
    type=0  # 0: WS2812 (目前僅支持 WS2812)
)

# Strip 2: 接在 Pin 2 (data_pins[1])，長度 50 顆
strip2 = bus.add_strip(
    pin_index=1, 
    length=50, 
    type=0
)
```

### 2.3 控制顏色

像操作列表一樣操作 Strip 對象。

```python
# 設置顏色 (R, G, B) - 範圍 0~255
strip1[0] = (255, 0, 0)   # 第一顆燈變紅
strip1[1] = (0, 255, 0)   # 第二顆燈變綠

strip2[0] = (0, 0, 255)   # 第二條燈帶第一顆變藍

# 讀取顏色
r, g, b = strip1[0]
print(f"LED 0 Color: {r}, {g}, {b}")
```

### 2.4 刷新顯示 (Show)

調用 `bus.show()` 會觸發以下動作：
1.  清空 DMA Buffer。
2.  並行編碼所有 Strip 的數據到 Buffer 中。
3.  啟動 DMA 傳輸。

```python
bus.show()
```

---

## 3. 進階用法：直接寫入 Buffer

如果你想繞過 Strip 對象，直接控制 Bus 發送原始波形數據（例如用於調試或特殊協議），可以使用 `write()`。

```python
# 創建一個符合 DMA 要求的 buffer (建議使用 bytearray)
# 注意：長度必須小於 max_transfer_sz
raw_data = bytearray(1000)

# 填充數據 (模擬波形)
# 在 10MHz 下，WS2812 的一個 Bit 需要 13 個字節來表示
for i in range(len(raw_data)):
    raw_data[i] = 0xFF # 全部拉高

# 直接發送
# 如果 raw_data 地址符合 DMA 要求且 4 字節對齊，將零拷貝直接發送
# 否則會自動複製到內部 DMA Buffer 後發送
bus.write(raw_data)
```

---

## 4. 資源釋放與注意事項

### 資源釋放
I8080 外設佔用寶貴的硬件資源。如果程序中斷或重新運行，建議顯式釋放資源。

```python
try:
    # ... 你的代碼 ...
    bus.show()
finally:
    bus.deinit()
    print("Bus released")
```

### 性能貼士
1.  **盡量使用 `bus.show()`**：它在 C 層面並行處理所有通道，比在 Python 中循環寫入要快得多。
2.  **內存管理**：`max_transfer_sz` 決定了內部 DMA Buffer 的大小。對於 8 路並行 WS2812，每顆 LED 需要約 **312 字節** (24 bits * 13 samples) 的緩衝區。
    *   100 顆 LED x 8 路 = ~31KB
    *   1000 顆 LED x 8 路 = ~312KB (需要 PSRAM)

---

## 5. 目前支持的硬件
*   **ESP32-S3** (測試通過)
*   **ESP32-P4** (理論支持，需驗證引腳映射)
