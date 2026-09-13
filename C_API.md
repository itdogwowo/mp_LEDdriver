# mp_LEDdriver — C 模組 API 設計稿（v0.4）

> **用途**：`mp_led` C user module 的 API 設計。**本專案從一開始就是 C 模組，不在 Python 裡繞。**
> **狀態**：討論稿
> **最後更新**：2026-09-13
> **API 母本**：`mp_lcd_bus`（`DSIBus` 的 page-flip 模型 + `I80Bus` 的 queue 模型）＋ `mp_Net-Core/buffer_hub.py`（三態 SPSC ring）
> **前作**：`DESIGN.md`（硬體/DMA）、`API.md`（早期 Python-first 草案，**已被本文件取代**）

---

## 0. 我從 `mp_lcd_bus` 學到的四件事（你點出來的）

你叫我仔細看 `rgb` / `i80` / `MIPI DSI` 三條 bus 的雙緩衝。我讀完了，**先更正你的判斷，再說我借了什麼**：

| Bus | 內部雙緩衝？ | 實際機制 | 出處 |
|---|---|---|---|
| **DSIBus** | ✅ **真的雙緩衝** | `fb_count=2`（預設）→ `fbs[2]` + `cur_fb` + page-flip | `dsi_bus.c:146,187,241,294,798-846` |
| **I80Bus** | ❌ 不是雙緩衝 | `queue_depth=4` 的 **pending/ref_bufs 槽位環**，每次 `write(buf)` 由**呼叫端**提供 buffer | `i80_bus.c:27-38,152-217` |
| **RGBBus** | ❌ **明確關掉** | `.num_fbs = 0` + `.flags.no_fb = true` → 走 **bounce buffer** 模式 | `rgb_bus.c:133,146` |

**所以真正的「內部雙緩衝」典範是 DSI。**

### 0.1 ⭐ 你問「設計了很多，真正用的不是全部」—— 我查了實際用法

`present()` / `back_buffer()` **有被使用**，但**不是全部場合都用**，而且用法出現了分歧：

| 使用場合 | 用什麼 | 模式 |
|---|---|---|
| `test_dsi.py` 的 `test_double_buffer()` / `test_double_buffer_fps()` | **`frame_buffer(0/1)` + `write(target)`** | 上層**自己輪流**兩塊 fb |
| `bus_adapter.py` `_backbuf_present()` / `show_atomic()` | **`back_buffer()` + `present()`** | C 層決定離屏 |
| `tft_test_tool.py`（benchmark） | **`back_buffer()` + `present()`** | 同上 |

**你觀察到的現象是對的**：DSI 同時暴露了兩套等價機制——
- `frame_buffer(idx)`（明確、上層管輪替）
- `back_buffer()` / `present()`（隱含、C 管輪替）

而 `test_dsi.py` 的雙緩衝測試註解**自己寫出了正確規則**（`test_dsi.py:380-391`）：

> 畫的對象永遠是「目前沒在顯示」的那塊 (fb0/fb1 輪流)。
> 每次都要「整幀」更新，因為切換過去後沒畫到的區域顯示的是那一塊的舊內容。

→ **兩套機制並存的代價是：上層要記得「哪一套在用」**（`bus_adapter.py` 甚至用 `hasattr(bus, 'back_buffer')` 做能力探測）。這是冗餘，不是優點。

**本專案只保留一套**：`back_buffer()` + `present()`（隱含式，照 DSI 的設計意圖），
但**額外提供 `pixel_buffer(idx)`** 給需要明確控制的人（照 `test_dsi.py` 的實際用法）。
兩者共用同一個 `cur_fb`，不會矛盾。

### 0.2 ⭐⭐ 你自己的 benchmark 直接回答了緩衝區問題

`tft_test_tool.py` 裡你自己量了四條路徑（我引用你的註解原文）：

```
#1 write PPA (PSRAM src)     ：45 MB/s（PPA blit）
#2 write PPA (DRAM  src)     ：bounce 代價對照
#3 back_buf[:] = (CPU memcpy)：80 MB/s   ← 比 PPA 快 1.76×
#4 present only (翻頁天花板)  ：≈ 43 FPS（VSYNC 相位）
#5 back_buf + present 端到端  ：~43 FPS
```

而 `bus_adapter.py` 的註解結論更直接：

> 實測 (2026-08): CPU memcpy (80 MB/s) 比 PPA blit (45 MB/s) 快 1.76×，
> 故全頁更新改走 back_buffer 直寫，舍棄較慢的 C write(PPA)

**這三條數字直接決定了 LED driver 的緩衝架構（見 §7）。**

### 0.3 ⭐⭐⭐ 最重要的結論：SRAM 不足有解（見 §6）

你擔心的「SRAM 太少」是真的，但**有兩個關鍵區分讓它變成可解**：

| # | 結論 | 出處 |
|---|---|---|
| 1 | **DMA 編碼緩衝必須在內部 SRAM**（硬約束）；但**像素緩衝可以安全放 PSRAM** | §6.1 |
| 2 | **DMA 緩衝大小與 lane 數無關**（16 lane 共用一條 word 流）→ 多條短燈帶很便宜 | §6.3 |
| 3 | ⭐ **編碼同步發生在 `show()` 內（<1 ms），像素緩衝隨即解鎖** —— 與 20 ms 的傳輸完全解耦 | §7.3 |

**你的場景（14 lane × 200 顆 RGBW）：SRAM 只需約 110 KB** → 在 200 KB 預算內游刃有餘，
還能同時跑 WiFi + LVGL。**滿載（16 × 666）才會吃緊。**

### 0.4 ⭐⭐ 兩個定案（你後續確認）

| # | 定案 | 出處 |
|---|---|---|
| 1 | **切片數由可用 SRAM 反推，不是固定三份**（`slice_bytes = min(32KB, budget//2)`） | §7.8 |
| 2 | ⭐ **統一 LCD 資源 + 單一大中斷管理器**（你授權的妥協）—— 不在傳輸中回呼 Python，ISR 內直接鏈下一個 transfer | §11 |

**第 2 點是整份設計最重要的決定**：它把「傳輸中要回呼 Python 做即時計算」這個**不可能的要求**，
換成「Python 只在 `show()` 進入一次，之後硬體全自動」的**確定性設計**，
並且讓多個 bus **共用一次中斷成本**（「宏觀地用了少了時間」）。

---

## 1. 分層、命名與總線範圍

### 1.1 命名定案

> **你的回覆**：*「這個庫應該叫做 mp_LEDdriver 或者 mp_led_bus」*

| 層 | 名稱 | 理由 |
|---|---|---|
| **repo / 專案** | **`mp_LEDdriver`** | 已存在的目錄名；對齊 `mp_LEDController` 的命名血統；`mp_` 前綴對齊 `mp_lcd_bus` / `mp_heap_caps` / `mp_jpeg` |
| **C user module** | **`led_bus`** | 對齊 `lcd_bus`（你的既有慣例：C module 名 = import 名 = `lcd_bus`） |
| **C 原始碼目錄** | `mp_led_bus/` | 對齊 `mp_lcd_bus/` 的佈局（`mp_` + module 名） |
| **Python 套件** | `led/` | 高階風格層（`LEDCommander` / `LEDController`），**不含編碼邏輯** |

```python
import led_bus                      # C 模組（低階：bus / buf / show）
from led import LEDCommander        # Python 風格層（高階：你現有的世界觀）
```

> **為什麼不用 `import led`**：`led` 這個名字太通用，未來若有第二個 LED 模組會撞名；
> 且 `led_bus` 與 `lcd_bus` 並排時一目了然是同一家族。

### 1.2 檔案佈局

```
mp_LEDdriver/                        ← repo（Git：itdogwowo/mp_LEDdriver）
├── mp_led_bus/                      ← C user module
│   ├── micropython.cmake            ← 照 mp_rs485_hd 模板（idf_component_get_property 拉 include）
│   ├── micropython.mk
│   ├── modled_bus.c                 ← 模組註冊（MP_REGISTER_MODULE(MP_QSTR_led_bus, ...)）
│   ├── led_bus.h                    ← 物件結構 + 後端 enum
│   │
│   ├── led_mgr.c/.h                 ← ⭐ LED 專屬：統一資源 + 單一中斷管理器（§11）
│   ├── led_frame.c                  ← 切片 / 雙層雙緩衝 / 三態槽位環 / show() 接受語意
│   ├── led_slice.c                  ← plan_slices()：由可用 SRAM 反推切片數（§7.8）
│   │
│   ├── led_ws_i80.c                 ← WS2812 on I80：esp_lcd_i80 + 位元轉置編碼
│   ├── led_ws_parlio.c              ← WS2812 on PARLIO（P4）：Wave8 編碼 + ring
│   ├── led_ws_rmt.c                 ← WS2812 on RMT：≤4 條備援
│   │
│   ├── led_apa_spi.c                ← 🆕【正式】APA102：原生 SPI + DMA（spi_device_queue_trans）
│   ├── led_pca_i2c.c                ← 🆕【正式】PCA9685：原生 I2C，一筆 transaction 寫完 16ch
│   │   （PWM LED 不在此列 —— 用 MicroPython 原生 machine.PWM，見 §1.4.1）
│   ├── led_wave.c                   ← 【候補/備援】I80 通用波形引擎（§1.5）
│   │   ├── ws2812_pattern()          →  3 word/bit，16 lane 廣播（＝WS2812 正式路徑）
│   │   ├── apa102_pattern()          →  k=2，CLK lane + MOSI lane（備援）
│   │   └── i2c_pattern()             →  k=2，SCL lane + SDA lane（備援）
│   │
│   ├── led_encode.c                 ← 位元轉置共用邏輯（已逆向驗證，見 §7.2）
│   ├── led_color.c                  ← LUT：gamma / brightness / 通道遮罩
│   ├── led_alloc.c/.h               ← heap_caps 配置策略（pix→PSRAM，dma→SRAM）
│   └── led_cache.h                  ← cache 抽象（S3 = no-op，P4 = msync）
│
├── led/                             ← Python 風格層（薄）
│   ├── __init__.py                  ← init_led / init_rgb / neopixel_compat
│   ├── LEDCommander.py
│   ├── LEDController.py             ← 對應你現有的 PixelController
│   └── LEDMathMethod.py
├── tests/
├── tools/
├── ports_config/                    ← S3 / P4 範例 config.json
├── C_API.md / DESIGN.md / README.md
```

> **`mp_lcd_bus/` 的內部佈局可完全對照**：`modlcd_bus.c` / `esp32_src/i80_bus.c` / `esp32_include/i80_bus.h`。
> 我們多了 `led_mgr` 與 `led_slice` 兩層，因為 LED 需要跨 bus 的中斷統一管理（§11）。

---

### 1.3 ⚠️ 這是「LED 驅動」，不是「顯示驅動」—— 兩者不可混

> **你的指示**：*「請不要將 lcd 和 LED 的驅動混在一起」*
>
> **收到，我上一版 §11.6 的 `led.mgr_claim("lcd_bus")` 是錯的設計，已刪除。**

| | 顯示驅動（`mp_lcd_bus`） | **本專案（LED 驅動）** |
|---|---|---|
| 服務對象 | **面板**（連續掃描、有 VSYNC、要保持畫面） | **燈珠**（離散、無掃描、一次性寫入後自保持） |
| 資料流 | 連續像素流（frame buffer + 掃描時序） | **一次性傳完就結束**（WS2812 靠 reset gap 鎖存） |
| 時間模型 | 連續、週期性（PCLK/DE/HSYNC/VSYNC） | **突發式**（burst，傳完就停） |
| 需要 VSYNC / page-flip | ✅ 是 | ❌ **完全不需要** |
| 需要 framebuffer 掃描 | ✅ 是 | ❌ 否 |

**→ 共用週邊 ≠ 共用驅動。** 即使同一台機器上 I80 螢幕與 I80 LED 都存在，
兩者的**資料模型、時間模型、生命週期都不同**，硬要統一會讓兩邊都變糟。

**修正後的邊界**：
- `led_bus`（本專案）：只管 LED。`led_mgr` 是**LED 專屬**的中斷管理器。
- `lcd_bus`（你的）：只管顯示。**兩者不互相 claim、不互相 import。**
- **同一個 I80 控制器只能給一方用**（`SOC_LCD_I80_BUSES = 1`）。
  - 若同機要「I80 LED + 顯示」→ **顯示改用 RGB LCD 或 SPI**，I80 留給 LED。
  - 若顯示必須用 I80 → **LED 改用 RMT（≤4 條）或 P4 PARLIO**。

---

### 1.4 本驅動的總線清單與角色（**原生週邊為正式方案**）

> **你的定案**：*「這個驅動應該原生要支援實際硬件的 SPI、I2C（APA102、PCA9685），這是剛才 I80 失敗的候選方案。」*
>
> **收到。原生 SPI / I2C 是正式路徑；I80 波形引擎退回「候選/備援」（§1.5）。**

| 目標 | **正式後端（原生週邊）** | 備援 | 需要 led_mgr 中斷管理嗎 | 需要切片/雙緩衝嗎 |
|---|---|---|---|---|
| **WS2812 / SK6812** | **LCD_CAM I80**（S3/P4）／ **PARLIO**（P4） | RMT（≤4 條）、I2S、I80 波形引擎 | ✅ **需要** | ✅ **需要** |
| **APA102 / SK9822** | 🆕 **原生 SPI + DMA** | I80 波形引擎（§1.5） | ❌ **不需要** | ❌ **不需要** |
| **PCA9685** | 🆕 **原生 I2C** | I80 波形引擎（§1.5） | ❌ **不需要** | ❌ **不需要** |
| 一般 PWM LED / 電機 / 伺服 | ✅ **`machine.PWM`（MicroPython 原生）** —— 不需 C 模組 | — | ❌ | ❌ |

**關鍵架構區分（這決定了程式碼的複雜度分佈）：**

```
┌──────────────────────────────────────────────────────────────┐
│ 「難」的一側：WS2812 系列                                      │
│   • 時序零容忍（無 CLK 線，靠時間長度編碼）                     │
│   • 大資料量（多 lane 平行）                                   │
│   → 需要：切片 + 雙層雙緩衝 + led_mgr 統一中斷 + 非同步         │
├──────────────────────────────────────────────────────────────┤
│ 「簡單」的一側：APA102 / PCA9685                               │
│   • 有獨立 CLK/SCL 線 → 傳輸中途有空隙完全無害                  │
│   • 資料量極小（1168 B / 66 B）                                │
│   → 只需要：原生週邊 + 同步 write()。不進 led_mgr、不切片       │
└──────────────────────────────────────────────────────────────┘
```

**為什麼這個切分是對的**：
- WS2812 **沒有原生週邊可用**（RMT 只 4 lane、I2S 更貴）→ 必須自己用 I80/PARLIO 造波形 → 難
- APA102 / PCA9685 **都有原生週邊**（S3: SPI×3、I2C×2；P4: I2C×3）→ 直接用 → 簡單
- **硬要統一（全用 I80）會把簡單的一側也拖進複雜度**，換不到任何好處

**實測資源**（`soc_caps.h`）：
```
S3：SOC_SPI_PERIPH_NUM = 3    SOC_I2C_NUM = 2    SOC_LCD_I80_BUSES = 1
P4：SOC_SPI_PERIPH_NUM = 2    SOC_I2C_NUM = 3    SOC_PARLIO_TX_UNITS = 1(×16 lane)
```

### 1.4.1 ⚠️ PWM LED 用 MicroPython 原生，**不需要 C 模組**

> **你的指正**：*「PWM LED 不是直接 mpy 原生驅動就可以了嗎？為什麼要用 arduino 的驅動？」*
>
> **你是對的，我寫 `led_pwm_ledc.c` 是畫蛇添足。**

**技術事實**：MicroPython ESP32 的 `machine.PWM` **本來就是 ESP-IDF LEDC**：

```c
/* ports/esp32/machine_pwm.c:37 */
#include "driver/ledc.h"
#include "soc/ledc_periph.h"
```

所以用 `machine.PWM` = 用 IDF LEDC，**零中介層**。我的 C 版本只會多做一次參數轉譯。

| 做法 | 中 substance | 評價 |
|---|---|---|
| **`machine.PWM`（原生）** | MicroPython → IDF `ledc_*` | ✅ **採用** |
| 我原本的 `led_pwm_ledc.c` | Python → 我的 C → IDF `ledc_*` | ❌ 多一層，無收益 |

**什麼情況才需要 C 層**（目前都不成立，列為 YAGNI）：

| 情境 | 需要嗎 |
|---|---|
| 8 通道改一次 duty | ❌ 8 次 `pwm.duty_u16()` 足夠（8 通道本來就少） |
| 硬體 fade（呼吸燈零 CPU） | ⚠️ 選配 —— 要用時可加薄函式，或查 MicroPython 是否已暴露 |
| `machine.PWM` 與 WS2812 搶 timer | ❌ LEDC 有 4 個 timer，與 I80 無關 |

**→ 決策：PWM LED / 電機 / 伺服 用 `machine.PWM`，本專案不提供 C 實作。**
`led/` Python 套件可以包一層方便介面（吃 `array('H')` 批次寫），但**底層就是 `machine.PWM`**。

---

### 1.4.2 技術棧優先序（你的原則）

> **你的原則**：*「我的驅動首選是 ESP-IDF，然後是 mpy，真的沒有辦法才會是其他。」*

| 優先序 | 來源 | 本專案的應用 |
|---|---|---|
| **1** | **ESP-IDF 原生驅動** | `esp_lcd` I80（WS2812）· `esp_driver_parlio`（P4）· `esp_driver_spi`（APA102）· `esp_driver_i2c`（PCA9685）· `esp_mm`/`esp_cache` · `esp_heap_caps` |
| **2** | **MicroPython 原生** | `machine.PWM`（LEDC）· `machine.SPI` / `machine.I2C` 物件可傳入 · `micropython.viper` · `heap_caps` 模組（你的） |
| **3** | **自己的 C 模組** | 只有「IDF 沒有現成、MicroPython 也沒有」時才寫：**WS2812 波形編碼 + 切片 + led_mgr** |
| **4** | 其他（Arduino / 第三方 lib） | ❌ **不用**。grep 全專案確保零 Arduino 依賴 |

**程式碼分佈因此很乾淨：**

| 功能 | 來源 | 自己寫的 C 行數 |
|---|---|---|
| WS2812 波形 + 16 lane | IDF `esp_lcd` I80 + 自己的編碼器 | 中 |
| 切片 / 雙層雙緩衝 / led_mgr | 全靠自己 | 中 |
| APA102 | **IDF `esp_driver_spi`**，只寫 frame 組裝 | 小 |
| PCA9685 | **IDF `esp_driver_i2c`**，只寫 65-byte 組裝 | 小 |
| PWM LED | **MicroPython `machine.PWM`** | **0** |

---

### 1.5 【候選/備援】I80 波形引擎 —— 波形分析與成本（**非正式方案**）

> **你的反問**：*「認真地思考一下，因為我不是要求他全功能。你看一看 APA 其實只是單向的，PCA 也不需要雙向。如果他們用了 I80 模仿 WS2812 需要的波，為什麼就不能模擬 APA/PCA 驅動需要的波？」*
>
> **這個反問是對的，我上一版用「協定不同」帶過是偷懶。** 下面是逐項波形分析。
> **結論：邏輯上都可行、電氣上一個可行一個不可行 —— 但兩個都不該做。理由不是「做不到」，是「划不來」。**

#### 1.5.1 先看兩個協定真正需要的波形

```
APA102（SPI，純單向寫）：
        ┌───┐   ┌───┐
  CLK   │   │   │   │        每 bit = 1 個完整 CLK 週期
     ───┘   └───┘   └───
              ╳══════════     MOSI 在 CLK 高電位期間穩定（上升緣取樣）
        ↑ 只有 2 條線：CLK + MOSI。單向。無 ACK。

PCA9685（I2C，寫入）：
  SCL  ─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌──
        └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘
  SDA  ═╗ ╔═╗ ╔═╗ ╔═╗ ╔═╗ ╔═╗ ╔═╗ ╔═       ← 資料
        ╚═╝ ╚═╝ ╚═╝ ╚═╝ ╚═╝ ╚═╝ ╚═╝ ╚═
        └START┘ └─ 8 bit data ─┘ └ACK┘      ← ⚠️ ACK 是「從機拉低 SDA」
```

#### 1.5.2 I80 能給什麼波形

| I80 資源 | 行為 | 可對應 |
|---|---|---|
| **WR 腳** | 每個 word 產生 **1 個 PCLK 寬的脈衝** | ✅ 可當 **CLK / SCL** |
| **16 條 data 線** | 每個 word 在整個週期內輸出該 word 的內容 | ✅ 可當 **MOSI / SDA**（用其中 1 條） |
| **DC 腳** | 強制存在，可拿去當 CS | 中性 |

**所以邏輯上確實可以合成** —— **你的直覺正確**。關鍵是「1 個目標 bit = k 個 I80 word」。

#### 1.5.3 ⭐ 真正的成本：DMA 頻寬放大 **8 倍**

```
I80 的 DMA 每個 PCLK 讀一個 32-bit word    ← 這才是硬體的真實速率
所以「1 個 SPI bit」用 k 個 word = k × 4 bytes 的 DMA 流量
但只換到 1 bit 的真實資料  →  放大 8 倍（無論 k 多少）
```

| SPI 目標速率 | k（word/bit） | PCLK | **DMA 流量需求** | S3 可行？ |
|---|---|---|---|---|
| 2.5 MHz | 4 | 10 MHz | **10 MB/s** | ✅ 輕鬆 |
| 5 MHz | 4 | 20 MHz | **20 MB/s** | ✅ |
| 10 MHz | 4 | 40 MHz | **40 MB/s** | ✅ 可行 |
| 20 MHz | 8 | 160 MHz | **80 MB/s** | ⚠️ 吃掉大半記憶體頻寬 |
| 40 MHz | 8 | 320 MHz | **160 MB/s** | ❌ 不可能 |

#### 1.5.4 ⭐ 你的關鍵論證成立：APA/PCA 不吃高頻 → 成本結構反轉

> **你的反駁**：*「I80 有 16 個 IO，我完全可以用一個來做時鐘信號。至於頻率問題，APA 又不吃高頻率，PCA 更加不吃。」*
>
> **完全正確，而且這推翻了「放大 8 倍所以划不來」的結論。** 我先前是用 WS2812 等級的頻率去算，那是錯的前提。

**① 時鐘線：不需要佔用「額外」lane，WR 或任一 data lane 都能當 CLK**

```c
/* esp_lcd_panel_io_i80.c:457 — CMD 相位可以完全消除 */
uint32_t cmd_cycles = i80_device->lcd_cmd_bits / bus->bus_width;   // lcd_cmd_bits = 0 → 0 cycles
```

**`lcd_cmd_bits = 0` → 完全沒有 CMD 相位、沒有額外時鐘脈衝**（yves driver 就是這樣用的）。
所以 master **精確控制每一個時鐘緣**：

| 訊號 | 用哪條線 | 說明 |
|---|---|---|
| **CLK / SCL** | WR 腳，**或任一 data lane** | 反正每個 word 都產生一個可用的時鐘緣 |
| **MOSI / SDA** | 其餘 data lane 中挑 1 條 | |
| DC | 指派一個「不使用」的腳位即可（`lcd_cmd_bits=0` 時無作用） | |

**② 頻率覆蓋範圍（依你的實測與建議）**

> 你的指示：*「最好是覆蓋得到 APA102 的上限（2 MHz 至 12 MHz 之間），不過我實測一高頻就花，實際上我沒有超過過八。I2C 400 kHz 非常足夠了。」*

**k=2（兩個 word 除頻都設 2 → 完美 50% 時鐘工作週期）：**

| SPI CLK | PCLK | DMA 流量 | 每幀（290 顆） | fps | SRAM 波形 | 判定 |
|---|---|---|---|---|---|---|
| 2 MHz | 4 MHz | 16 MB/s | 4.67 ms | 214 | 36.5 KB | ✅ 規格下限 |
| 5 MHz | 10 MHz | 40 MB/s | 1.87 ms | 535 | 36.5 KB | ✅ |
| **8 MHz** | **16 MHz** | **64 MB/s** | **1.17 ms** | **856** | 36.5 KB | ✅ **你實測上限** |
| 12 MHz | 24 MHz | 96 MB/s | 0.78 ms | 1284 | 36.5 KB | ✅ APA102 規格上限 |
| 16 MHz | 32 MHz | 128 MB/s | 0.58 ms | 1712 | 36.5 KB | ⚠️ 超出你的實測穩定區 |

**→ I80 完整覆蓋 2–12 MHz（含你說的 8 MHz 上限），DMA 流量最高 96 MB/s（占可用 320 MB/s 的 30%）。**

**PCA9685 @ 400 kHz**（你說非常足夠）：

| k | PCLK | 每幀（66 bytes） | fps | SRAM |
|---|---|---|---|---|
| **2** | **800 kHz** | **1.32 ms** | **758** | **2.06 KB** |
| 4 | 1.6 MHz | 1.32 ms | 758 | 4.12 KB |

> **SRAM 36.5 KB 是固定的**（與頻率無關 —— 它取決於 bit 數 × k × 2 bytes）。
> 所以 APA102 不管跑 2 MHz 還是 12 MHz，緩衝都是 36.5 KB。
> **要省就是降 k（但 k=2 已是 SPI 對稱波形的下限）或改用 8-bit word 模式（18.2 KB）。**

**③ I80 頻寬占用：完全不吃緊**

| 用途 | PCLK | DMA 流量 | 占 320 MB/s 的比例 |
|---|---|---|---|
| WS2812 16 lane | 2.4 MHz | 4.8 MB/s | 1.5% |
| APA102 | 5 MHz | 20 MB/s | 6.3% |
| PCA9685 | 800 kHz | 3.2 MB/s | 1.0% |
| **合計** | | **28 MB/s** | **8.8%** ✅ |

**④ SRAM 成本（k=2，16-bit word）**

| 裝置 | 真實 payload | I80 波形緩衝 | 放大 |
|---|---|---|---|
| APA102 290 顆 | 1168 B | **36.5 KB** | 32× |
| PCA9685 1 晶片 | 66 B | **2.1 KB** | 32× |

**→ 36.5 KB 是唯一實質成本**，而且因為這兩個後端**傳完即結束、不像 WS2812 要 ping-pong**，
這塊緩衝是**暫態的、可與其他用途共用**（或直接用 8-bit 模式砍半 → 18.2 KB）。

#### 1.5.5 定位：**候選/備援**，不是正式方案

| 裝置 | 正式後端 | I80 波形引擎的定位 |
|---|---|---|
| **WS2812** | ✅ **I80 / PARLIO**（唯一選擇） | 就是正式方案本身 |
| **APA102** | ✅ **原生 SPI**（§1.4） | ⚠️ **備援** —— 只在「原生 SPI 都用完」時啟用 |
| **PCA9685** | ✅ **原生 I2C**（§1.4） | ⚠️ **備援** —— 只在「原生 I2C 都用完」時啟用 |

**保留 `led_wave.c` 的理由**（不刪，但不當預設）：

| 理由 | 說明 |
|---|---|
| 波形產生器**共用** | `ws2812_pattern()` 一定要有；`apa102_pattern()` / `i2c_pattern()` 只是同一 infra 的額外分支，**增量成本極小** |
| 極端配置的逃生口 | 若某天 SPI×3 + I2C×2 全被佔用，還有路可走 |
| 驗證價值 | 證明了 I80 的通用性（這是有價值的技術結論） |

**波形引擎的程式碼組織（三種 pattern 共用同一 infra）：**

```
led_encode.c（共用）
  └── 波形產生器（把 payload 展成 uint16 word 序列）
        ├── ws2812_pattern()   → 24 bytes/LED（3 word/bit，16 lane 廣播）
        ├── apa102_pattern()   → 4 bytes/bit（k=2，CLK lane + MOSI lane）
        └── i2c_pattern()      → 4 bytes/bit（k=2，SCL lane + SDA lane）
```

**這比「另外接 SPI/I2C 週邊」的三個好處：**

| 好處 | 說明 |
|---|---|
| **單一中斷模型** | 三者共用 `led_mgr` 的同一個完成中斷（§11） |
| **單一緩衝架構** | 三者共用雙緩衝 / 切片 / 三態 ring 的程式碼 |
| **不依賴額外週邊** | SPI/I2C 控制器可留給其他用途（或根本不用） |

**代價與風險（必須明確記錄）：**

| # | 風險 | 說明 | 對策 |
|---|---|---|---|
| **R1** | **無中途錯誤恢復** | DMA 一啟動就線性吐完，無法偵測 I2C NAK / 從機 clock stretching | 傳完後**可選**讀回驗證（佔用 lane）；或用保守時序 |
| **R2** | **APA102 需容忍被注入的時鐘緣** | 每筆 transaction 的邊界可能多出時鐘緣 | `lcd_cmd_bits = 0` + 精確計算 bit 數 → 每個 chunk 的邊界都對齊 byte |
| **R3** | **PCA9685 對畸形 I2C 較敏感** | 位址不匹配時它會忽略並在 NAK 後重置 | 時序以最大容忍度設計；先做低速驗證 |
| **R4** | **APA102 波形 36.5 KB** | 是三者中唯一實質成本 | 用 8-bit 模式（18.2 KB）或共用暫態緩衝 |

> **我上一版的結論（「用硬體 SPI/I2C 更好」）撤回。** 你的論證成立：
> **低頻讓 8× 放大不再重要，而 16 條 lane 剛好夠一個時鐘 + 一個資料。**
> **本專案改用「I80 通用波形引擎」統一驅動 WS2812 / APA102 / PCA9685。**

---

### 1.6 I2S 要不要用？（評估結論：選配，非必要）

**I2S 能驅動 WS2812 嗎？能** —— FastLED 3.10.3 就有 `clockless_i2s_esp32s3`（用 3 個 I2S word 編一個 bit，
與 I80 的 3 word/bit 同構）。**但本專案不需要**：

| 理由 | 說明 |
|---|---|
| I80 已經給 16 lane | S3 的 I80 就是 16 lane，與 I2S 同級 |
| 節省開發成本 | 已有 I80 後端，再做 I2S 是重複工作 |
| RAM 更差 | I2S 的 3 word/bit 要用 **32-bit slot** → 每 bit 12 bytes（I80 只要 6 bytes） |

**→ 列為 P3 選配**，只在「S3 上 I80 被顯示佔用、又需要 >4 條 LED」時才做。

---

## 2. 物件結構（C）

```c
/* led_obj.h */
#define LED_MAX_LANES       16
#define LED_MAX_QUEUE_DEPTH 8
#define LED_DEFAULT_QUEUE   3      /* 見 §6.7：3 塊 DMA 緩衝吸收抖動 */

/* 三態（照抄 buffer_hub）*/
enum { LED_SLOT_IDLE = 0, LED_SLOT_READY = 1, LED_SLOT_SENDING = 2 };

typedef struct {
    mp_obj_base_t base;

    /* ── 硬體後端 ── */
    led_backend_t   backend;          /* I80 / PARLIO */
    esp_lcd_i80_bus_handle_t i80_bus;
    esp_lcd_panel_io_handle_t panel_io;
    parlio_tx_unit_handle_t   parlio;
    int             lane_count;
    int             data_pins[LED_MAX_LANES];
    int             wr_pin, freq_hz;

    /* ── 幾何 ── */
    int             q;                /* 每 lane 顆數 */
    int             order;            /* 位元遮罩：GRBW 等 */
    int             write_mask;       /* 🔥 0b1111 / 0b0111 / 0b1000 */
    int             bytes_per_led;    /* 固定 4（RGBW 統一格式，你定的規格） */

    /* ── 像素緩衝：2 塊，PSRAM（見 §7.6）── */
    uint8_t        *pix[2];           /* .buf / back_buffer() 指向 back */
    size_t          pix_size;         /* q * 4 * lane_count = 42.6 KB @滿載 */
    int             cur_front, cur_back;
    bool            pix_in_psram;

    /* ── DMA 編碼緩衝：N 塊，內部 SRAM（見 §6.6/6.7）── */
    uint16_t       *dma[LED_MAX_QUEUE_DEPTH];
    size_t          dma_size;         /* I80: q * 4 * 24 = 62.4 KB @滿載 */

    /* ── 三態槽位環（照抄 lcd_bus + buffer_hub）── */
    uint8_t         slot_state[LED_MAX_QUEUE_DEPTH];
    mp_obj_t        ref_bufs[LED_MAX_QUEUE_DEPTH];   /* 防 GC */
    int             queue_head, queue_tail, queue_count, queue_depth;

    /* ── 統計 ── */
    uint32_t        frames, dropped;
    uint32_t        encode_us, dma_us, blocked_us;

    bool            initialized;
} mp_led_bus_obj_t;
```

**記憶體配置策略（對應 §6.6）**：

| 緩衝 | caps | 理由 |
|---|---|---|
| `pix[2]` 像素緩衝 | `MALLOC_CAP_SPIRAM`（fallback INTERNAL） | 大、頻寬需求只 2.1 MB/s；省下內部 SRAM |
| `dma[N]` 編碼緩衝 | **`MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL`，64-byte 對齊** | **S3 硬限制**；P4 也優先內部 |

```c
/* led_alloc.h — 照 buffer_hub 的 alloc_dma 精神 */
static void *led_alloc_pix(size_t size) {
    void *p = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_aligned_alloc(64, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}
static void *led_alloc_dma(size_t size) {
    void *p = heap_caps_aligned_alloc(64, size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!p) p = heap_caps_aligned_alloc(64, size, MALLOC_CAP_DMA);  /* 僅 P4 可能成功 */
    return p;
}
```

> **錯誤策略**（照你的三套慣例）：
> - **建構期配置失敗 → raise `MemoryError`**（設定問題，人為可修）
> - **執行期 `present()` 永不 raise** → 佇列滿就丟幀（`dropped++`）或等最舊那筆
>   （照 `buffer_hub` 的「滿了回 `False`，不 raise」）

---

## 3. Python API

### 3.1 建構子（照 `DSIBus` 的慣例：必填位置參數 + 選填 kwarg）

```python
import led

bus = led.WS2812Bus(
    pins,                   # 必填：int 或 tuple（1–16 條）
    q,                      # 必填：每條顆數（int 或 tuple）
    order="GRBW",           # 硬體輸出順序
    backend="auto",         # "auto" | "i80" | "parlio"
    wr=-1,                  # i80 的 WR 腳（-1 = 自動挑）
    write=0b1111,           # 通道遮罩：0b1111 / 0b0111 / 0b1000
    queue_depth=2,          # 佇列深度
    neutral=0,              # dStay
    gamma=None,             # None | float | 256-byte LUT
    brightness=4095,        # 全域亮度 0–4095
    overclock=1.0,          # 協定時鐘倍率
    timing=None,            # None | "ws2811" | (T0H,T0L,T1H,T1L) ns
)
```

與 `lcd_bus` 的對照（**同一套文法**）：

| `lcd_bus.DSIBus` | `led.WS2812Bus` |
|---|---|
| `lanes, width, height, lane_bit_rate_mbps`（必填位置） | `pins, q`（必填位置） |
| `fb_count=2` | `pixel_buffers=2` |
| `queue_depth=4`（DMA 交易佇列） | `queue_depth=3`（**DMA 緩衝塊數**，見 §6.7） |
| `use_dma2d=True` | `backend="auto"` |
| `rst=-1`, `virtual_channel=0`（`-1` = 不管理） | `wr=-1`（`-1` = 不管理／自動） |

### 3.2 方法表

| 方法 | 回傳 | 說明 | 對應 `lcd_bus` |
|---|---|---|---|
| **`bus.buf`** | `memoryview` | 🔥 **可直接寫的像素緩衝**（`[lane][q][4]`，stride 固定 4 bytes） | — |
| `bus.pixel_buffer(idx=None)` | `memoryview` | 明確取第 `idx` 塊（0/1）；`None` = 目前離屏那塊 | `DSIBus.frame_buffer(i)` |
| `bus.back_buffer()` | `memoryview` | `pixel_buffer(None)` 的別名（照 DSI 命名） | `DSIBus.back_buffer()` |
| `bus.view(lane)` | `memoryview` | 單條燈帶的 view（給 `PixelController`） | — |
| **`bus.show()`** | `int` (`tid`) | 🔥 **編碼 + 入隊 + 非同步返回** | `I80Bus.write()` |
| `bus.present()` | `int` (`tid`) | `show()` 的別名（照 DSI 命名） | `DSIBus.present()` |
| `bus.write_mask` | property | 執行期切換通道遮罩 | — |
| `bus.brightness` | property | 全域亮度 0–4095 | — |
| `bus.set_neutral(v)` | `None` | dStay | — |
| `bus.clear()` | `None` | 填中性值到緩衝 | `PixelStreamer.clear_all()` |
| `bus.is_busy()` | `bool` | `queue_count > 0` | `I80Bus.is_busy()` |
| `bus.pending()` | `int` | 佇列未完成筆數 | `I80Bus.pending()` |
| `bus.wait(tid, timeout_ms=-1)` | `bool` | 等特定筆 | `I80Bus.wait()` |
| `bus.wait_all(timeout_ms=-1)` | `None` | 等全部 | `I80Bus.wait_all()` |
| `bus.lane_count()` | `int` | lane 數 | `I80Bus.lane_count()` |
| `bus.info()` | `dict` | `{backend, lanes, q, pix_bufs, pix_in_psram, dma_blocks, dma_bytes, sram_free}` | 🆕 |
| `bus.stats()` | `dict` | `{frames, dropped, encode_us, dma_us, blocked_us, leds_per_sec}` | — |
| `bus.deinit()` | `None` | 釋放（含 `__del__`） | `I80Bus.deinit()` |

> **命名取捨**：你 `test_dsi.py` 用 `frame_buffer(0/1)` + `write(target)`，
> `bus_adapter.py` 用 `back_buffer()` + `present()`。**兩套並存是冗餘**，所以本設計收斂成：
> - **主要動詞 `bus.buf` + `bus.show()`**（短，符合 `mp_Net-Core` 的 `show()` 慣例）
> - **`pixel_buffer()` / `back_buffer()` / `present()` 作為別名**（讓 DSI 的直覺可搬）
> - 全部共用同一個 `cur_front`，不會矛盾

### 3.3 用法：三種風格，隨你挑

**風格 A — 最簡**

```python
import led

bus = led.WS2812Bus(pins=(1,2,3,4,7,8,9,10,11,12,13,16,17,18),
                    q=(200,10,10,10,10,100,50,50,10,10,10,10,10,10),
                    order="GRBW", write=0b1111)

while True:
    buf = bus.buf                      # 離屏那塊，零拷貝
    buf[0:4] = b"\xff\x00\x00\x00"  # 第 0 條第 0 顆 = R
    bus.show()                         # 編碼 + 入隊，立即返回
```

**風格 B — 明確持有兩塊（照你 `test_dsi.py` 的用法）**

```python
bufs = (bus.pixel_buffer(0), bus.pixel_buffer(1))
n = 0
while True:
    target = bufs[n & 1]               # 永遠寫「目前沒在顯示」的那塊
    fill_effect(target, n)             # 效果直接寫進去，零拷貝
    bus.show()
    n += 1
```

**風格 C — 與現有 `PixelStreamer` 接軌（最省）**

```python
class PixelStreamer:
    def __init__(self, bus, controllers):
        self.bus, self.controllers = bus, controllers

    def show_all(self):
        buf = self.bus.buf             # 取離屏那塊（零拷貝）
        for c in self.controllers:
            c.load_into(buf)           # 各 controller 寫自己 lane 的區段
        self.bus.show()                # 🔥 一次送出全部 lane

    def clear_all(self):
        # 停止/熄燈：填中性值（燈=0，motor=0x80 死區停 — 見你的教訓）
        self.bus.clear()
        self.bus.show()
```

**這就實現了「不斷讀取 RGBW 緩衝然後顯示」**：
- 編碼與顯示都在 C，Python 只寫像素 + 呼叫 `show()`
- `show()` 只做「編碼 + 入隊」（C 版 < 1 ms），**不等 DMA 送完**
- 顯示在背景 DMA 接力，之間零 Python 介入

---

### 3.4 APA102 — 原生 SPI（正式後端）

#### 建構子

```python
apa = led_bus.APA102(
    spi,                    # 必填：machine.SPI 物件，或 int（bus id，由 C 建立）
    q,                      # 必填：顆數（int）
    *,
    cs=-1,                  # CS 腳；-1 = 不管理
    mosi=-1, sck=-1,        # spi 給 int 時用（由 C 建 bus）
    freq=8_000_000,         # 🔥 SPI 時鐘（2–12 MHz；你實測 8 MHz 穩定）
    order="BGRW",           # 依燈珠（APA102 常見 BGR / BGRW）
    write=0b1111,           # 通道遮罩（4-byte 統一格式）
    double_buffer=True,     # DMA 用；耗 RAM 極少
    brightness=4095,        # 全域亮度 0–4095（APA102 有 5-bit 亮度頭）
    gamma=None,
    neutral=0,
)
```

#### 方法表

| 方法 | 回傳 | 說明 |
|---|---|---|
| **`bus.buf`** | `memoryview` | 像素緩衝，**每 LED 4 bytes `[R,G,B,W]`**（與 WS2812 同格式） |
| `bus.show()` | `int` (`tid`) | 組 frame（起始 4B + 4B/px + 結束 4B+）→ **非同步 SPI 送出** |
| `bus.wait(tid)` / `wait_all()` | | 與 WS2812 同契約 |
| `bus.is_busy()` / `pending()` | | 同上 |
| `bus.write_mask` / `brightness` / `set_neutral()` / `clear()` | | 同上（**共用同一個 Python 介面**） |
| `bus.info()` | `dict` | `{backend:'spi', freq, q, frame_bytes, sram_used}` |

#### SPI 後端的實作要點

```c
/* led_apa_spi.c */
/* frame 佈局： [0x00 x4] + [0xE0|bri5, B, G, R] x q + [0xFF x ((q+15)/16)] */

/* 1. 一次組好整幀到 DMA 緩衝（起始 + 像素 + 結束） */
/* 2. 一次 spi_device_queue_trans 送出（非同步，完成回呼） */
/* 3. CS 全程保持（APA102 靠 clock 分幀，不需要 CS 翻轉） */
```

| 要點 | 說明 |
|---|---|
| **資料量極小** | 290 顆 = 4 + 1160 + 4 = **1168 bytes**（對比 I80 波形 36.5 KB） |
| **不需要切片** | 遠低於 SPI 單筆上限 |
| **不需要 led_mgr** | 有 CLK 線 → 空隙無害；用 SPI 自己的完成回呼即可 |
| **RAM** | 1168 B × 2（雙緩衝）= **2.3 KB**（可忽略） |
| **速度** | @8 MHz → **1.17 ms/幀** |
| **亮度頭** | APA102 內建 5-bit 全域亮度（`0xE0 \| (bri>>3)`），可省 CPU |

#### 錯誤處理

| 情境 | 策略 |
|---|---|
| `spi` 是 int 但 `mosi`/`sck` 沒給 | **raise `ValueError`**（設定問題） |
| `freq` 超出 1–20 MHz | **收集 warning、不 raise**，用 clamp 值 |
| 執行期 queue 滿 | 回 `None`（與 WS2812 同契約，**不 raise**） |

---

### 3.5 PCA9685 — 原生 I2C（正式後端）

#### 建構子

```python
pca = led_bus.PCA9685(
    i2c,                    # 必填：machine.I2C 物件，或 int（bus id，由 C 建立）
    *,
    scl=-1, sda=-1,         # i2c 給 int 時用
    address=0x40,           # 0x40–0x7F；或 0x70（ALLCALL 廣播）
    channels=16,            # 每晶片 16 通道
    count=1,                # 同一位址下的晶片數（多晶片 = 多個 bus 實例）
    freq=1000,              # PWM 頻率（Hz，PCA9685 範圍 24–1526）
    i2c_freq=400_000,       # 🔥 I2C 時鐘（你說 400 kHz 非常足夠）
    write=0b1000,           # 預設只寫 W 通道（單色）
    order="W",
    neutral=0,              # dStay（motor 用 0x80 死區停）
    brightness=4095,
)
```

#### 方法表

| 方法 | 回傳 | 說明 |
|---|---|---|
| **`bus.buf`** | `memoryview` | **每通道 4 bytes**（與像素統一格式一致，只用 W） |
| `bus.show()` | `int` (`tid`) | 組 68-byte 幀 → **一筆 I2C transaction 寫完 16 通道** |
| `bus.set_pwm_freq(hz)` | `None` | 改 PWM 頻率 |
| **`bus.sleep(bool)`** | `None` | 省電（PCA9685 MODE1 SLEEP） |
| `bus.allcall(bool)` | `None` | 開啟 ALLCALL（你的 0x70 廣播用法） |
| `bus.info()` | `dict` | `{backend:'i2c', address, channels, freq, frame_bytes}` |

#### I2C 後端的實作要點

```c
/* led_pca_i2c.c */
/* frame = [reg=0x06 (LED0_ON_L)] + 64 bytes（16ch × 4 bytes，ON_L/ON_H/OFF_L/OFF_H）
   → auto-increment，一筆寫完 16 通道 */

uint8_t frame[1 + 64];
frame[0] = 0x06;
for (ch = 0; ch < 16; ch++) {
    uint16_t duty = ...;                       /* 12-bit */
    frame[1 + ch*4 + 0] = 0;                   /* ON_L  */
    frame[1 + ch*4 + 1] = 0;                   /* ON_H  */
    frame[1 + ch*4 + 2] = duty & 0xFF;         /* OFF_L */
    frame[1 + ch*4 + 3] = duty >> 8;           /* OFF_H */
}
i2c_master_transmit(dev, frame, sizeof(frame), timeout_ms);
```

| 要點 | 說明 |
|---|---|
| **資料量極小** | **65 bytes**（1 reg + 64） |
| **12-bit 直寫** | `duty = (v12 * 4095) / 4095` → **直通，不掉精度**（對比你舊版的 `(w<<4)\|(w>>4)`） |
| **不需要 led_mgr** | 有 SCL 線 → 空隙無害 |
| **廣播** | 你已用 `0x70` ALLCALL —— 保留，而且**一塊板子或多塊板子都只有 1 筆 transaction** |
| **速度** | @400 kHz → 65 B ≈ **1.3 ms**（16 晶片 ≈ 21 ms，此時才需要多條 I2C bus） |
| **錯誤處理** | I2C 有 **ACK**，所以**能真正偵測失敗**（比 I80 波形引擎強）→ 回 `False` 不 raise |

#### 與你現有 `pca9685_drv.py` 的對應

| 你現有的做法 | 本驅動 |
|---|---|
| `pca.buffer = LED_Buffer` + `sync_buffer()` | `bus.buf` + `bus.show()` |
| `(w << 4) \| (w >> 4)` 8-bit 擴展 | **12-bit 直寫**（不掉精度） |
| 逐通道 `write_reg` | **一筆 65-byte auto-increment** |
| `address="0xFF"` 自動掃描 | 保留（掃描後建 0x70 廣播 controller） |
| 中性值（motor `0x80`） | `neutral=` 參數，`clear()` 用它 |

---

## 4. 幀生命週期（present 內部）

```
present() 在 C 裡做的事（全部在 C，Python 不介入）：

  1. 佇列滿？ → 不 raise。看 queue_depth：
       queue_depth >= 2 → 等最舊那筆完成（只等「剛好夠」），blocked_us += 等待時間
       queue_depth == 1 → 直接丟棄本幀，dropped++，return None
  2. 編碼：把 pix[cur_back] 依 write_mask 查 LUT → 位元轉置 → dma[cur_back]
       （同時做 brightness / gamma）
  3. cache 維護（僅 P4 + PSRAM，見 §5）
  4. 翻頁：cur_front <-> cur_back 原子交換
  5. tx_color(dma[cur_front], dma_size)  ← 非同步，入 queue
  6. return tid
```

**關鍵：`present()` 不需要 em 呼叫端提供 buffer，也不需要呼叫端指定 index**（照 DSI 的設計意圖）。

### 4.1 為什麼這樣就「不中斷」

| 需求 | 機制 |
|---|---|
| 不關中斷 | DMA 硬體搬位元，CPU 中斷全開 |
| 不擋 Python | 雙緩衝；`present()` 只做編碼 + 入隊 |
| 顯示不斷 | 佇列有下一筆待送 → DMA 無空隙接力 |

### 4.2 `queue_depth` 的抉擇

| `queue_depth` | 行為 | 記憶體 | 適用 |
|---|---|---|---|
| **1** | `present()` 永不等待，來不及就丟幀（`dropped++`） | 最省（單 DMA buffer） | 顯示優先 |
| **2（預設）** | 可容忍一幀的抖動，不丟幀 | 2× DMA buffer | 通用 |
| 3–4 | 更能吸收抖動，但顯示延遲增加 | 3–4× | 效果計算抖動大時 |

> 註：**像素雙緩衝永遠是 2**（否則會撕裂，見 §6）。`queue_depth` 只控制 **DMA 編碼緩衝**的數量。

---

## 5. cache 維護（S3 vs P4 的關鍵差異）

| 晶片 | 內部 SRAM 是否經 cache | DMA 讀取 | 需要 msync？ |
|---|---|---|---|
| **ESP32-S3** | ❌ **不經 cache**（`SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE` **未定義**） | 直讀實體記憶體 | ✅ **不需要** |
| **ESP32-P4** | ✅ **經 L1 cache**（`SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE = 1`） | 直讀實體記憶體 | ⚠️ **需要** |

```c
/* led_cache.h — 唯一做 cache 維護的地方 */
static inline void led_cache_flush(const void *addr, size_t size)
{
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
    /* P4：CPU 寫入走 write-back L1，DMA 直讀實體 → 必須 C2M（且只做一次！） */
    esp_cache_msync((void *)addr, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#else
    /* S3：內部 SRAM 不經 cache → 零成本 */
    (void)addr; (void)size;
#endif
}
```

> **⚠️ 你踩過的坑直接適用**（`dsi_bus.c:814-822`）：
> **`led_cache_flush()` 只能在編碼完成後呼叫一次。** 若在 `present()` 與 `tx_color` 路徑各做一次 =
> 每幀重複寫回整個 DMA buffer；在 P4 + PSRAM 大 buffer 時會讓每幀工作量超過幀週期，**直接砍半 fps**。
>
> **而 S3 選內部 SRAM 讓這個問題直接消失**（你選對了）—— 這是 mpy 版能比 FastLED 版（用 PSRAM）更穩的技術根據。
>
> **補充**：像素緩衝若放 PSRAM（§6.6 建議），S3 的 PSRAM 是走 cache 的，所以**像素側**要另外考慮一致性——
> 但因為編碼是 CPU 讀、應用也是 CPU 寫（同一顆 core 的 cache），**同 core 下自動一致，不需要 msync**。
> 只有「跨 core 生產 + DMA 直接讀像素」才需要（本設計的 DMA 讀的是編碼結果，不是像素）。
>
> **⑨ IDF 對 PSRAM 來源的自動處理**：`esp_lcd_panel_io_i80.c:584` 的 `tx_color` **無條件呼叫**
> `esp_cache_msync(..., C2M | UNALIGNED)`。所以即使 DMA 來源是 PSRAM，IDF 也會幫你做 cache 寫回。
> **但這正是你 DSI 踩過的坑**：它在**每次交易**都做一次。若 DMA 緩衝在內部 SRAM，這個 msync 是 no-op（S3 內部不經 cache）；
> 若在 PSRAM，就是每幀一次的額外寫回成本。**再次支持「DMA 緩衝放內部 SRAM」。**

---

## 6. ⭐ SRAM 預算：本專案最硬的約束（你提出的核心顧慮）

> 你的顧慮：*「S3 上面 PSRAM 與 Flash 有競爭問題，他會打斷數據傳輸，所以 DMA 似乎是必須要放在 SRAM，因此設計一個正確的數據流就變得尤其緊要了，但問題就是 SRAM 太少了」*
>
> **你是對的，而且我查證了機制。** 本節把預算與對策算清楚。

### 6.1 你的判斷在技術上完全成立

**① I80 的 DMA 其實「可以」讀 PSRAM —— 但你不該用**

IDF 有現成的 API（`components/esp_lcd/i80/esp_lcd_panel_io_i80.c:713`）：

```c
void *esp_lcd_i80_alloc_draw_buffer(esp_lcd_panel_io_handle_t io, size_t size, uint32_t caps)
{
    if (caps & MALLOC_CAP_SPIRAM)   // ← 允許 PSRAM
        buf = heap_caps_aligned_calloc(bus->ext_mem_align, 1, size,
                                       MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    else
        buf = heap_caps_aligned_calloc(bus->int_mem_align, 1, size,
                                       MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    return buf;
}
```

而且 `tx_color` 會自動做 cache 維護（`esp_lcd_panel_io_i80.c:584`）：

```c
esp_cache_msync((void *)color, color_size,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
```

**所以「能不能」不是問題，「該不該」才是。** 你的理由是對的：

| 面向 | 用 PSRAM 當 DMA 來源 | 用內部 SRAM |
|---|---|---|
| WS2812 時序容錯 | ❌ **零容忍**（無時鐘線，靠時間長度編碼） | ✅ |
| PSRAM 與 Flash 共用 SPI bus | ❌ SPI0 被佔用時（NVS 寫入、OTA）DMA 直接餓死 | ✅ 內部 SRAM 走另一條路徑 |
| MicroPython 本身 | ❌ GC 掃描、`os.stat()`、任何 flash 操作都會踩到 | ✅ |
| cache 維護成本 | 每幀 C2M 寫回（你 DSI 踩過的坑） | ✅ **S3 內部 SRAM 不經 cache，零成本** |
| 資料正確性 | ⚠️ DMA 讀 PSRAM 而 CPU 寫經 cache → 需 msync | ✅ 無一致性問題 |

> **→ 結論：I80 的 DMA 緩衝必須在內部 SRAM，沒有妥協空間。這是硬約束。**

**② 但「像素緩衝」可以安全放 PSRAM** —— 這個區分是解開 SRAM 不足的鑰匙

| 緩衝 | 誰讀 | PSRAM 可接受嗎 |
|---|---|---|
| **DMA 編碼緩衝** | **硬體 DMA**（非同步、時序零容忍） | ❌ **絕對不行** |
| **像素緩衝** | **CPU 編碼器**（同步、時序無關） | ✅ **可以**。PSRAM 卡頓只讓編碼變慢，**不會產生時序錯誤** |

**③ 你的建置剛好幫了忙**（我查了 `build-ESP32_GENERIC_S3-SPIRAM_OCT/sdkconfig`）：

```
# CONFIG_SPIRAM_FETCH_INSTRUCTIONS is not set    ← 程式碼不從 PSRAM 抓
# CONFIG_SPIRAM_RODATA is not set                ← 唯讀資料也不從 PSRAM 抓
CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE=0x4000     ← 16 KB
CONFIG_ESP32S3_DATA_CACHE_SIZE=0x8000            ← 32 KB
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768      ← 32 KB 保留給內部
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192         ← ≤8 KB 配置一律進內部
```

→ **指令從 flash cache 取、PSRAM 只放資料** = CPU 執行不與 PSRAM 讀取搶 bus。
對「編碼要快」這件事是有利的。

### 6.2 SRAM 預算：你實際有多少？

ESP32-S3 內部 SRAM **512 KB**，但實際可用的少得多：

| 項目 | 大小 | 備註 |
|---|---|---|
| 總內部 SRAM | 512 KB | SRAM0–2 = 416 KB，SRAM3(DRAM) = 64 KB… 實務以連續可用塊計 |
| 減：I/D cache | −48 KB | 16 KB I + 32 KB D |
| 減：ROM / bootloader / IDF | −50~80 KB | |
| 減：MicroPython 初始 heap | −**64 KB** | `MICROPY_GC_INITIAL_HEAP_SIZE`（有 `MICROPY_GC_SPLIT_HEAP_AUTO`，會動態長） |
| 減：WiFi / LWIP | −40~60 KB | **只在啟用 WiFi 時** |
| 減：LVGL | 依 buffer 大小 | 若同機跑 |
| **粗估可用（無 WiFi）** | **約 250–320 KB** | |
| **粗估可用（有 WiFi）** | **約 180–250 KB** | |

> ⚠️ **這是估算，不是實測。** 開工時第一件事就是用 `heap_caps.get_largest_free_block(CAP_DMA|CAP_INTERNAL)` 量出**真實**數字。

### 6.3 SRAM 需求 = DMA 緩衝 + LUT

**DMA 緩衝**（每塊，公式 `q × bpp × 24 bytes`）：

| 每 lane 顆數 | RGB (3ch) | RGBW (4ch) | 8-lane 8-bit RGB（省一半） |
|---|---|---|---|
| 100 | 7.0 KB | 9.4 KB | 3.5 KB |
| 200 | 14.1 KB | 18.8 KB | 7.0 KB |
| 310 | 21.8 KB | 29.1 KB | 10.9 KB |
| 400 | 28.1 KB | 37.5 KB | 14.1 KB |
| 666 | 46.8 KB | 62.4 KB | 23.4 KB |
| 1000 | 70.3 KB | 93.8 KB | 35.2 KB |

> **⭐ 關鍵**：**與 lane 數無關**（16 lane 共用同一條 word 流）。所以「多條短燈帶」很便宜。

**LUT**（可選，但強烈建議）：

| 模式 | 大小 | 說明 |
|---|---|---|
| **無 LUT**（旗標展開 + popcount） | **0** | 最省 SRAM，編碼慢 ~2–3× |
| **8-bit LUT** | **24.0 KB** | 4 通道 × 256 值 × 24 bytes。**推薦** |
| 16-bit LUT | 48.0 KB | 4 通道 × 256 值 × 24 word × 2 bytes |

> **🔧 修正我上一版**：我原本寫 LUT 要 48 KB。實際上 **DMA 走 8-bit 模式時 LUT 也是 8-bit → 24 KB**（省一半）。
> 代價：8-bit 並列匯流排只能推 **8 lane**（需要 16 lane 就必須用 16-bit → 48 KB LUT）。

### 6.4 SRAM 總表：哪些配置放得下

| 配置 | LUT | DMA×2 | DMA×3 | **SRAM 合計** | 判定（可用 200 KB） |
|---|---|---|---|---|---|
| **14 lane × 200 顆 RGBW**（你的實際場景） | 48 KB | 37.5 KB | — | **85.5 KB** | ✅✅ 輕鬆 |
| 16 lane × 200 顆 RGBW | 48 KB | 37.5 KB | 56.2 KB | 85.5 / 104 KB | ✅✅ 輕鬆 |
| 16 lane × 310 顆 RGBW | 48 KB | 58.1 KB | 87.2 KB | 106 / 135 KB | ✅ |
| 16 lane × 400 顆 RGBW | 48 KB | 75.0 KB | 112.5 KB | 123 / 161 KB | ✅ 有 WiFi 就吃緊 |
| 16 lane × 500 顆 RGBW | 48 KB | 93.8 KB | 140.6 KB | 142 / 189 KB | ⚠️ 需降級 |
| **16 lane × 666 顆 RGBW** | 48 KB | 124.9 KB | 187.3 KB | **173 / 235 KB** | ❌ **放不下（有 WiFi）** |

**→ 你的實際場景（14 × 200）只需要 85 KB，完全沒問題。**
**滿載（16 × 666）才需要降級。**

### 6.5 ⭐ 省 SRAM 的五個槓桿（按效益排序）

| # | 槓桿 | 節省 | 代價 |
|---|---|---|---|
| **1** | **`queue_depth`（切片數）由 SRAM 反推** | 用滿可用 SRAM，不浪費 | 見 §7.8 演算法 |
| **2** | **`write` 遮罩改 RGB（3ch）** | DMA + LUT 省 25% | 無（若燈珠本來就是 RGB） |
| **3** | **不用 LUT**（旗標展開 + popcount） | **省 24~48 KB** | 編碼慢 2–3×（C 版仍可能 < 2 ms） |
| **4** | **降到 8 lane / 8-bit 模式** | DMA 減半、LUT 減半 | **lane 數上限變 8** |
| **5** | **`pixel_buffers` 用 PSRAM** | 不佔 SRAM（本來就是 PSRAM） | 無 |

### 6.6 為什麼「2 塊切片」是下限（不是「重複送幀」）

> **🔧 收回我先前的說法**：我曾在這裡主張「WS2812 是保持型裝置，重複送同一幀合法，
> 所以 1 塊緩衝也夠」。**那個說法是錯的**，正確理由見 **§7.3**：
> 重複送幀會讓像素緩衝被鎖定 `N × 20 ms`，與小切片互斥。
>
> **真正讓 2 塊就夠的理由**是：**編碼同步發生在 `show()` 內（< 1 ms），像素緩衝隨即解鎖**，
> 與 20 ms 的 DMA 傳輸完全解耦。切片只需 ping-pong 就能零空隙接力。

**切片數的計算規則見 §7.8**（由可用 SRAM 反推，非固定三份）。

### 6.7 完整的資料流設計圖

```
┌───────────────────────────────────────────────────────────────────┐
│ ① 應用/效果層（Python 或 viper）                                    │
│    寫入 pix[back]  ← **PSRAM**（42.6 KB/塊，頻寬只用了 2.6%）        │
└────────────────────────────┬──────────────────────────────────────┘
                             │ show()
                             ▼
┌───────────────────────────────────────────────────────────────────┐
│ ② 編碼器（C，IRAM 執行，LUT 放 SRAM）                               │
│    for each pixel, channel:  LUT[ch][val] → 位元轉置 → dma[free]    │
│    輸出 8-bit 或 16-bit word 流 → **內部 SRAM**（S3 內部不經 cache）│
└────────────────────────────┬──────────────────────────────────────┘
                             │ tx_color() 非同步
                             ▼
┌───────────────────────────────────────────────────────────────────┐
│ ③ I80 DMA（硬體，背景）→ 16 條燈帶同時吐                            │
│    dma[0] ⇄ dma[1] 兩塊輪替；沒有新幀就重送舊幀 → 顯示永不中斷        │
└───────────────────────────────────────────────────────────────────┘

SRAM 佔用 = LUT(24~48 KB) + DMA×2(≤125 KB)   ← 與 lane 數無關
PSRAM 佔用 = 像素緩衝 2 塊（42.6 KB × 2 = 85 KB，PSRAM 是 MB 級，無壓力）
```

### 6.8 給 S3 的最終建議配置

| 參數 | 值 | 理由 |
|---|---|---|
| `queue_depth` | **2（1 片時）／由 §7.8 反推（多片時）** | 2 塊即可 ping-pong 零空隙；更多塊吸收抖動 |
| `write` | 依燈珠（RGB → `0b0111`） | 省 25% |
| `lut` | **`True`（8-bit，24 KB）** | 編碼快 2–3×，值得那 24 KB |
| `pixel_buffers` | **2（PSRAM）** | 不佔 SRAM |
| DMA 記憶體 | **`MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL`** | **硬約束** |
| 像素記憶體 | `MALLOC_CAP_SPIRAM`（fallback INTERNAL） | 省 SRAM |

**你的場景（14 × 200 RGBW）SRAM 合計 ≈ 48 + 24 + 37.5 = 110 KB** → 在 200 KB 預算內游刃有餘，
**還能同時跑 WiFi + LVGL**。

---

## 7. 切片與雙層雙緩衝：確認你的架構描述（本設計的定案模型）

> 你的描述：*「像素緩衝需要雙緩衝，SRAM 硬件時序緩衝也要雙緩衝，兩個雙緩衝。首先是像素緩衝的背景緩衝不斷被 SRAM 緩衝讀取，一個 SRAM 緩衝背後發射，另一個 SRAM 緩衝再更新新的區域。假設一個大緩衝需要被拆分成三份，SRAM 就是利用這種交換來完整讀取這三份。然後上面層的像素雙緩衝就讓用戶好像 DSI 的 API 操作 —— 能寫入就告訴用戶已經寫入了，不能寫入就告訴用戶沒有寫進去，這樣也方便用戶計算 FPS，並且控制像素緩衝的下一幀。」*
>
> **✅ 你的模型是對的，比我自己上一版寫的更精確。** 下面是逐條確認，以及**一個必須修正的隱含錯誤**（那是我上一版留下的）。

### 7.1 逐條確認你的描述

| 你的說法 | 判定 | 補充 |
|---|---|---|
| 像素緩衝雙緩衝 | ✅ 對 | 在 **PSRAM**（85 KB，不佔 SRAM） |
| SRAM 時序緩衝也要雙緩衝 | ✅ 對 | 在 **內部 SRAM**（硬約束） |
| 兩個雙緩衝 | ✅ 對 | 兩層，各自獨立輪替 |
| 一個 SRAM 緩衝發射、另一個更新新區域 | ✅ 對 | 這正是 ping-pong |
| 大緩衝拆成三份，靠交換完整讀取 | ✅ **對，而且這正是既定設計** | 見下方修正 ① |
| 上層像素雙緩衝，像 DSI 的 API | ✅ 對 | 但**只有像素側**能這樣做，見修正 ② |
| 能寫入 → 告知成功；不能 → 告知失敗 | ✅ 對 | `show()` 回 `tid` 或 `None` |
| 方便用戶計算 FPS、控制下一幀 | ✅ 對 | `show()` 的接受率就是應用側真實 FPS |

### 7.2 修正 ①：切片有三種切法，我們用的是「空間切片」

「大緩衝拆成三份」有兩種可能，必須選對：

| 切法 | 做法 | 能用嗎 |
|---|---|---|
| ❌ **時間切片** | 同一塊區域重複送，逐次更新不同區域 | 畫面會出現「掃描式」撕裂 |
| ✅ **空間切片** | 把 frame 沿珍珠方向切成 N 段，每段獨立編碼 | **零撕裂**，本設計採用 |

```
frame 的編碼結果（I80：q × 32 bytes）
├── slice 0  ← dma[0]（SRAM）  ← DMA 發射中
├── slice 1  ← dma[1]（SRAM）  ← CPU 編碼中
├── slice 2  ← dma[2]（SRAM）  ← 待送
└── ...      ← dma[k]（SRAM）
```

**每片大小 = `slice_leds × bpp × 24 bytes`**，而 **L80 單筆交易上限 32768 bytes**
（`esp_lcd_panel_io_i80.c` 的 `max_transfer_bytes`，且 `tx_color` 是**自足交易**，帶 CMD/END）。

### 7.3 ⭐ 修正 ②：兩層的耦合 —— 我上一版「重複送幀」的說法要收回

這是我要更正的重點。上一版我說：

> ~~「WS2812 是保持型裝置，所以重複送同一幀完全合法，`queue_depth=2` 就夠」~~

**這句話只對了一半。** 真正的問題是 **像素緩衝會被鎖多久**：

```
若切片傳輸 + 重複送幀：
  像素緩衝 pix[0] 必須保持有效，直到「整幀 N 片全部送完」
  → 鎖定時間 = N × 20 ms，不是 20 ms ❌
```

**→ 重複送幀與小切片是互斥的。**

**但正確答案更簡單，而且完全符合你的描述：**

> **編碼是「同步、一次性」發生在 `show()` 內部的。**
> 所以 **pix 只需要在編碼期間（< 1 ms）保持有效**，之後就自由了。

```
show() 內部（C，同步）：
  1. 鎖定 pix[back]
  2. for each slice: 編碼 pix[back] 的這一段 → dma[free]  →  入隊（非同步）
  3. 解鎖 pix[back]        ← ⭐ 這裡就解鎖了，不用等到 DMA 送完
  4. 翻轉 pix 雙緩衝
  5. return tid
```

**這才是真正的解耦**：像素緩衝只在「編碼的 1 ms」被佔用，顯示則在背景跑 20 ms。兩者完全獨立。

### 7.4 修正後的完整時間軸

```
        ├─ show() ─┤
應用：  [寫 pix[1] ][呼叫 show][寫 pix[0] ][呼叫 show][寫 pix[1] ]...
                     ↑編碼 1ms        ↑編碼 1ms
                     ↓解鎖 pix[1]     ↓解鎖 pix[0]
SRAM：              [s0→DMA][s1→DMA][s2→DMA]        ← 幀 N 的 3 片
                            [s0→DMA][s1→DMA][s2→DMA] ← 幀 N+1 的 3 片
                     └──────── 20 ms ────────┘
顯示：  ═══════════════ 16 條燈帶持續輸出，零空隙 ═══════════════
```

**注意**：應用寫 `pix[1]` 時，`pix[0]` 正在被編碼（或空閒）；**永遠不會有「DMA 正在讀 pix」的情況**，
因為 DMA 讀的是 **SRAM 裡的編碼結果（slice）**，不是 pix。這就是兩層解耦的關鍵。

### 7.5 `show()` 的接受語意（你要求的「能寫 / 不能寫」回報）

```python
tid = bus.show()
#   tid  = int  → ✅ 已接受，這幀會顯示；tid 可傳給 wait()
#   tid  = None → ❌ 未接受（切片全忙），這幀被丟棄，dropped++
```

| 回傳 | 意義 | 應用該怎麼做 |
|---|---|---|
| `int` | 已接受（編碼完成並入隊） | 繼續下一幀 |
| `None` | 未接受（所有 slice 都在忙） | 數 `dropped`、算真實 FPS、決定是否放慢 |

**應用側 FPS 計算變得非常直接**：

```python
import time
n_ok = n_drop = 0
t0 = time.ticks_ms()
while True:
    fill_effect(bus.buf)
    if bus.show() is not None:
        n_ok += 1
    else:
        n_drop += 1
    # 每秒結算一次
    if time.ticks_diff(time.ticks_ms(), t0) >= 1000:
        print(f"accepted={n_ok}/s dropped={n_drop}/s  "
              f"leds/s={n_ok * total_leds}")
        n_ok = n_drop = 0
        t0 = time.ticks_ms()
```

> **這正是你要的**：`show()` 的回傳值 = 顯示層對應用層的**背壓訊號**。
> 應用可以用它做閉環控制（例如動態降低效果複雜度），或純粹觀測。

### 7.6 定案架構（與你的描述一致）

```
┌──────────────────────────────────────────────────────────────────┐
│ 應用層                                                            │
│   寫 pix[back]（PSRAM，4-byte RGBW）→ show()                      │
└───────────────────────────┬──────────────────────────────────────┘
                            │ show() 同步：編碼 + 入隊 + 翻頁
                            │ 回 tid（接受）/ None（丟棄）
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│ 編碼器（C，IRAM 執行，LUT 在 SRAM）                                │
│   for each slice: pix[back] 的這一段 → LUT → 位元轉置 → dma[free]  │
└───────────────────────────┬──────────────────────────────────────┘
                            │ 入隊（非同步）
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│ 硬體 I80 DMA：dma[0..N-1] ping-pong，16 條燈帶同時輸出             │
│   一個在發射、一個在編碼、其餘待送                                  │
└──────────────────────────────────────────────────────────────────┘

記憶體：
  像素 pix[2]   ← PSRAM     85 KB（與 lane 數無關）
  切片 dma[N]   ← 內部 SRAM  N × slice_bytes（與 lane 數無關）
  LUT          ← 內部 SRAM  24~48 KB
```

### 7.7 切片數該選多少？

| 每 lane 顆數 | frame 編碼大小（RGBW） | 需幾片（單筆 ≤32 KB） | 建議 `queue_depth` |
|---|---|---|---|
| 200（你的場景） | 18.8 KB | **1**（不需切） | 2（ping-pong） |
| 310 | 29.1 KB | 1（臨界） | 2 |
| 400 | 37.5 KB | 2 | 3 |
| 500 | 46.9 KB | 2 | 3 |
| **666** | **62.4 KB** | **2** | **3~4** |
| 1000 | 93.8 KB | 3 | 4 |

> **你的場景（14 × 200）根本不需要切片** —— 整幀 18.8 KB 塞得進單筆交易，
> 所以 SRAM 側就是單純的 **2 塊 ping-pong（37.5 KB）**。
> 切片數的完整計算規則見 **§7.8**（由可用 SRAM 反推，非固定三份）。

---

### 7.8 ⭐ 切片數由「可用 SRAM」反推（不是固定三份）

> 你的澄清：*「大緩衝拆三份這裏我只是比喻，實際上我們要根據我們可用的 SRAM 來判斷應該拆多少份」*
>
> **正確。下面是計算規則。**

**常數**：
- `SLICE_MAX = 32768 bytes` —— **I80 單筆交易硬上限**（IDF 的 `assert(color_size <= bus->max_transfer_bytes)`）
- `frame = q × bpp × 24 bytes` —— 整幀編碼結果
- `budget = avail_sram − LUT` —— 可用給切片的內部 SRAM

**演算法**：

```python
SLICE_MAX = 32768

def plan_slices(frame, budget):
    """回傳 (slices, slice_bytes)；無法配置回 None"""
    cap = min(SLICE_MAX, budget)
    if frame <= cap:
        return 1, frame                       # 整幀一筆，不需切片
    # 至少留 2 片做 ping-pong；盡量取大以減少片數
    slice_bytes = min(SLICE_MAX, budget // 2)
    if slice_bytes == 0:
        return None
    slices = (frame + slice_bytes - 1) // slice_bytes
    if slices * slice_bytes > budget:         # 放不下
        return None
    return slices, slice_bytes
```

**結果表**（RGBW，`SLICE_MAX = 32 KB`）：

| 每 lane 顆數 | frame | `budget=100 KB` | `budget=200 KB` |
|---|---|---|---|
| 200（**你的場景**） | 18.8 KB | **1 片**（用 18.8 KB） | 1 片 |
| 310 | 29.1 KB | **1 片**（用 29.1 KB） | 1 片 |
| 400 | 37.5 KB | 2 片（用 64 KB） | 2 片 |
| 500 | 46.9 KB | 2 片（用 64 KB） | 2 片 |
| 666 | 62.4 KB | 2 片（用 64 KB） | 2 片 |
| 800 | 75.0 KB | 3 片（用 96 KB） | 3 片 |
| 1000 | 93.8 KB | 3 片（用 96 KB） | 3 片 |
| 1500 | 140.6 KB | ❌ 5 片 (160 KB) 不夠 | 5 片 (160 KB) |

> **注意**：`slice_bytes` 在中大型場景會被 **32 KB 的硬上限**卡住，而不是被 SRAM 卡住。
> 所以「幾片」主要由 `frame / 32 KB` 決定；SRAM 決定的是**能不能容納 `slices × slice_bytes`**。
>
> **你的場景（14 × 200）是 1 片** —— 完全不需要切片邏輯，SRAM 側就是單純 2 塊 ping-pong。

**自動降級策略**（`plan_slices` 回 `None` 時）：

| 順序 | 動作 | 效果 |
|---|---|---|
| 1 | 關掉 LUT（旗標展開 + popcount） | budget +24~48 KB |
| 2 | `write` 遮罩改 RGB（3ch） | frame −25% |
| 3 | 降低 `pixel_buffers`（PSRAM，不影響 SRAM） | 無效 |
| 4 | 減少 lane 數（8 lane / 8-bit 模式） | frame −50% |
| 5 | 降到 2 片以下 → ❌ 不允許（會失去 ping-pong） | — |

> **建構期就決定並回報**：`bus.info()['slices']`、`bus.info()['slice_bytes']`、`bus.info()['sram_used']`。
> 若 `plan_slices` 回 `None` → **raise `MemoryError`**（設定問題，人為可修）。

---

## 8. 與你現有 Python 世界的接軌（薄風格層）

`led/` 套件**不含編碼邏輯**，只是把 C 模組包成你習慣的形狀：

```python
# led/LEDController.py — 對應 mp_Net-Core 的 PixelController
class PixelController:
    """pixel 控制器 — 直接吃 led.WS2812Bus，編碼全在 C。
    這裡只負責 order / 中立值 / 亮度這些「語意」。
    """
    def __init__(self, bus, lane, num_pixels, order="GRBW", dStay=0):
        self.bus = bus
        self.lane = lane
        self.num_pixels = num_pixels
        self.neutral_value = dStay

    def st_load_and_convert(self, source_buffer, offset):
        """把來源的 4-byte RGBW 資料寫進 C 的 back buffer。
        ⚠️ 這是一次 memcpy；要零拷貝就讓效果直接寫 bus.back_buffer()。"""
        self.bus.view(self.lane)[:] = source_buffer[offset:offset + self.num_pixels * 4]

    def st_show(self):
        pass        # 編碼 + 顯示由 bus.present() 統一驅動，不在這裡做
```

`PixelStreamer.show_all()` 的對應改動：

```python
def show_all(self):
    for i, ctrl in enumerate(self.controllers):
        ctrl.st_load_and_convert(self.big_buffer, self.offsets[i])
    self.bus.present()      # 🔥 一次送出全部 lane（硬體平行）
```

> **⚠️ 效能提醒**：`st_load_and_convert` 的 memcpy 是唯一的拷貝。
> 要零拷貝，讓效果**直接寫 `bus.back_buffer()`**（layout 就是 `[lane][q][4]`，與 `big_buffer` 同構），
> 這樣 `PixelStreamer` 可以完全退化成「效果寫 back_buffer → `present()`」。

---

## 9. 與 `neopixel` 的相容（路徑 ①）

```python
# led/__init__.py
def neopixel_compat(pin, q, order="GRB"):
    """neopixel.NeoPixel 形狀的 3-byte 外觀，內部 stride-4 寫進 C 的 back buffer。
    給現有 PixelController._convert 零修改使用。"""
```

| 舊介面 | 新介面 | 實作 |
|---|---|---|
| `.buf`（3 bytes/px） | `bus.back_buffer()`（4 bytes/px） | 轉接層做 stride 映射 |
| `.write()` | `bus.present()` | 別名 |
| `.n` | `.q` | 別名 |

---

## 10. 建置整合

### 10.1 官方模板：`mp_jpeg`（你指定的）

> **你的指示**：*「你應該參考最正式的模板應該是 mp_jpeg」*
>
> **收到，我先前用 `mp_rs485_hd` 是錯的**（那是你已刪除的舊 module）。改用 `mp_jpeg`。

**`mp_jpeg` 的模板特徵（我逐項讀過，全部照抄）：**

| # | 特徵 | `mp_jpeg` 的做法 | 本專案 |
|---|---|---|---|
| 1 | 目錄 | 扁平 `src/*.c` + `micropython.cmake` + `CMakeLists.txt` + `idf_component.yml` | 同（但 C 檔較多，見 §1.2） |
| 2 | 建構子 | `mp_arg_parse_all_kw_array` + `allowed_args[]` + `enum { ARG_... }` 索引 | ✅ 完全照抄 |
| 3 | 物件配置 | **`mp_obj_malloc_with_finaliser(type, obj_t)`**（支援 `__del__`） | ✅ |
| 4 | 型別註冊 | `MP_DEFINE_CONST_OBJ_TYPE(...)` + `make_new` + `locals_dict` | ✅ |
| 5 | 模組註冊 | `MP_REGISTER_MODULE(MP_QSTR_jpeg, mp_module_jpeg);`（2 參數） | ✅ |
| 6 | 參數檢查 | 不合法 → `mp_raise_msg_varg(&mp_type_ValueError, ...)`；可容忍 → `mp_printf` 警告不 raise | ✅ **與你的三套錯誤策略一致**（§1.2 風格母本） |
| 7 | 常數字串 | `target_compile_definitions(... MICROPY_ROM_TEXT_COMPRESSION=0)`（模組字串不壓縮，避免體積增加） | ✅ |
| 8 | 版本回報 | `jpeg.version()` + `MP_JPEG_DRIVER_VERSION` 編譯期巨集 | ✅ `led_bus.version()` |
| 9 | 相依元件 | `idf_component.yml` 宣告 `espressif/esp_new_jpeg: "^1.0.0"` | ⚠️ **本專案不需要**（見下） |
| 10 | `CMakeLists.txt` | `idf_component_register()` 薄殼，讓 IDF Component Manager 處理 `idf_component.yml` | ⚠️ 同上（可省） |
| 11 | CI | `.github/workflows/ESP32.yml` 真的編一個 MicroPython 來驗證 | ✅ 建議加 |

> **本專案不需要 `idf_component.yml`**：`mp_jpeg` 要它是因为 `esp_new_jpeg` 是**外部 managed component**。
> 我們用的 `esp_lcd` / `esp_driver_spi` / `esp_driver_i2c` / `esp_driver_rmt` / `esp_driver_parlio`
> **都是 IDF 內建元件**，只要用 `idf_component_get_property` 拉 include 即可。

### 10.2 `micropython.cmake`

```cmake
# micropython.cmake — mp_LEDdriver: ESP-IDF native LED bus driver for MicroPython
#
# 掛載（mp_Make-Tools 的 exmod.list）：
#   "/mp_LEDdriver/mp_led_bus/micropython.cmake"
#
# 技術棧優先序：ESP-IDF 原生 > MicroPython 原生 > 自寫 C（見 C_API.md §1.4.2）
#   WS2812  → esp_lcd (I80)  / esp_driver_parlio (P4)
#   APA102  → esp_driver_spi
#   PCA9685 → esp_driver_i2c
#   PWM LED → MicroPython machine.PWM（不在此模組）

add_library(usermod_mp_led_bus INTERFACE)

set(INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}
)

set(SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/modled_bus.c
    ${CMAKE_CURRENT_LIST_DIR}/led_mgr.c
    ${CMAKE_CURRENT_LIST_DIR}/led_frame.c
    ${CMAKE_CURRENT_LIST_DIR}/led_slice.c
    ${CMAKE_CURRENT_LIST_DIR}/led_ws_i80.c
    ${CMAKE_CURRENT_LIST_DIR}/led_ws_parlio.c
    ${CMAKE_CURRENT_LIST_DIR}/led_ws_rmt.c
    ${CMAKE_CURRENT_LIST_DIR}/led_apa_spi.c
    ${CMAKE_CURRENT_LIST_DIR}/led_pca_i2c.c
    ${CMAKE_CURRENT_LIST_DIR}/led_wave.c
    ${CMAKE_CURRENT_LIST_DIR}/led_encode.c
    ${CMAKE_CURRENT_LIST_DIR}/led_color.c
    ${CMAKE_CURRENT_LIST_DIR}/led_alloc.c
)

if(ESP_PLATFORM)
    # 把 IDF 內建元件的 include 路徑拉進來（照 mp_rs485_hd 的手法，mp_jpeg 亦同）
    foreach(comp esp_lcd esp_driver_spi esp_driver_i2c esp_driver_rmt
                 esp_driver_parlio esp_driver_ledc esp_mm hal soc)
        idf_component_get_property(_inc ${comp} INCLUDE_DIRS)
        idf_component_get_property(_dir ${comp} COMPONENT_DIR)
        if(_inc)
            list(TRANSFORM _inc PREPEND ${_dir}/)
            list(APPEND INCLUDES ${_inc})
        endif()
    endforeach()
endif(ESP_PLATFORM)

target_sources(usermod_mp_led_bus INTERFACE ${SOURCES})
target_include_directories(usermod_mp_led_bus INTERFACE ${INCLUDES})

# 模組字串不適合壓縮（照 mp_jpeg 的做法）
target_compile_definitions(usermod_mp_led_bus INTERFACE
    MICROPY_ROM_TEXT_COMPRESSION=0
)

target_link_libraries(usermod INTERFACE usermod_mp_led_bus)
```

### 10.3 `micropython.mk`（legacy make build）

```make
# micropython.mk — build rules (legacy make build system)
MOD_DIR := $(USERMOD_DIR)

CFLAGS_USERMOD += -I$(MOD_DIR)
SRC_USERMOD_C += $(MOD_DIR)/modled_bus.c \
                 $(MOD_DIR)/led_mgr.c \
                 $(MOD_DIR)/led_frame.c \
                 $(MOD_DIR)/led_slice.c \
                 $(MOD_DIR)/led_ws_i80.c \
                 $(MOD_DIR)/led_ws_parlio.c \
                 $(MOD_DIR)/led_ws_rmt.c \
                 $(MOD_DIR)/led_apa_spi.c \
                 $(MOD_DIR)/led_pca_i2c.c \
                 $(MOD_DIR)/led_wave.c \
                 $(MOD_DIR)/led_encode.c \
                 $(MOD_DIR)/led_color.c \
                 $(MOD_DIR)/led_alloc.c
```

### 10.4 模組註冊（照 `mp_jpeg` 的四段式）

```c
/* ── 模組方法表 ─────────────────────────────────────────────── */
static const mp_rom_map_elem_t led_bus_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_led_bus) },
    { MP_ROM_QSTR(MP_QSTR_WS2812Bus), MP_ROM_PTR(&mp_led_ws2812_bus_type) },
    { MP_ROM_QSTR(MP_QSTR_APA102),    MP_ROM_PTR(&mp_led_apa102_type) },
    { MP_ROM_QSTR(MP_QSTR_PCA9685),   MP_ROM_PTR(&mp_led_pca9685_type) },
    { MP_ROM_QSTR(MP_QSTR_version),   MP_ROM_PTR(&mp_led_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_mgr_info),  MP_ROM_PTR(&mp_led_mgr_info_obj) },
};
static MP_DEFINE_CONST_DICT(led_bus_module_globals, led_bus_module_globals_table);

const mp_obj_module_t mp_module_led_bus = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&led_bus_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_led_bus, mp_module_led_bus);
```

### 10.5 `mp_Make-Tools` 註冊

`make_config.json`：
```json
"exmod": {
  "root": "ext_mod",
  "list": ["/mp_jpeg/micropython.cmake", "...", "/mp_LEDdriver/mp_led_bus/micropython.cmake"]
}
```

`git_config.json`（照 `mp_jpeg` / `mp_heap_caps` 的形狀）：
```json
"mp_LEDdriver": {
  "dir": "ext_mod/mp_LEDdriver",
  "url": "https://github.com/itdogwowo/mp_LEDdriver.git",
  "ref": "main",
  "recursive": true
}
```

### 10.6 CI（照 `mp_jpeg` 的 `.github/workflows/ESP32.yml`）

> `mp_jpeg` 有一條真正的建置驗證 CI（下載 MicroPython + ESP-IDF，實際編一個韌體）。
> **本專案更需要它** —— 因為 C 檔多、還有 `esp_lcd` / `esp_driver_parlio` 的條件編譯（S3 vs P4）。
> 建議 matrix：`esp32s3` × `esp32p4`。

---

## 11. ⭐ 統一 **LED** 資源與單一中斷管理器（你授權的關鍵設計）

> **你的授權原文**：*「最後一點就是一部份妥協的許可，讓你須知，我知道這樣的 API 設計上是非常困難的，因為在傳輸的過程當中有計算要交回 python 以及讓他及時地計算是非常難，所以我最後的妥協，是整合所有 lcd 資源，用一個大的中斷來管理，這樣也能夠充分發揮每一個 bus DMA 非中斷，從而宏觀地用了少了時間。」*
>
> **我接受這個授權，並且認為這是整份設計裡最重要的一個決定。** 本節把它正式化。

### 11.1 這個授權解決了什麼問題

原本（不授權）的設計會被迫在傳輸期間「回呼 Python 做計算」，這在 MicroPython 上是不可能的：

| 困難 | 為什麼在 mpy 做不到 |
|---|---|
| 傳輸中把計算交回 Python | Python 有 GIL、有 GC、有 bytecode 解譯 → **延遲不可預測（ms 級）** |
| Python 要「及時」算完下一幀 | 無法保證在 DMA 完成前算完 → 顯示會斷 |
| 多個 bus 各自的 ISR 協調 | 多個中斷互相搶佔 → 抖動疊加 → **時序更容易崩** |
| 每幀一次 Python 往返 | 光是 VM 切換成本就吃掉預算 |

**→ 你的授權把「不可能」變成「可做」：**

> **不在傳輸期間回呼 Python。改成一個高優先級中斷，統一管理所有 LCD/DMA 資源，在 ISR 內部就把下一個 transfer 鏈上去。**

### 11.2 為什麼「單一大中斷」在技術上更優（不只是妥協）

| 面向 | 多個 bus 各自 ISR | ✅ 單一統一 ISR |
|---|---|---|
| 中斷次數 | 每個 bus 各一次（疊加） | **一次** |
| 中斷抖動 | ISR 互相搶佔，最壞情況疊加 | **可控**（固定順序、固定成本） |
| 顯示空隙 | 多個 ISR 排隊 → 空隙不可控 | **零空隙**（ISR 內直接鏈下一個 transfer） |
| 資源仲裁 | 各 bus 獨立，可能同時搶 DMA/IRAM | **單一仲裁者，不會互搶** |
| Python 影響 | 每個 ISR 都可能撞到 GIL/GC | **統一時機**，可刻意避開 GC |
| 除錯 | 多個來源、難以重現 | **單一進入點**，可完整記錄時間戳 |

> **「宏觀地用了少了時間」正是這個意思**：多個小中斷 × 各自抖動 > 一個大中斷 × 固定成本。

### 11.3 S3 / P4 的可行性（我查證過）

| 平台 | 統一管理可行嗎 | 依據 |
|---|---|---|
| **ESP32-S3** | ✅ **天生可行** | **`SOC_LCD_I80_BUSES = 1`** —— 只有**一個** LCD_CAM/I80 控制器，所有 I80 LED bus 本來就共用同一個週邊、同一個 `TRANS_DONE` 中斷（顯示面板另計，見 §1.3） |
| **ESP32-P4** | ✅ 可行 | LCD_CAM（I80 + RGB）與 PARLIO 是不同週邊，但可**指定同一個 ISR** 或由一個 master ISR 統一驅動（見 11.4） |

**S3 特別有利**：若用 RMT 推多條 LED，多個 RMT 通道各有中斷；
但用 **I80 推 16 條 LED 時，全部 lane 共用一個 `TRANS_DONE`** —— **天然就是單一中斷源**，不是硬湊。
（顯示面板不納入 `led_mgr`，見 §1.3）

### 11.4 設計：`led_mgr`（**LED 專屬**的統一資源與中斷管理器）

```c
/* led_mgr.h — 全晶片的 LCD/DMA 資源仲裁者（單例） */
typedef struct {
    /* 所有受管理的 bus（LED + 顯示） */
    struct led_bus_obj *buses[LED_MGR_MAX_BUSES];
    int n_buses;

    /* 單一完成中斷的上下文 */
    volatile int      active;          /* 目前誰在傳 */
    volatile uint32_t irq_count;
    volatile uint32_t irq_max_us;      /* 最長一次 ISR（監測抖動）*/
    volatile uint32_t chain_misses;    /* ISR 內找不到下一塊可鏈的次數 */

    /* 需要在 ISR 內完成的事（預先算好，ISR 只做指標操作）*/
    struct led_slice   *pending[LED_MGR_MAX_BUSES];
} led_mgr_t;

extern led_mgr_t g_led_mgr;
```

**ISR 內的動作（極簡、固定成本、不碰 Python）**：

```c
static void IRAM_ATTR led_mgr_isr(void *arg)
{
    uint32_t t0 = esp_cpu_get_cycle_count();

    struct led_bus_obj *b = g_led_mgr.buses[g_led_mgr.active];
    b->queue_head = (b->queue_head + 1) % b->queue_depth;
    b->queue_count--;
    b->frames++;

    /* 🔥 直接在 ISR 內鏈下一個 transfer —— 零空隙的關鍵 */
    if (b->queue_count > 0) {
        struct led_slice *nx = &b->slices[b->queue_head];
        esp_lcd_panel_io_tx_color(b->panel_io, -1, nx->buf, nx->len);  /* 非阻塞入隊 */
    } else {
        g_led_mgr.chain_misses++;    /* 應用來不及 → 顯示空隙（可觀測） */
    }

    g_led_mgr.irq_count++;
    uint32_t dt = esp_cpu_get_cycle_count() - t0;
    if (dt > g_led_mgr.irq_max_us) g_led_mgr.irq_max_us = dt;   /* 抖動監測 */
}
```

**關鍵性質**：
1. **ISR 內不碰 Python、不配置記憶體、不做編碼** —— 只有指標運算 + 一次 `tx_color`
2. **編碼留給 `show()`（在 Python 呼叫的執行緒上下文）** —— 那裡才有時間做重運算
3. **零空隙來自 ISR 內直接鏈結**，不是靠 Python 趕上
4. **`chain_misses` 是可觀測的「應用來不及」計數** —— 取代猜測

### 11.5 這如何改變 API 契約（以及妥協在哪裡）

| 項目 | 原本（不可能） | ✅ 授權後 |
|---|---|---|
| 傳輸中的計算 | 回呼 Python 算下一幀 | **不回呼**。Python 在 `show()` 時算完 |
| 「及時」的保證 | 依賴 Python 排程 | **不依賴**。ISR 只鏈已編好的切片 |
| 應用太慢時 | 顯示斷裂、不可預期 | **`chain_misses++`**，顯示斷但**可計數、可觀測** |
| Python 的角色 | 參與即時迴圈（危險） | **只在 `show()` 進入一次**，之後全自動 |
| 多 bus | 各自為政 | **統一仲裁**，一次中斷服務全部 |

**妥協的內容（明確記錄）**：
- 我們**放棄**「傳輸期間動態改變編碼參數」的能力（例如傳到一半改亮度）。**必須在 `show()` 之前決定。**
- 我們**放棄**「每幀與 Python 同步」的嚴格保證。改成**接受丟幀 + 可觀測**（`dropped` / `chain_misses`）。
- 換得：**確定性的零空隙顯示**，以及**多 bus 共用一次中斷成本**。

> **這個交換是值得的**：動態改參數可以用「下一幀生效」實現（延遲一幀），
> 而時序確定性是 WS2812 唯一不能妥協的東西。

### 11.6 `led_mgr` 的公開 API（**只管 LED**）

```python
import led_bus

led_bus.mgr_info()
# {'buses': 2, 'irq_count': 12345, 'irq_max_us': 3, 'chain_misses': 0,
#  'active': 'ws2812#0', 'free_irq_budget_us': 47}

led_bus.mgr_stats()
# 每個 LED bus 的 frames / dropped / encode_us / dma_us
```

> **⚠️ 邊界（依你的指示修正）**：`led_mgr` **只管 LED bus**。
> **不碰 `mp_lcd_bus`、不碰顯示面板、不做跨模組 claim。**
> 上一版我寫的 `led.mgr_claim("lcd_bus")` 是錯誤設計，**已刪除**。
>
> **同一台機器要同時有 I80 LED 與顯示時的處理**（見 §1.3）：
> - 顯示改走 **RGB LCD 或 SPI**，I80 留給 LED；或
> - LED 改走 **RMT（≤4 條）**，I80 留給顯示；或
> - 用 **P4**（PARLIO 給 LED、LCD_CAM 給顯示，兩者不衝突）。

---

## 12. 實作順序：分階段，每階段可獨立驗收

> **你的指示**：*「我不一定要一個版本就做好的，我是完全接受分開多個階段來做的。」*
>
> **每個階段都有「能獨立跑起來並驗收」的產物**，不需要等全部做完。任何一階段停下，前面的成果都可用。

### C0 — 建置管線打通（**最小可驗收單元**）

| 項目 | 內容 |
|---|---|
| 產物 | `mp_led_bus/` 目錄 + `micropython.cmake` + `modled_bus.c` + 最小 `WS2812Bus` type |
| 功能 | 建構子解析參數、配置記憶體、`info()` / `stats()` |
| **驗收** | `import led_bus` 成功；`bus = led_bus.WS2812Bus(pins=5, q=10)` 建得起；**`info()` 印出實測 `sram_free`** |
| 為什麼先做這個 | **把「可用 SRAM 到底多少」從估算變成實測** —— 後面所有切片決策的基礎 |

> ⭐ **C0 的關鍵價值**：我們現在所有的 SRAM 數字都是估算（我算 200 KB 可用）。
> C0 之後就是實測。**若實測比預期少，後面的計畫要改；所以先做這個。**

### C1 — WS2812 單條會亮（驗證編碼器正確性）

| 項目 | 內容 |
|---|---|
| 產物 | `led_ws_i80.c` + `led_wave.c`（ws2812_pattern）+ `led_encode.c` |
| 功能 | 單 lane、`q` 小（10 顆）、同步 `show()`、`lcd_cmd_bits=0` |
| **驗收** | **10 顆燈會亮、顏色/order 正確**；用邏輯分析儀量一條燈帶的實際波形 |
| 風險 | 🔴 **R0（§9）**：yves 編碼器的實際波形語意未完全確認 → **這一階段就是要解掉它** |

### C2 — 16 lane + 非同步 + 緩衝架構

| 項目 | 內容 |
|---|---|
| 產物 | 雙層雙緩衝（§7）+ 三態 ring + `bus.buf` / `show()` 接受語意 + `plan_slices()` |
| 功能 | 14 lane × 200 顆、非同步、零空隙 |
| **驗收** | 你的實際場景跑起來；`stats()` 的 `encode_us` / `dropped` / `leds_per_sec` 可觀測 |

### C3 — `led_mgr` 統一 LED 中斷管理器

| 項目 | 內容 |
|---|---|
| 產物 | `led_mgr.c/.h`（§11） |
| 功能 | 多 bus 共用一個完成中斷；ISR 內直接鏈下一個 transfer |
| **驗收** | `chain_misses` 為 0；`irq_max_us` 穩定；多 bus 同時跑不掉幀 |

### C4 — 滿載壓力測試（決定切片策略）

| 項目 | 內容 |
|---|---|
| 功能 | 16 lane × 666 顆（或你的上限） |
| **驗收** | 依實測 `sram_free` 決定 `plan_slices()` 結果；確認顯示 free-running |

### C5 — APA102（I80 通用波形引擎的第一個擴充）

| 項目 | 內容 |
|---|---|
| 產物 | `led_wave.c` 的 `apa102_pattern()` + `led_apa.c` |
| 功能 | k=2、CLK lane + MOSI lane、2–12 MHz 可配 |
| **驗收** | 290 顆 APA102 會亮、顏色正確；量 SPI CLK 波形 |
| 風險 | R2（注入時鐘緣）、R4（36.5 KB 波形） |

### C6 — PCA9685（同一個波形引擎）

| 項目 | 內容 |
|---|---|
| 產物 | `i2c_pattern()` + `led_pca.c` |
| 功能 | k=2、SCL/SDA 開汲極、400 kHz |
| **驗收** | 16 通道 12-bit 輸出正確；量 I2C 波形（含 ACK） |
| 風險 | R3（對畸形 I2C 敏感） |

### C7 — Python 風格層（與你現有世界觀接軌）

| 項目 | 內容 |
|---|---|
| 產物 | `led/` 套件：`LEDCommander` / `LEDController` / `neopixel_compat` |
| **驗收** | 你現有的 `main.py` 可移植過來跑 |

### C8 — 選配

| 項目 | 內容 |
|---|---|
| I2S 後端 | 只在「I80 被佔用又要 >4 lane」時 |
| PARLIO 後端 | P4 的 32 lane |
| RMT 後端 | 少條備援 |
| I80 波形引擎 | APA/PCA 的原生週邊用完時的備援（§1.5） |

### 里程碑對照

```
C0 ──▶ C1 ──▶ C2 ──▶ C3 ──▶ C4       ← WS2812 完整可用（核心價值）
      └─ 驗證編碼器  └─ 你的場景  └─ 統一中斷

C5 ──▶ C6 ──▶ C7                       ← 其他協定 + 風格層
      └─ APA102   └─ PCA9685  └─ 接軌舊程式
```

> **建議：C0–C4 是一條完整的價值線**（WS2812 從「會亮」到「滿載 free-running」）。
> C5 之後是擴充，可以之後再排。

---

## 13. 待你確認

| # | 問題 | 我的建議 |
|---|---|---|
| **B1** | 模組名：`led` / `_led` / `led_bus`？ | **`led`**（照 `lcd_bus` 用 `lcd` 的慣例；C module 名 = Python import 名） |
| **B2** | `queue_depth` 預設 | **2**（可容忍一幀抖動，不丟幀） |
| **B3** | `present()` 佇列滿時：等待還是丟幀？ | **`queue_depth>=2` 等待、`==1` 丟幀**（語意明確，不 raise） |
| **B4** | 像素 buffer 是否也要 `queue_depth` 那麼多塊？ | **不用，固定 2**（多於 2 無益，只增加延遲） |
| **B5** | 要不要 `bus.freeze()` / `bus.thaw()`（暫停/恢復顯示）？ | 先不做，用 `clear()` + 停呼叫 `present()` 即可 |
| **B6** | `stats()` 要不要回 `fps`？ | **回 `leds_per_sec`**（`顆數 × fps` 才是可比較的指標，你指出的） |
