# mp_LEDdriver — 設計討論稿（v0.1 draft）

> **用途**：新 mpy 多 LED 驅動器的 API 與 DMA 架構討論稿。**本階段只討論、不寫程式。**
> **狀態**：草稿，等你確認 §12 的決策點後才進入實作
> **最後更新**：2026-09-13
> **參考**：`mp_Net-Core`（風格母本）、`mp_LEDController`（現行舊版）、`fastLED`（硬體性能目標）、`mp_LVGL` / `mp_heap_caps` / `mp_jpeg`（C module 慣例）
> **建置**：`mp_Make-Tools`（ESP-IDF v5.5 / MicroPython，chips = esp32, esp32s3, esp32p4）

---

## 0. 先講結論（TL;DR）

1. **驅動器的定義只有一句：不斷讀取 RGBW 緩衝，然後非中斷地顯示。**
   不定 fps、不做時間補償；顯示永遠跑在硬體上限（free-running），fps 是**推導出來的結果**，不是設定進去的目標。
2. **「非中斷」有兩層，兩層都要做到**：① 傳輸期間**不關中斷**（DMA 硬體做）② 傳輸期間**不擋 Python**（雙緩衝）。現況 mpy 的 `bitstream` 兩層都違反。
3. **不中斷的價值 = 16 倍吞吐**。WS2812 是 30 µs/LED、單 pin 33,333 顆/秒；mpy 現在的 bitstream 一次只能推**一個** pin，所以 666 顆就是 50 fps 的上限（**你的量測結果，不是需求**）。I80/PARLIO **16 條線同時吐 → 像素吞吐 ×16**，顆數與 fps 的乘積固定在 533,333 顆/秒。
4. **C 後端是必要條件**：16 lane × 666 顆的編碼量在 viper 下要 200–500 ms，比 20 ms 的傳輸時間慢 10–25 倍 → 顯示會被編碼拖慢，fps 掉到硬體上限的 1/20。C 後端目標 **< 1 ms/幀**，讓顯示真正 free-running。純 Python 階段的價值是**定 API + 驗證正確性 + 驗證硬體**。
5. **統一像素格式：每個 LED 一律 4 bytes（R,G,B,W）** —— 你定的規格，本設計完全遵守。關鍵是**它對編碼器是零額外成本**（逐通道查表，只要 R/G/B 就少查一次表），反而省掉效果層的 RGB↔RGBW 重排。建議再加 `write` 遮罩（`0b0111`/`0b1111`/`0b1000`），單色效果可快 4 倍。
6. **DMA 記憶體**：**S3 只能內部 SRAM**（硬限制），**P4 可用 PSRAM**。這正好與你選的策略一致，而且讓 mpy 版比用 PSRAM 的 FastLED 版更穩（無 cache 抖動）。
7. **首發後端是 S3 的 LCD_CAM I80**（不是 RMT）。你每個 slave 跑 **14 條燈帶**，而 **RMT 硬體上限只有 4 條 TX 通道** —— 你現在用 FastLED 的 yves LCD/I80 driver 是唯一解，不是偏好。
8. **P4 用 PARLIO**：16 lane/unit、RAM 更省（40 vs 96 bytes/LED），**P4 有 2 個 TX unit → 32 lane**，且不佔用 LCD_CAM（可留給螢幕）。
9. **緩衝區大小與 lane 數無關**（已驗證）：I80 的配置公式 `24 × bpp × q` 裡**沒有 `numstrips`**，因為 16 lane 共用同一條 16-bit word 流。

---

## 1. 名詞與範圍

### 1.1 要驅動的目標

| 型別 | 協定 | 你主力硬體 |
|---|---|---|
| WS2812 / SK6812 / WS2811 | 800 kHz 單線歸零碼（NRZ + 歸零） | S3、P4 |
| APA102 / SK9822 | SPI（CLK + DATA，無嚴格時序） | S3、P4 |
| PCA9685 | I2C，16 ch × 12-bit PWM | S3、P4 |
| 一般 PWM LED / 電機 / 伺服 | LEDC PWM | S3、P4 |

### 1.2 統一像素格式：**每個 LED 一律 4 bytes（R, G, B, W）**

> **你定的規格，本設計完全遵守。** 理由（你的原話）：*方便不同效果之間的轉換*。

```
像素緩衝（.buf）= 每個 LED 固定 4 bytes：  [R, G, B, W]
    RGB 燈珠  → 讀 R/G/B，W 忽略（或當第 4 個亮度通道）
    RGBW 燈珠 → 讀 R/G/B/W
    單色/PWM  → 只讀 W
    電機/伺服  → 只讀 W（0x80 = 死區停，對齊 mp_Net-Core 的教訓）
```

**為什麼這是對的（而不是「RGB 用 3 bytes 更省」）**：

| 方案 | 記憶體（10,656 顆） | 效果轉換 | 編碼器 |
|---|---|---|---|
| **4 bytes 統一（你的設計）** | 42.6 KB | ✅ 效果輸出格式單一，`write` 遮罩決定用哪幾個通道 | ✅ **編碼器用 stride-4 讀取，完全不需要轉換步驟** |
| 3 bytes RGB / 4 bytes RGBW 混用 | 32–42 KB | ❌ 每個效果要處理兩種 layout | ❌ stride 不固定，編碼器要分支 |

**關鍵洞察：4 bytes 統一面額外成本其實是零。**
編碼器本來就是逐通道查表（`LUT[通道][值]`），所以「只要 R/G/B」就是**少查一次表 + 少 6 個 OR**，不需要任何前置轉換。反過來，混用 layout 才需要在效果層做 RGB↔RGBW 重排。

**建議加上 `write` 遮罩**（沿用你 `mp_Net-Core` 的 `_WRITE_CODES` / `WRITE_WHITELIST` 概念）：

| 遮罩 | 意義 | 編碼器行為 | 每像素成本（viper） |
|---|---|---|---|
| `0b0111` (7) | RGB | 查 3 次表 | 36 取 + 36 OR |
| `0b1111` (15) | RGBW | 查 4 次表 | 48 取 + 48 OR |
| `0b1000` (8) | 單色（W） | 查 1 次表 | 12 取 + 12 OR |
| 自訂 | `r`/`g`/`b`/`w`/`rgb`/`rgbw`/`ww` | 同 `mp_Net-Core` 的 `write` 白名單 | — |

→ **單色燈帶的效果比 RGB 快 3 倍**，這是 4-bytes 統一格式白送的好處。

### 1.3 明確不做（本階段）

- 網路串流協定（那是 `mp_Net-Core` 的職責，本專案只提供 driver 層）
- LVGL / UI
- 效果 DSL 的重新設計（沿用 `LEDMathMethod` / `effect_core` 的波形段格式）

---

## 2. 現況診斷：為什麼現在的 mpy 跑不滿硬體

這一節是後面所有設計決策的根據。三個問題都很具體，不是感覺。

### 2.1 問題一：`neopixel` 在 ESP32 是「每次 write 都重新建 RMT 通道」

你的 `neopixel`（`micropython-lib` 純 Python 版）呼叫 `machine.bitstream(pin, 0, timing, buf)`。
我讀了 MicroPython ESP32 port 的 `machine_bitstream.c`，實作是：

```c
// ports/esp32/machine_bitstream.c:159  每次 write() 都做這整套
static bool machine_bitstream_high_low_rmt(...) {
    rmt_tx_channel_config_t tx_chan_config = {
        .resolution_hz = APB_CLK_FREQ / 2,          // 40 MHz
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,  // ← S3 = 48 words
        .trans_queue_depth = 1,
        // ⚠️ 沒有 .flags.with_dma = 1
    };
    rmt_new_tx_channel(&tx_chan_config, &channel);   // 建
    rmt_enable(channel);
    rmt_new_bytes_encoder(&bytes_encoder_config, &encoder);
    rmt_transmit(channel, encoder, buf, len, &tx_config);
    rmt_tx_wait_all_done(channel, -1);               // 阻塞等到送完
    rmt_del_encoder(encoder);
    rmt_disable(channel);
    rmt_del_channel(channel);                        // 拆
}
```

三個致命點：

| # | 問題 | 後果 |
|---|---|---|
| a | **`mem_block_symbols = 48`（1 個 RMT block）** | 48 symbol = 48 個 WS2812 bit。310 顆燈 = 7440 bit，需要 **約 155 次 ISR refill**，每幀都做 |
| b | **`with_dma` 沒有開** | 只能靠中斷補記憶體，ISR 風暴 |
| c | **建/拆通道 + 阻塞等待** | 每幀的固定開銷；`rmt_tx_wait_all_done(-1)` 整段忙等，CPU 完全卡住 9 ms |

補充數字：310 顆燈的資料 = 7440 bit，在 40 MHz resolution 下 1 bit = 1 個 symbol = 4 bytes → **29.8 KB 的 symbol 資料**，卻只有 48 個 symbol 的硬體 FIFO。這就是「ISR 風暴」的量化定義。

### 2.2 問題二：`write()` 是同步阻塞，render 與傳輸無法重疊

現在的流程是嚴格串列：

```
算一幀 (Python, 數 ms) → write() 阻塞 9 ms → 算下一幀 → ...
```

WS2812 的 9 ms 傳輸時間**完全無法用於計算**。而 FastLED 的核心價值就在這裡：DMA 在背景吐位元，CPU 同時算下一幀。理想情況下面 fps = `1 / max(傳輸時間, 計算時間)`，而不是 `1 / (傳輸 + 計算)`。

對 310 顆燈：現在 ≈ 1/(9ms + 計算)，DMA 化後 ≈ 1/max(9ms, 計算) —— 直接翻倍以上。

### 2.3 問題三：逐像素 Python 迴圈是第二個天花板

`PixelController._convert` 已經是 viper 了，很好。但外層仍有：

- 每幀 Python 層逐 pixel 走訪（`show_all` → 每個 controller 一次 `_convert`）
- HSV → RGB 若走 Python 路徑（單顆 3.4 µs），3000 顆 = 10 ms
- 你實測的 scatter 數字很有價值：**336 顆，純 Python 10.37 µs/顆 → viper 0.43 µs/顆（24×）**

結論：**只有搬進 C 才能再往下一個數量級**。viper 的 0.43 µs/顆 × 3000 顆 = 1.3 ms 已經不錯，但 C 是 0.02 µs/顆 等級（20 ns）。

### 2.4 性能的正確表述：**驅動器的職責是「不斷讀取 RGBW 緩衝然後顯示」**

> **🔧 我先前把「50 fps」誤讀成目標。** 你的原意是：*「這只是我的測試結果，用來指出中斷式 bitstream 在傳輸時間固定下的天花板；不是指定要 50 fps，這應該是可以彈性調整的東西。」*

**所以性能不是本驅動的規格，而是它的自然結果。**

#### 2.4.1 驅動器的一句話定義

```
不斷讀取 RGBW 緩衝 → 非中斷地顯示
        ↑                      ↑
   應用程式持續寫        硬體持續吐，不擋 Python、不關中斷
```

- **不做**：定 fps、做時間補償、做補幀
- **做**：讀緩衝、編碼、丟給 DMA、完成後**立刻**接下一個 frame（free-running）

#### 2.4.2 fps 是**推導出來的**，不是設定進去的

```
每幀傳輸時間 = 最長那條燈帶的顆數 × 30 µs + reset
顯示 fps     = 1 / 每幀傳輸時間              ← 完全由顆數決定
```

| 每 lane 顆數 | 每幀傳輸時間 | 推導出的 fps | 16 lane 的總吞吐 |
|---|---|---|---|
| 200 | 6.0 ms | **166** | 3,200 × 166 = 531,200 顆/秒 |
| 400 | 12.0 ms | **83** | 1,066,560 顆/秒 |
| **666** | **20.0 ms** | **50** | **1,065,600 顆/秒**（你量到的） |
| 1000 | 30.0 ms | **33** | 1,065,600 顆/秒 |
| 2000 | 60.0 ms | **16.6** | 1,065,600 顆/秒 |

> **⭐ 關鍵洞察**：`16 lane × 33,333 顆/秒 = 533,333 顆/秒` 的**像素吞吐量是固定的**。
> 顆數增加，fps 就等比例下降；**兩者的乘積不變**。所以「要幾 fps」根本不是驅動器該回答的問題——
> 驅動器只要保證**永遠跑在硬體上限**（free-running、零空隙），fps 自然落在上表對應的位置。

#### 2.4.3 「非中斷」的兩層意思（兩層都要做到）

| 層次 | 意義 | 現況 | 本專案 |
|---|---|---|---|
| **不關中斷** | 傳輸期間不 `disable_irq` | ❌ mpy `bitstream` 全程關中斷 | ✅ DMA 硬體做，CPU 中斷全開 |
| **不擋 Python** | 傳輸期間 CPU 可繼續執行 | ❌ `write()` 阻塞 | ✅ 雙緩衝，`show()` 立即返回 |

兩層都做到之後，Python 主迴圈才能自由決定「多久寫一次緩衝」，而顯示永遠以硬體上限在跑。**這就是彈性的來源。**

#### 2.4.4 各自獨立的三件事（不要綁在一起）

```
① 顯示節奏（硬體上限，free-running）      ← 驅動器負責，永遠最快
② 應用寫入節奏（效果計算速度）            ← 應用負責，愛多快就多快
③ 同步策略（撕裂 vs 丟幀）                ← 由 show() 的引數選擇（見 API.md §3.5）
```

> 先前我寫的「50 fps 下 666 顆、16 lane 10,656 顆」是**推導值**，不是需求。
> 真正的需求只有一句：**非中斷地、不斷地讀緩衝然後顯示**。

---

## 3. 設計原則（從你的專案提煉，新專案要遵守）

| # | 原則 | 來源 |
|---|---|---|
| 1 | 12-bit（0–4095）+ `array('H')` 是內部通用值域，只有進 8-bit 硬體才 `>> 4` | `PixelMathMethod` |
| 2 | 全程整數，**無浮點、無 `math.sin`、無查表**；用 viper 整數多項式逼近 | `PixelMathMethod` docstring 三條硬約束 |
| 3 | `lib/sw/` 不碰硬體、不碰 bus、不碰 stream —— 純演算法 | `effect_core` / `pixel_layout` |
| 4 | DMA 記憶體只透過 `alloc_dma()` / `free_dma()`，回傳 `(buf, is_dma)` tuple，**永不回 `None`** | `buffer_hub.py` |
| 5 | config.json 是唯一真源，扁平 `{enable, list:[{GPIO:{...}, Q, order, dStay}]}`，`_comment` 直接寫進 JSON | `ws2812_drv.py` |
| 6 | 設定衝突 → 收集 warning 後印，**不 raise**；API 誤用 → raise 英文大寫訊息；週邊 I/O → try/except + log | 三套錯誤策略 |
| 7 | 熱路徑 API 回 `True/False`/`None` 表滿/空，不 raise | `AtomicStreamHub` |
| 8 | 每個 viper 模組都有 `_MP` 雙路徑，PC 可跑自檢 | `pixel_layout.py` |
| 9 | 每個 lib 檔尾 `if __name__ == '__main__':` + `assert` 自檢 | `PixelController.py` |
| 10 | 模組 docstring：`檔名 — 一句話定位` + `設定來源:` / `產物:`；註解寫「為什麼 / 踩過的坑 / 實測數字」 | 全專案 |
| 11 | C module：`ext_mod/mp_<name>/`、`mod<name>.c`、module-level 函式、`MP_REGISTER_MODULE` 兩參形式 | `mp_rs485_hd` / `mp_heap_caps` / `mp_jpeg` |
| 12 | 執行期 log 英文 + `[Tag]`，給人看的設定警告中文；跑 Python 一律 `python -B` | `AGENTS.md` |

---

## 4. 分層架構

```
┌───────────────────────────────────────────────────────────────┐
│  應用 / 播放層（你現有的世界觀，不改）                          │
│  LEDCommander.run_Pattern() / PixelStreamer / Effect.frame()   │
│  → 只吐 array('H') 12-bit 亮度緩衝區                            │
├───────────────────────────────────────────────────────────────┤
│  led_driver/controller.py — PixelController（薄 Python）        │
│  色彩：HSV/S 12-bit、order(RGB/GRB/BGRW…)、dStay 中性值         │
│  職責：把「應用的語意」轉成「驅動器的請求」，不碰 DMA            │
├───────────────────────────────────────────────────────────────┤
│  _led (C) — 真正的驅動器                                        │
│  ┌────────────┬────────────┬────────────┬────────────┐        │
│  │ WS2812     │ APA102     │ PCA9685    │ PWM(LEDC)  │        │
│  │ RMT+DMA    │ SPI+DMA    │ I2C bulk   │ LEDC       │        │
│  └────────────┴────────────┴────────────┴────────────┘        │
│  共用：DMA buffer 池 / 雙緩衝 / 位元編碼 LUT / 完成回呼         │
├───────────────────────────────────────────────────────────────┤
│  ESP-IDF v5.5 週邊驅動                                          │
│  esp_driver_rmt / esp_driver_i2s / esp_driver_parlio /         │
│  esp_driver_spi / esp_driver_i2c / esp_driver_ledc             │
└───────────────────────────────────────────────────────────────┘
```

**關鍵設計決定：分界線畫在 12-bit 亮度緩衝區。**

- 應用層（你的效果引擎、網路串流）只認 12-bit。
- `_led` 只認 8-bit bytes。
- 轉換（含 order 重排、全域亮度、HSV→RGB、gamma）全部在 C，用 LUT + viper 等級的迴圈一次掃完。

---

## 5. 硬體後端選擇與 DMA 策略

### 5.1 WS2812 / 歸零碼 — 後端總表（已用 FastLED 實證數字校正）

| 後端 | 每 LED bit 的 RAM | 1000 顆 / 16 lane 的 DMA buffer | 平行通道 | 複雜度 | 可用晶片 |
|---|---|---|---|---|---|
| RMT + DMA | **4 bytes** | 96 KB × 16 = 1.5 MB ❌ | **4**（硬限制） | 低 | S3、P4 |
| LCD_CAM I80（yves 手法） | **3 bytes** | **72 KB** ✅ | **16** | 中 | **只有 S3**（P4 是 RGB 版） |
| LCD_CAM RGB（P4 版） | 4 bytes（4 pixel/bit × 16-bit） | 96 KB ✅ | **16**（理論 24） | 中 | 只有 P4 |
| PARLIO（Wave8） | **3.75 bytes**（30 bytes/LED/16lane） | 60 KB ✅ | **16**（P4 有 2 個 TX unit → 32） | 中 | P4、C6、H2、C5 |
| PARLIO（Wave3） | **0.375 bytes** | 6 KB ✅✅ | 16 | 中 | 同上 |

**校正後的關鍵結論（與我最初的直覺相反）：**

1. **單條 / 少條燈帶 → RMT + DMA 最佳**（4 bytes/bit，最省 RAM，且 S3/P4 都有）。適合行動電源、小裝置。
2. **多條燈帶（你的主場景）→ RMT 直接出局**：`SOC_RMT_TX_CANDIDATES_PER_GROUP = 4`，S3/P4 用 RMT **最多 4 條**。你的 FastLED 配置是 **14 條**，遠超上限。
3. **所以你在 S3 上用 LCD_CAM I80 是唯一正解，不是偏好**。這也解釋了你為什麼要 patch yves driver——那條路是對的。
4. **P4 用 PARLIO 比 S3 的 I80 更好**：RAM 一樣省（60 KB vs 72 KB），但可以開 **2 個 TX unit = 32 lane**，且不佔用 LCD_CAM（可留給螢幕）。

### 5.1.1 S3 的 LCD_CAM I80 編碼（FastLED yves driver，你正在用的）

我讀了 `FastLED/src/third_party/yves/I2SClockLessLedDriveresp32s3/src/I2SClockLessLedDriveresp32s3.h`（3.10.3）。機制：

| 項目 | 值 | 出處 |
|---|---|---|
| 週邊 | `esp_lcd_new_i80_bus()` + `esp_lcd_new_panel_io_i80()` | `_initled()` |
| bus_width | **16** | `bus_config.bus_width = 16` |
| PCLK | **2.4 MHz**（`24 * 100 * 1000`） | `FASTLED_ESP32S3_I2S_CLOCK_HZ` |
| 每 bit 時鐘數 | **3**（3 × 416.7 ns = 1.25 µs） | 由 clock 反推 |
| 0 碼 / 1 碼 | `0b100` / `0b110` | 由 `transpose16x1` 的 `0xFFFF` 填入推得 |
| 每 LED 資料 | `8 × bpp × 3 × 2` = **每通道每 LED 24 bytes**（`bpp` = 通道數，RGBW=4 → 96 bytes/LED） | `heap_caps_aligned_alloc` 大小 |
| 對齊 | **64 bytes**（`LCD_DRIVER_PSRAM_DATA_ALIGNMENT`） | 同上 |
| 記憶體位置 | `MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT` + `heap_caps_aligned_alloc` | 同上 |
| 雙緩衝 | `uint16_t *buffers[2]` + **完成回呼 + FreeRTOS semaphore** | `flush_ready()` / `show()` |

**核心手法是「位元轉置」（bit transpose）**：

```
對第 j 顆像素、第 i 條燈帶：
    byte_to_bitplane[i] = color[i][j]              # 16 條燈帶 → 16 個 byte
    (R, G, B 各一份，經 gamma/brightness LUT)
接著 transpose16x1_noinline2()：
    把 16 個 lane 的同一個 bit 收集成一個 16-bit word
    → 每個 LED bit 產生 3 個 16-bit word（0b100 / 0b110）
輸出格式：每 LED 每 component = 24 個 uint16 = 48 bytes
         （3 word/bit × 8 bit = 24 word = 48 bytes）
```

換句話說：**硬體一次吐 16 條燈帶的同一個 bit**，所以資料量與燈帶數無關（16 條以內）。

**⚠️ 你 patch 掉的那個 bug 要記得**：`esp_lcd_panel_io_i80_config_t io_config;` 沒初始化，`flags` 位元欄帶著堆疊垃圾 → WS2812 時序偶發錯誤 → 冷開機「整條全白」。必須 `= {}`。**我們的 C 版本要用 `= {}` 或 `memset` 從一開始就避免。**

### 5.1.2 P4 的 PARLIO 編碼（FastLED 3.10.4 channel driver）

出處：`/tmp/fastled_src/src/platforms/esp/32/drivers/parlio/README.md` + `parlio_buffer_calc.h`。

| 項目 | Wave8（預設） | Wave3 |
|---|---|---|
| 每個 LED bit 展開 | 8 pulses | 3 ticks |
| 時鐘 | **8.0 MHz**（125 ns/tick） | — |
| 每 component 波形 | 8 bit × 10 tick = **80 bit** | 8 bit × 3 tick = **24 bit** |
| 每 LED（RGB） | 240 bit = **30 bytes** | 24 × 3 = **9 bytes** |
| 1000 顆 × 16 lane | 480 KB（雙緩衝）→ 分塊送 | 144 KB |

- 每個 LED bit 用 **10-bit 視窗**：`0` = 3 tick 高 + 7 tick 低（375/875 ns）；`1` = 7 高 + 3 低（875/375 ns）。8 MHz × 10 tick = 1.25 µs，剛好。
- **DMA 單筆上限 65,535 bytes** → 驅動自動分塊（16 lane 時每塊約 273 顆）。
- **環形緩衝區（ring buffer）上限**：P4 內部 512 KB、PSRAM **2 MB**。
- 官方聲稱：**8 條 × 1000 顆 > 100 FPS**。
- **P4 有 2 個 PARLIO TX unit**，每個 16 lane → 理論 **32 條燈帶**（與 LCD_CAM 並用可更多）。
- **注意**：FastLED 的 PARLIO 實作**沒有用 BitScrambler**（那個 API 存在於 IDF 5.5，我在 header 確認過），是軟體展開進 ring buffer + DMA。我們可以先用同樣做法（穩、有參考），BitScrambler 當後續優化。

### 5.1.3 RMT + DMA（單／少條燈帶用）

FastLED 在 `drivers/rmt/rmt_5/` 有完整的 IDF5 RMT 實作。我們的用法更簡單：

```c
rmt_tx_channel_config_t cfg = {
    .gpio_num = pin,
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 10 * 1000 * 1000,   // 10 MHz，1 tick = 100 ns
    .mem_block_symbols = 4096,           // DMA 模式下可放大（不再是 48）
    .trans_queue_depth = 2,              // 雙緩衝佇列
    .flags.with_dma = 1,                 // 🔥 關鍵
};
```

**WS2812 專門的 encoder（不用 `rmt_new_bytes_encoder`）**：
`rmt_new_bytes_encoder` 是「照 bit 展開」，但它**每個 bit 都要走 ISR encoder 邏輯**。更好的做法是自訂 encoder：把整個 frame 一次展開成 `rmt_symbol_word_t` 陣列（用 256-entry × 8-symbol 的 LUT 記憶體展開），再一筆 `rmt_transmit` 送完。

| 時序 | 值 | 10 MHz ticks |
|---|---|---|
| T0H | 0.35 µs | 3–4 |
| T0L | 0.90 µs | 9 |
| T1H | 0.70 µs | 7 |
| T1L | 0.55 µs | 5–6 |
| reset | > 50 µs | — |

RAM：**4 bytes/LED bit = 12 bytes/LED**（1000 顆 = 12 KB）。這是所有方案裡最省的「每條燈帶」成本，但只能 4 條。

**IDF 5.5 已確認可用的東西**（我查了 `mp_Make-Tools/lib/esp-idf`）：

```c
// components/esp_driver_rmt/include/driver/rmt_tx.h:44
uint32_t with_dma: 1;   /*!< If set, the driver will allocate an RMT channel with DMA capability */
```

```c
// components/soc/esp32s3/include/soc/soc_caps.h
#define SOC_RMT_TX_CANDIDATES_PER_GROUP  4
#define SOC_RMT_CHANNELS_PER_GROUP       8
#define SOC_RMT_SUPPORT_DMA              1   // S3 有
// esp32p4 同樣 SOC_RMT_SUPPORT_DMA 1
#define SOC_PARLIO_SUPPORTED             1   // 只有 P4
#define SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH 16 // P4 PARLIO 16 lane
```

**還有三個很好用的 IDF 機制（本機 header 已確認存在）：**

| API | 位置 | 用途 |
|---|---|---|
| `rmt_new_sync_manager()` | `rmt_tx.h:161` | 多個 TX 通道「同步啟動」，消除燈帶間相位差 |
| `rmt_transmit_config_t.flags.queue_nonblocking` | `rmt_tx.h` | 佇列滿不阻塞，直接返回 |
| `rmt_tx_register_event_callbacks()` + `on_trans_done` | `rmt_tx.h:26,143` | DMA 完成回呼，讓 `show()` 可完全非同步 |
| `rmt_tx_wait_all_done(ch, timeout_ms)` | `rmt_tx.h:126` | `timeout_ms = 0` 可做非阻塞輪詢 |

**RMT 通道數的硬限制**（這決定了「單晶片最多幾條 WS2812」）：

```
SOC_RMT_TX_CANDIDATES_PER_GROUP = 4     # S3 與 P4 都一樣
SOC_RMT_CHANNELS_PER_GROUP      = 8     # 8 個 channel，只有 4 個能 TX
```

→ **S3 與 P4 用 RMT 最多同時 4 條燈帶**（除非用 LP RMT 或借 I2S/PARLIO）。這是 §5.2 平行方案的直接動機。

**雙緩衝設計（這是性能翻倍的關鍵）：**

```
DMA buffer 池：每個通道 2 塊（ping-pong）
  show()：
    1. 確認上一塊已送完（沒送完 → 只等「差值」，不是等全部）
    2. 把 Python 的 array('H') 掃進下一塊（C 迴圈 + LUT + order 重排）
    3. rmt_transmit(next, ..., queue_nonblocking=1)
    4. 立刻返回 → Python 繼續算下一幀
  wait()：
    等到全部送完（給需要精確 fps 控制的場景）
```

**每個 LED bit 的編碼用 LUT，不用逐位元 if：**

```c
// 開機時建一次：256 個 byte → RMT symbol 對
// 1 byte 的 8 個 bit 展開成 8 個 symbol（每 symbol 代表 1 個 WS2812 bit）
static rmt_symbol_word_t s_bit_lut[2];   // [0] = bit 0 波形, [1] = bit 1 波形
// 對每個 byte b:  for (j = 7; j >= 0; j--) out[i++] = s_bit_lut[(b >> j) & 1];
```

WS2812 時序（用你最常遇到的 GRB）：
- 0 碼：T0H = 0.35 µs，T0L = 0.90 µs（週期 1.25 µs）
- 1 碼：T1H = 0.70 µs，T1L = 0.55 µs
- reset：> 50 µs 低電位

40 MHz resolution 下 1 tick = 25 ns，時序精度 ±25 ns，足夠。

### 5.2 平行多通道 — 這才是重點（校正版）

單條燈帶的物理上限是 **33,333 顆/秒**（1.25 µs × 24 bit）：
- 310 顆 → 最高 ~111 fps
- 1000 顆 → 最高 ~33 fps
- 200 顆（你的 slave 配置）→ 最高 ~166 fps

**但平行之後總 throughput 是 × N**（N = lane 數），而且**每 lane 的更新速率完全不變**。所以：

| 平台 | 首選方案 | lane 數 | 每 LED RAM | 備註 |
|---|---|---|---|---|
| **S3** | **LCD_CAM I80** | 16 | 3 bytes | **唯一能上多條的路**（RMT 只有 4） |
| S3 | RMT + DMA | 4 | 4 bytes | 少條、無 PSRAM 時的選擇 |
| **P4** | **PARLIO ×2 unit** | **32** | 3.75 bytes | 最強，且不佔 LCD_CAM |
| P4 | LCD_CAM RGB | 16（理論 24） | 4 bytes | 與 PARLIO 並用可更多 |
| P4 | RMT + DMA | 4 | 4 bytes | 少條用 |

**你的實測配置**（從 `fastLED/platformio_local.ini` 讀到）：每個 S3 slave 跑 **14 條燈帶**（RGB1/2/3/4/7/8/9/10/11/12/13/16/17/18），最大 200 顆，其餘 10–100 顆。這個規模：
- ❌ RMT 做不到（只有 4 條）
- ✅ LCD_CAM I80 剛好夠（16 lane，用 14）

**所以路線圖修正為：**
1. **P0：S3 直接做 LCD_CAM I80 後端**（對齊你現在的 FastLED 能力），RMT 當「少條/無 PSRAM」的備援後端。
2. **P1：P4 做 PARLIO 後端**（比 S3 更強，32 lane）。
3. **P2：雙後端並用**（P4：PARLIO + LCD_CAM RGB 同時開，衝 40+ lane）。

### 5.3 APA102 — SPI + DMA

APA102 不需要精確時序（有 CLK 線），所以 SPI 天生合適，而且 **RAM 只要 4 bytes/LED**（亮度頭 + BGR）。

```
SPI frame = [0x00 × 4] + [0xE0|bri5, B, G, R] × N + [0xFF × ceil(N/16)]
```

- 你的 `APA102.py` 已經有 `spi_buffer` 的完整框架，很好，改成 C 版本即可。
- 用 `spi_device_transmit()` 一次送完整幀（DMA）。**關鍵：CS 全程不回拉**，只靠 clock stretching 也不會壞（APA102 是 clocked 的）。
- baudrate 8–20 MHz：290 顆 × 4 bytes = 1160 bytes → 8 MHz 下 **1.2 ms**。這比 WS2812 快一個數量級。
- S3/P4 都有硬體 SPI + DMA，P4 還有專用 SPI 週邊可挑。

### 5.4 PCA9685 — 一次 I2C 寫完整幀

你現在的問題是逐通道、逐晶片寫。改進：

- PCA9685 支援 **auto-increment**：從 `LED0_ON_L` 開始連續寫 64 bytes 就能一次更新 16 通道 × 12-bit。
- **每幀每晶片只發 1 筆 I2C transaction**（68 bytes）。
- 你已經用 `0x70` ALLCALL 廣播（datasheet Fig.25）—— 這是對的，保留。廣播時不管幾塊板子都只有 1 筆 transaction。
- I2C 1 MHz 下，68 bytes ≈ 0.68 ms/晶片；16 塊 = 11 ms，**這會變成瓶頸**，所以：
  - 優先推廣播（已做）
  - 支援多條 I2C bus 分散負載
  - 12-bit 直寫（`v * 16 + (v >> 8)`），不要 `(v << 4) | (v >> 4)`
  - 若要更猛，P4 有 I2C DMA 可用

### 5.5 PWM LED / 電機 — LEDC

- S3 / P4 都是 **LEDC 8 通道（4 timer × 2 channel）**。
- `machine.PWM` 每顆燈一個 Python 物件 → 3000 顆不可能。改成 C 層一次配置 + 批次 `ledc_set_duty` + 一次 `ledc_update_duty`。
- **`SOC_LEDC_SUPPORT_FADE_STOP` S3/P4 都有** → 可以用硬體 fade 做呼吸燈（連 CPU 都不用）。
- 12-bit 值直進 LEDC 的 duty（LEDC 支援到 14-bit），不用 `>> 4` 掉精度。
- gamma 校正表在 C 層預算好。

---

## 6. RAM 預算（已修正 + 加入 DMA 記憶體限制）

### 6.0 ⚠️ DMA 緩衝區的記憶體限制（你指出的重點）

| 晶片 | DMA 緩衝區可放哪 | 本專案策略 |
|---|---|---|
| **ESP32-S3** | **只有內部 SRAM**（`MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL`） | 內部 SRAM，**完全不碰 PSRAM** |
| **ESP32-P4** | 內部 SRAM **或** PSRAM | 優先用內部 SRAM；真的不夠才用 PSRAM |

> 這正好與你選的「優先使用內部 SRAM，不用 PSRAM」一致，而且在 S3 上**不是偏好而是硬限制**。
>
> **副作用（正面）**：內部 SRAM 的 DMA 沒有 cache 一致性問題、沒有 PSRAM 的頻寬競爭與延遲抖動 → **mpy 版比 FastLED 版（用 PSRAM）更穩**。
>
> **副作用（要留意）**：S3 內部 SRAM 只有 512 KB，扣掉 MicroPython heap、WiFi/LWIP、LVGL 之後可用不多。所以下面的 §6.2 計算是**必須遵守的預算表**。
>
> 註：FastLED 的 PARLIO driver 有 `FASTLED_PARLIO_MAX_RING_BUFFER_TOTAL_BYTES_PSRAM`（P4 2 MB / 其他 1 MB）的 PSRAM 路徑——那在 P4 才成立。

### 6.1 每 LED 成本

| 後端 | 每 RGB LED | 每 RGBW LED | 666 顆（單／雙緩衝） |
|---|---|---|---|
| **LCD_CAM I80（S3）** | **72 bytes** | 96 bytes | **46.8 / 93.6 KB** |
| **PARLIO Wave8（P4）** | 30 bytes | 40 bytes | 19.5 / 39.0 KB |
| PARLIO Wave3 | 9 bytes | 12 bytes | 5.9 / 11.7 KB |
| RMT + DMA（**每 lane 各自一份**） | 12 bytes | 16 bytes | 7.8 / 15.6 KB × lane |

> **✅ 已驗證**：**I80 / PARLIO 的緩衝區大小只與「每條燈帶幾顆」有關，與 lane 數無關。**
> yves driver 的配置 = `8 × bpp × NUM_LED_PER_STRIP × 3 × 2` = `24 × bpp × q`，**公式裡沒有 `numstrips`**（16 lane 共用同一條 16-bit word 流）。
> **例外是 RMT**：每個 RMT 通道有獨立 DMA buffer，那個才隨 lane 線性成長。

### 6.2 滿載場景預算表（16 lane × 666 顆 = 10,656 顆）

| 項目 | 計算 | 大小 |
|---|---|---|
| I80 DMA buffer（單緩衝） | `24 × 4 × 666`（RGBW 4 通道） | **62.4 KB** |
| 雙緩衝 | ×2 | **124.9 KB** |
| 像素緩衝 `.buf`（4 bytes/LED） | `10,656 × 4` | 42.6 KB |
| 編碼 LUT（4 通道 × 256 值 × 24 word） | | 49.2 KB |
| **合計** | | **約 217 KB** |

→ **S3 的 512 KB SRAM 吃得下，但必須省著用**（還要留給 MicroPython heap、WiFi、LVGL）。
→ 若同時跑 LVGL + WiFi，建議降到 **8 lane × 666 顆**（合計約 170 KB），或把 DMA buffer 改單緩衝（省 62 KB）。
→ **P4 則完全無壓力**：PARLIO 只有 `24 × 4 × 666 = 62.4 KB`… 實際上更低（Wave8 = 40 bytes/LED → **26.6 KB** 單緩衝），必要時還能吃 PSRAM。

#### 6.2.1 RGB 遮罩能省多少

若該幀只用 RGB（`write=0b0111`），**DMA buffer 不變**（word 數固定），但編碼量降為 3/4：

| 遮罩 | 每像素查表次數 | 編碼量（16×666） | 相對成本 |
|---|---|---|---|
| RGBW (15) | 4 | 32 萬次 | 100% |
| RGB (7) | 3 | 24 萬次 | 75% |
| 單色 (8) | 1 | 8 萬次 | **25%** |

→ **單色燈帶的效果便宜 4 倍**，這是 4-bytes 統一格式白送的好處。

### 6.3 你的實際場景（14 條 × 200 顆）

| 項目 | 計算 | 大小 |
|---|---|---|
| I80 DMA buffer | `24 × 4 × 200` | **18.8 KB** 單 / **37.5 KB** 雙 |
| 像素緩衝 `.buf` | `14 × 200 × 4` | 11.2 KB |
| 編碼 LUT（4 通道） | | 49.2 KB |
| **合計** | | **約 98 KB** |

→ 輕鬆。這個規模下 mpy 版可以很奢侈地用雙緩衝 + RGBW + gamma。

---

## 7. Python API 設計

### 7.1 設計取捨

| 選項 | 說明 | 評價 |
|---|---|---|
| A. 純 Python，`neopixel` drop-in | 保持 `.buf` / `.write()` 介面不變 | ✅ 相容性最好，❌ 性能上限 = 現在 |
| B. **C class + 薄 Python 控制器** | C 建立物件與 method，Python 包一層語意層 | ✅ **推薦**：性能 + 風格兼顧，符合 `mp_jpeg` 的 `Decoder` class 慣例 |
| C. 純 C，Python 只是薄殼 | 連效果引擎都進 C | ❌ 失去 mpy 的開發效率，違背你的風格 |

**選 B。** 而且刻意讓 C 物件保持「跟 `neopixel.NeoPixel` 同形狀」（有 `.buf` writable memoryview、有 `.write()`），這樣你 `PixelController` 的 `st_load_and_convert` / `st_show` **一行都不用改**就能吃到 DMA。

### 7.2 模組命名

| 層 | 名稱 | 理由 |
|---|---|---|
| C user module | `_led`（私有） | 對齊 `mp_rs485_hd` 的全小寫慣例；底線前綴表示「不要直接 import」 |
| Python 套件 | `led` | `from led import LEDCommander, init_led` |
| ext_mod 目錄 | `ext_mod/mp_led/` | `mp_<name>` |

```
mp_LEDdriver/
├── DESIGN.md                        ← 本文件
├── README.md
├── LICENSE
├── mp_led/                          ← C user module（照 mp_rs485_hd 模板）
│   ├── micropython.cmake
│   ├── micropython.mk
│   ├── modled.c                     ← 模組註冊 + globals table
│   ├── led_common.h/.c              ← 共用：DMA buffer 池、雙緩衝狀態機、完成回呼
│   ├── led_ws2812_i80.c             ← 🔥 S3 主力：LCD_CAM I80 平行（位元轉置）
│   ├── led_ws2812_parlio.c          ← 🔥 P4 主力：PARLIO ring buffer（Wave8）
│   ├── led_ws2812_rmt.c             ← 備援：RMT + DMA（≤4 條／小裝置）
│   ├── led_ws2812_lcd_rgb.c         ← P4 選配：RGB LCD 控制器
│   ├── led_apa102_spi.c             ← APA102 / SPI + DMA
│   ├── led_pca9685_i2c.c            ← PCA9685 / I2C bulk（auto-increment 一次寫完）
│   ├── led_pwm_ledc.c               ← LEDC PWM（8 通道 + 硬體 fade）
│   └── led_color.c                  ← HSV→RGB、order 重排、gamma/brightness LUT
├── led/                             ← Python 套件（風格層）
│   ├── __init__.py
│   ├── LEDCommander.py
│   ├── LEDController.py
│   ├── LEDMathMethod.py
│   └── BufferReader.py
├── tests/                           ← 每檔尾自檢 + REPL 測試腳本
├── tools/                           ← benchmarks、config 產生器
└── ports_config/                    ← S3 / P4 範例 config.json
```

### 7.3 底層 C API（`import _led`）

照 `mp_rs485_hd` 的 module-level 函式慣例 + `mp_jpeg` 的 class 慣例混合。
**`_led` 的 Python 名一律 snake_case**。

```python
import _led

# ── 建驅動器（一個 channel = 一個物件）───────────────────────────
ch = _led.ws2812(
    pin=5,                    # GPIO
    q=310,                    # 顆數
    order="GRB",              # 色序（"GRB" / "RGB" / "BGR" / "GRBW" …）
    backend="rmt",            # "rmt"（預設，≤4 條）/ "i80"（S3，16 條）
                              # / "parlio"（P4，16 條 ×2 unit）/ "lcd_rgb"（P4）
    dma=True,                 # 預設 True；False = 退回舊行為做對照
    double_buffer=True,       # 預設 True
)

# ── 多條燈帶：一次建一整組（才吃得到平行匯流排）─────────────────
bus = _led.ws2812_bus(
    pins=[1,2,3,4,7,8,9,10,11,12,13,16,17,18],   # 14 條，一個 I80 匯流排
    q=200,                    # 每條顆數（可傳 list 做不等長）
    order="GRB",
    backend="i80",            # S3 自動選；P4 會選 "parlio"
    # 內部：一個 esp_lcd_i80_bus + 雙緩衝 + 完成回呼
)
bus.buf            # memoryview，形狀為 [lane][led][3]（或 views() 取每條的 view）
bus.views()        # list of memoryview，每條燈帶一個 → 直接餵給現有 PixelController
bus.show()         # 一次更新全部 14 條（硬體平行）
bus.wait()         # 等送完

# ── buffer 存取（writable memoryview，給 viper ptr8 用）────────────
buf = ch.buf                  # memoryview，n * 3 bytes，形狀同 neopixel.NeoPixel.buf
buf16 = ch.buf16              # memoryview('H')，n 值 12-bit（給效果引擎）

# ── 顯示 ────────────────────────────────────────────────────────
ch.show()                     # 非阻塞（雙緩衝）：編碼 + 丟給 DMA + 立刻返回
ch.show(wait=True)            # 阻塞到送完（精確 fps 控制用）
ch.wait()                     # 等上一次送完
ch.busy()                     # 上一次是否還在送

# ── 批次操作（全部 C 迴圈）──────────────────────────────────────
ch.fill(0)                                  # 填 8-bit RGB
ch.fill_hsv(h=120, s=4095, v=2048)          # HSV，C 轉換
ch.load16(v12)                              # 從 array('H') 12-bit 載入（含 order 重排 + 亮度）
ch.set_brightness(2048)                     # 全域亮度 0-4095（12-bit，不掉精度）
ch.set_neutral(0)                           # dStay：停止/熄燈時回填的值

# ── 統計（給 debug 與 benchmark）────────────────────────────────
ch.stats()      # {'frames':…, 'last_encode_us':…, 'last_dma_us':…, 'dma_blocked_us':…}

# ── APA102（同一家族的 SPI 後端）───────────────────────────────
apa = _led.apa102(spi=0, sck=12, mosi=11, q=290, order="BGRW",
                  baudrate=10_000_000, dma=True)

# ── PCA9685 ───────────────────────────────────────────────────
pca = _led.pca9685(i2c=0, scl=47, sda=48, address=0x70, q=16, freq=1000)
pca.show()      # 一筆 I2C transaction 寫完 16 通道

# ── LEDC PWM ──────────────────────────────────────────────────
pwm = _led.pwm(pins=[11,12,13,14], freq=1000, resolution=12)
pwm.load16(v12)  # array('H') 12-bit 直接進 duty，不掉精度
pwm.show()

# ── 模組資訊 ──────────────────────────────────────────────────
_led.version()
_led.info()     # 各後端可用性、DMA 記憶體用量、殘餘 heap
```

### 7.4 `show()` 語意：把「同步策略」交給使用者選（最重要的 API 決定）

**核心原則：顯示要 free-running（永遠跑硬體上限），應用寫入節奏自由，兩者解耦。**

```python
bus.show(mode="auto")     # 預設：能送就送；送不了就等最舊那塊（只等「剛好夠」）
bus.show(mode="skip")     # 🔥 顯示優先：上一幀還在跑 → 丟棄這幀、立即返回、完全不擋 Python
bus.show(mode="block")    # 應用優先：等送完才返回（= 舊 neopixel.write() 語意）
bus.show(mode="latest")   # 覆蓋優先：可覆蓋未送出的那塊（單緩衝，可能撕裂）
```

| mode | 顯示停頓 | Python 被擋 | 撕裂 | 說明 |
|---|---|---|---|---|
| `"auto"` | 無 | 只等最舊那塊 | 無 | 通用，雙緩衝 |
| **`"skip"`** | **無** | **完全不擋** | 無 | 🔥 **「非中斷地不斷顯示」的主模式** |
| `"block"` | 無 | 擋到送完 | 無 | 相容舊行為 |
| `"latest"` | 無 | 完全不擋 | ⚠️ 有 | 單緩衝，最省 RAM |

**為什麼不預設「全非阻塞 + 第三塊 buffer」**：三塊 buffer 在 S3（DMA 只能內部 SRAM）上 RAM 吃不消，而且讓「丟幀」變得不可預測。**雙緩衝 + `mode` 選擇**是記憶體與行為的最佳平衡。

**顯示的 free-running 保證**：只要編碼時間 < 傳輸時間，`mode="skip"` 之下顯示就永遠不會停頓。這是 C 後端的核心 KPI（目標 < 1 ms vs 傳輸 20 ms）。

> **`daemon` 模式（P2 選配）**：`bus.start()` 讓 C 層在 DMA 完成回呼裡直接鏈下一個 transfer，零 Python 介入、零空隙。屆時 Python 端只需要「讀緩衝然後顯示」的最純粹形式。先用 `mode` 把顯式語意定清楚，daemon 之後再加。

### 7.5 高階 API（`led` 套件，保持你的世界觀）

底層換掉，但**上層語意完全不變**，這樣你現有的 `main.py` / `pixel_drv.py` 幾乎不用改：

```python
from led import LEDCommander, init_led, init_rgb

# 相容舊簽名：init_led(config['LED_IO'])
led_list = init_led(config)          # 內部自動依 config 選後端
rgb_list = init_rgb(config)

ledC = LEDCommander(led_list, rgb_list)
ledC.init_all()

# 動畫：完全沿用你現有的 pattern 格式
ledC.run_Pattern(led_init, gap_Time=20, run_time=200, encoder=4095, debug=True)
ledC.show_all(3, bright=4095)
```

`LEDCommander` 內部改動（性能關鍵）：

```python
@micropython.native
def show_all(self, channel=3, bright=4095):
    for c in self.all_controllers:
        c.set_brightness(bright)      # 設定一次，不是逐顆
        c.show()                      # 非阻塞
```

**全域亮度不再逐 pixel 乘**，改成：C 層在 `load16()` 內用 `(v * bri) >> 12`，或走 gamma LUT 的 index 偏移，一次掃完。

### 7.6 與 `neopixel` 的相容性

本專案的像素緩衝是 **4 bytes/LED `[R,G,B,W]`**（你定的規格），而 `neopixel.NeoPixel.buf` 是 **3 bytes/px**。完整的三條相容路徑寫在 `API.md` §5，摘要如下：

| 路徑 | 做法 | 改動量 |
|---|---|---|
| **① 轉接層（推薦）** | `led.neopixel_compat(pin, q)` 提供 3-bytes 外觀、內部 stride-4 | **0 行**（`PixelController` 不動） |
| ② 改 `PixelController._convert` | `d_idx = i * bpp` → `i * 4`，寫 R/G/B 三個 offset | 3 行 |
| ③ 直通模式 | 用 `load()` / `load16()` 餵 4-byte 資料 | 改呼叫端 |

```python
# 路徑 ①：你現有的 PixelController 完全不用改
dst = self.hw.buf          # ← 轉接層提供的 3-byte 外觀（內部 stride 4）
self.hw.write()            # ← 走 I80 DMA，非阻塞
```

> **⚠️ 陷阱**：**order 重排只能在一處發生**。① 讓 `PixelController` 重排（`led_bus` 的 order 設 raw 順序）；③ 讓 `led_bus` 依 `order="GRBW"` 重排。兩個都做 → 顏色 double-swap。

---

## 8. DMA 性能最大化：具體手段清單

| # | 手段 | 效果 | 階段 |
|---|---|---|---|
| 1 | `with_dma = 1` | 消滅 155 次/幀 ISR refill | P0 |
| 2 | 雙緩衝 + 非阻塞 transmit | 傳輸與計算重疊，fps 翻倍 | P0 |
| 3 | 開機建通道，之後只 transmit | 省掉每幀建/拆 | P0 |
| 4 | 位元編碼 LUT（`rmt_symbol_word_t[2]`） | 編碼從「逐位元 if」變 array indexing | P0 |
| 5 | 顏色轉換 + order 重排全部進 C | 3000 顆從 ~12 ms → <0.1 ms | P0 |
| 6 | 一次大 `rmt_transmit`（整幀一筆） | 不要分塊送 | P0 |
| 7 | `rmt_sync_manager` 同步多通道 | 多燈帶零相位差 | P1 |
| 8 | I2S 3-word/bit 平行 8–16 lane | S3 大陣列 | P1 |
| 9 | PARLIO + BitScrambler（P4） | 16 lane、24 KB、硬體展開 | P1 |
| 10 | DMA 完成回呼 + `_thread` 餵幀 | 完全非同步渲染管線 | P2 |
| 11 | LEDC 硬體 fade（`SOC_LEDC_SUPPORT_FADE_STOP`） | 呼吸燈零 CPU | P2 |
| 12 | 髒矩形/髒通道追蹤 | 只重編碼變化過的通道 | P2 |
| 13 | PSRAM 大 buffer + 內部 DMA bounce | S3 放不下的場景 | P2 |

---

## 9. 待驗證的技術風險

| # | 風險 | 驗證方式 |
|---|---|---|
| **R0** | 🔥 **yves I80 編碼器的實際波形語意未完全確認**：我驗證出「每 bit → 3 個 16-bit word，內容 `[mask, 0, 0]`」，但 `initled()` 又把每個 3-word 群的**第 2 個 word 預填 `0xFFFF`**，且轉置會覆寫它 → 實際波形是 `[mask, mask, 0]`，**不像標準 WS2812 的 `0b100` / `0b110`**。三個可能：word↔PCLK 對應有誤 / 哨兵 index 讀錯 / 實際 clock 與我推的不同 | 邏輯分析儀量一條燈帶的實際波形；或讀 `esp_lcd` I80 的 PCLK 產生邏輯。**這會阻塞 C 後端的正確實作，但不阻塞 API 設計** |
| R1 | MicroPython ESP32 port 能否直接 `#include "esp_lcd_panel_io.h"` 與 `esp_lcd_io_i80.h` | 編最小 C module（照 `mp_rs485_hd` 的 `idf_component_get_property` 手法，拉 `esp_lcd` INCLUDE_DIRS）|
| R2 | S3 的 LCD_I80 DMA buffer 能否放**內部 SRAM**（yves 用 PSRAM，我們想放 SRAM） | 對照 `heap_caps_aligned_alloc(64, size, MALLOC_CAP_DMA\|MALLOC_CAP_INTERNAL)` vs SPIRAM |
| R3 | 雙緩衝下，Python 計算是否來得及（14 條 × 200 顆 ≈ 6 ms 傳輸） | benchmark 腳本，量 `encode_us` / `dma_us` / `blocked_us` |
| R4 | P4 PARLIO 的 `parlio_tx_unit_transmit()` 最小可用範例（IDF 5.5 不靠 FastLED） | 最小 C 測試：1 lane、10 顆，先求會亮再生性能 |
| R5 | P4 `esp_lcd_new_rgb_panel` 在 MicroPython port 的 `esp_lcd` 版本是否含 RGB 面板 | 查 port sdkconfig + 編譯測試 |
| R6 | 與 `machine.bitstream` / `esp32.RMT` 的資源衝突（I80 與 RMT 不衝突，可並用） | 資源分配表 + fallback 策略 |
| R7 | `esp_lcd_panel_io_i80` 的 `tx_color` 是否真非阻塞（yves 用 semaphore 等待，且仍有 `delayMicroseconds(300)`） | 讀 IDF 原始碼 + 實測 |
| R8 | 14 條燈帶時 GPIO 可用性（你 FastLED 用 1,2,3,4,7,8,9,10,11,12,13,16,17,18；**S3 + Octal PSRAM 時 GPIO 33–37 被佔用**） | 依板子列 pin map（`mp_lcd_bus/test_bus.py` 有同樣的警告） |
| R9 | MicroPython GC 與 DMA buffer 生命週期（buffer 必須在傳輸期間存活） | 照抄 `lcd_bus` 的 `ref_bufs[slot]` 機制 + `heap_caps` 非 GC 配置 |

---

## 10. 實作路線（校正版）

| 階段 | 內容 | 產出 |
|---|---|---|
| **P0** | `_led` C module 骨架 + **S3 LCD_CAM I80 後端**（對齊你現在的 FastLED 能力）+ 雙緩衝 + 位元轉置 | 14 條 × 200 顆能跑，能量 fps |
| **P0.5** | benchmark：FastLED 版 vs mpy 版，同一顆燈、同一 pattern | 客觀數字對照表 |
| **P1** | **P4 PARLIO 後端**（Wave8，參考 FastLED 的 ring buffer 做法） | P4 上 16/32 lane |
| **P1.5** | RMT + DMA 後端（少條／無 PSRAM 備援） | 小裝置也能用 |
| **P2** | APA102 SPI+DMA、PCA9685 I2C bulk、LEDC PWM | 四種目標全通 |
| **P2.5** | `led` Python 套件：`init_led` / `LEDCommander` / `LEDController` | 你現有 `main.py` 可無痛移植 |
| **P3** | 效果引擎整合（`LEDMathMethod` 波形 → C `load16`） | 完整閉環 |
| **P4** | 串流/網路整合（與 `mp_Net-Core` 的 `PixelStreamer` 對接） | 上層應用 |

> **為什麼把 I80 放 P0**：你的主場景是 14 條燈帶，RMT 上限 4 條根本用不了。先把能覆蓋你現有能力的後端做出來，才有可比性。

---

## 11. 開發公約（本專案）

- 執行 Python 一律 `python -B`（不留 `__pycache__`）
- 每個 `lib/` 檔尾放 `if __name__ == '__main__':` + `assert` 自檢
- 每個 C 檔檔頭用 `/* */` block comment，寫「目標 / 用法 (Python) / 注意」三段（照 `modrs485_hd.c`）
- config.json 是唯一真源，`_comment` 寫在 JSON 裡
- 每個階段結束跑一次 `tools/bench.py`，把數字記進 `README.md` 的性能表

---

## 12. 需要你決定的問題

### Q1 — C module 的形狀

- **A（推薦）**：全 C driver + 薄 Python 層（`_led` C module + `led` 套件）
- B：純 Python + `neopixel` drop-in（保持相容優先，放棄性能）
- C：全 C（連效果引擎都進 C）

### Q2 — 首發後端

你的 FastLED 設定已經回答一半（14 條燈帶 → RMT 不可能），所以只剩這個選擇：

- **A（推薦）**：**S3 LCD_CAM I80 先做**（對齊你現在的能力），P4 PARLIO 緊接在後
- B：S3 I80 + P4 PARLIO 同時做（兩套一起，工作量大但一次到位）
- C：先做 RMT（只能 4 條，當暖身／小裝置用），主力後端延後
- D：先做純 Python 版把 API 定下來，C 後端之後再補

### Q3 — 規模與細節確認（你已回答，保留記錄）

**✅ 已由你確認**：
1. 每個 slave **14 條燈帶**，最大 200 顆（`fastLED/platformio_local.ini`）
2. **統一像素格式 = 每個 LED 4 bytes `[R,G,B,W]`** → 已寫入 §1.2
3. **S3 的 DMA 緩衝只有內部 SRAM；P4 可用 PSRAM** → 已寫入 §6.0
4. **性能的正確表述**：驅動器職責 = 「不斷讀取 RGBW 緩衝然後非中斷地顯示」；fps 是推導值（`1 / (顆數 × 30 µs)`），不是目標，可彈性調整 → 已寫入 §2.4

**仍待確認**：
1. mpy 版要**取代整個 FastLED firmware**（含 master/slave、storymode、OTA），還是**只做 driver 層**，上層沿用 `mp_Net-Core`？（`API.md` §8 的 A1 會受影響）
2. 14 條燈帶是否**必須嚴格同步**（同一瞬間開始）？I80 天生同步（同一匯流排），PARLIO 也是；未來若拆多個週邊並用才需要額外協調機制。
3. 你實際用的燈珠是 **RGB 還是 RGBW**？（4-byte 格式已定，但 `write` 遮罩的預設值取決於此）
4. 目標是 **16 lane 全開**嗎？還是 14 條夠用？（S3 內部 SRAM 在 16 lane × 666 顆時要 217 KB，需省著用）

### Q4 — 相容性策略

- **A（推薦）**：`_led.ws2812()` 做成 `neopixel` 同形狀（`.buf` / `.write()`），舊程式碼幾乎不改
- B：新 API 與舊 API 並存，config 加 `"driver": "dma" | "legacy"` 切換
- C：完全重寫，不考慮相容

### Q5 — 專案名稱與模組名

- C module：`_led` / `led_driver` / 其他？
- Python 套件：`led` / `led_driver` / `LEDdriver`？
- （我目前用 `_led` + `led`，你可否決）

### Q6 — 是否需要 `mp_Make-Tools` 的 exmod.list 註冊

預設會加 `"/mp_LEDdriver/mp_led/micropython.cmake"`。要放在 `mp_Make-Tools/ext_mod/` 下用相對路徑，還是用絕對路徑指向本 repo？

---

## 附錄 A：實測數字備忘（你既有的）

| 項目 | 數字 | 來源 |
|---|---|---|
| 正弦波計算（查表） | ~16 µs | `mp_LEDController` README |
| HSV→RGB 轉換 | ~3.4 µs/px | `mp_LEDController` README |
| 60 LED WS2812 更新 | ~1800 µs | `mp_LEDController` README |
| scatter 純 Python | 10.37 µs/顆 | `pixel_layout.py` docstring |
| scatter viper | 0.43 µs/顆（24×） | `pixel_layout.py` docstring |
| Pattern 幀處理 | ~20 ms | `mp_LEDController` README |

## 附錄 B：已確認的技術事實（本機與 FastLED 源碼查證）

### B.1 ESP-IDF 5.5（`mp_Make-Tools/lib/esp-idf`）

| 事實 | 位置 |
|---|---|
| RMT TX 支援 `with_dma` | `components/esp_driver_rmt/include/driver/rmt_tx.h:44` |
| S3/P4 `SOC_RMT_SUPPORT_DMA = 1` | `components/soc/{esp32s3,esp32p4}/include/soc/soc_caps.h` |
| **S3/P4 RMT 每組 8 通道、只有 4 可 TX** | `SOC_RMT_TX_CANDIDATES_PER_GROUP = 4` |
| `rmt_new_sync_manager()` 存在 | `rmt_tx.h:161` |
| `rmt_tx_register_event_callbacks()` / `on_trans_done` | `rmt_tx.h:26,143` |
| `rmt_tx_wait_all_done(ch, timeout_ms)`（0 = 非阻塞輪詢） | `rmt_tx.h:126` |
| P4 `SOC_PARLIO_SUPPORTED = 1`、16 lane、支援 bitscrambler decoration | `components/soc/esp32p4/.../soc_caps.h:41,503`；`esp_driver_parlio/include/driver/parlio_bitscrambler.h` |
| S3 I80 bus 寬度 16；P4 I80 寬度 24 | `components/soc/{esp32s3,esp32p4}/.../soc_caps.h` |
| S3/P4 LEDC 8 通道、支援硬體 fade | `SOC_LEDC_CHANNEL_NUM` / `SOC_LEDC_SUPPORT_FADE_STOP` |
| `esp_lcd_new_panel_io_parl`（P4 平行面板 IO，8 lane） | `esp_lcd/include/esp_lcd_io_parl.h` |

### B.2 MicroPython ESP32 port

| 事實 | 位置 |
|---|---|
| `neopixel` 是**純 Python**，走 `machine.bitstream` | `lib/micropython-lib/.../neopixel/neopixel.py` |
| `machine.bitstream` 的 RMT 路徑**不開 DMA**、`mem_block_symbols = 48`、每次 write 建/拆通道 | `ports/esp32/machine_bitstream.c:159ff` |
| 有 bitbang fallback（IRAM、`mp_hal_quiet_timing_enter`） | `ports/esp32/machine_bitstream.c:38ff` |
| `MICROPY_PY_THREAD = 1`（可用 `_thread`） | `ports/esp32/mpconfigport.h:82` |
| `esp32_rmt.c` 存在（`SOC_RMT_SUPPORTED` 時啟用） | `ports/esp32/esp32_rmt.c` |
| MicroPython ref = `c4fa8cb`，ESP-IDF = `v5.5.5` | `mp_Make-Tools/git_config.json` |

### B.3 FastLED 3.10.3（你 pin 的版本）

| 事實 | 位置 |
|---|---|
| 你用的是 **yves I2SClockLess（LCD/I80）driver**，不是 RMT | `src/third_party/yves/I2SClockLessLedDriveresp32s3/src/I2SClockLessLedDriveresp32s3.h` |
| 16 lane、PCLK 2.4 MHz、每 bit 3 clock、0 碼 `0b100` / 1 碼 `0b110` | 同上 |
| 每 LED 每 component 24 bytes（`8 × bpp × 3 × 2`） | `heap_caps_aligned_alloc` 呼叫處 |
| 64-byte 對齊、`MALLOC_CAP_SPIRAM`（PSRAM） | `LCD_DRIVER_PSRAM_DATA_ALIGNMENT` |
| 雙緩衝 `buffers[2]` + 完成回呼 + FreeRTOS semaphore | `show()` / `flush_ready()` |
| `esp_lcd_panel_io_i80_config_t io_config;` **未初始化**（你 patch 的 bug） | 同上，`_initled()` |
| `delayMicroseconds(300)` 的實驗性額外等待 | `FASTLED_EXPERIMENTAL_YVES_EXTRA_WAIT_MICROS` |

### B.4 FastLED 3.10.4-dev（新版 channel driver 架構）

| 事實 | 位置 |
|---|---|
| 新 driver 目錄：`rmt` / `parlio` / `lcd_cam` / `i2s` / `i2s_spi` / `lcd_spi` / `spi` | `src/platforms/esp/32/drivers/` |
| **PARLIO 支援 P4/C6/H2/C5，S3 沒有**（S3 用 LCD） | `drivers/parlio/README.md` |
| PARLIO：1/2/4/8/16 lane、Wave8（每 bit 8 pulse、8 MHz）、每 LED 30 bytes | `parlio_buffer_calc.h` / `README.md` |
| PARLIO 官方聲稱 **8 條 × 1000 顆 > 100 FPS** | `drivers/parlio/README.md` |
| PARLIO DMA 單筆上限 65,535 bytes → 自動分塊 | 同上 |
| PARLIO ring buffer 上限：P4 內部 512 KB / PSRAM 2 MB | `parlio_buffer_calc.h:28,45` |
| P4 RGB LCD 路線：`data_width=16`、`bits_per_pixel=16`、`dma_burst_size=64` | `drivers/lcd_cam/lcd_rgb_peripheral_esp.cpp.hpp:91ff` |
| P4 詳細設計說明（PCLK 3.2 MHz、4 pixel/bit、porch 當 reset gap、可到 24 lane） | `drivers/lcd_cam/implementation_notes.md` |

### B.5 你的 FastLED firmware 配置

| 事實 | 位置 |
|---|---|
| 每 slave **14 條燈帶**（RGB1–4、7–13、16–18） | `fastLED/firmware/shared/src/ledController.cpp:92ff` |
| 最大 200 顆，其餘 10/50/100 顆 | `fastLED/platformio_local.ini` |
| `-D BOARD_HAS_PSRAM`、`-D FASTLED_USES_ESP32S3_I2S` | `fastLED/platformio.ini:16ff` |
| 註解：「待推出 3.10.4 就可以轉用以下 LCD driver 提高效能」 | 同上 |
| `MAX_NUM_SLAVE=20`、I2C 多 slave 架構 | 同上 |
| 主控板 `lolin_s3_mini`、platform-espressif32 55.03.36 | 同上 |
