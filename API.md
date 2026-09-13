# mp_LEDdriver — API 設計稿（v0.2，討論用）

> **用途**：新 mpy 多 LED 驅動器的 **Python API** 設計。本階段**只定 API、不寫 C 後端**。
> **狀態**：討論稿。回應你「先純 Python 定 API，C 後端後補」+「核心只有 i80 與 parlio」+「參考 mp_lcd_bus」的指示。
> **最後更新**：2026-09-13
> **前置閱讀**：`DESIGN.md`（硬體/DMA 架構與現況診斷）
> **參考**：`mp_lcd_bus`（API 母本，最重要）、`FastLED` 上游 lib（`fl/channels/` driver 抽象）、`mp_Net-Core`（風格母本）

---

## 0. 這份文件的三個來源

| 來源 | 借什麼 |
|---|---|
| **`mp_lcd_bus`**（你自己寫的） | **整個 API 形狀**：建構子 kwarg 風格、`write(buf) → trans_id`、`is_busy()` / `pending()` / `wait()` / `wait_all()`、queue 深度、`ref_bufs` 防 GC、`-1` 表示不管理的腳位 |
| **FastLED 上游 lib**（`/tmp/fastled_upstream` @ 3.10.3） | **driver 抽象**：`Bus` enum（AUTO / RMT / FLEX_IO / SPI / UART）+ `ChannelManager` 自動選 bus + 明確 pin 住 bus 的能力；以及 yves I80 driver 的編碼器（已逆向驗證） |
| **`mp_Net-Core` / `mp_LEDController`** | **風格**：dict config、12-bit `array('H')`、`show()` / `set_buf()` 語意、三套錯誤策略、`_MP` 雙路徑、`write` 白名單遮罩概念 |

---

## 0.5 三個已定案的前提（由你確認）

| # | 前提 | 影響 |
|---|---|---|
| **①** | **統一像素格式：每個 LED 一律 4 bytes `[R,G,B,W]`** | 效果之間可自由轉換；編碼器用 `write` 遮罩決定讀哪幾個通道 → **統一格式的額外成本是零** |
| **②** | **DMA 緩衝：S3 只能內部 SRAM；P4 可用 PSRAM** | S3 的 RAM 預算是硬約束（§3.4.1）；滿載 16 lane × 666 顆需 217 KB |
| **③** | **驅動器職責：不斷讀取 RGBW 緩衝，非中斷地顯示** —— fps 是推導值，不是目標（見 §3.3.1） | 顯示 free-running 跑硬體上限；應用寫入節奏自由；兩者解耦 |

---

## 1. 設計原則（本輪新增）

| # | 原則 | 理由 |
|---|---|---|
| P1 | **`led_bus` 是獨立模組，與 `lcd_bus` 平行** | `lcd_bus` 已穩定、i80 是它的核心；改成 `WS2812Bus` 會污染顯示用途。兩者共用「同一個 API 文法」即可 |
| P2 | **低階（`led_bus`）與高階（`led`）分離** | 低階吃 bytes、給人精確控制；高階吃 12-bit `array('H')`、給你現有的效果引擎 |
| P3 | **`.buf` 是「像素緩衝」，不是「硬體緩衝」** | 讓 `.buf` 保持 `neopixel.NeoPixel.buf` 的形狀（`lane × q × bpp` bytes），你的 `PixelController` 一行不用改 |
| P4 | **轉置（transpose）是 `show()` 的事，不是使用者的事** | 使用者永遠不碰 DMA buffer 格式；要最佳化再開 `bus=` 自己管 |
| P5 | **後端選擇與硬體解耦** | `bus=` 可選 `None`/`"auto"`/`"python"`/`"i80"`/`"parlio"`/`"rmt"`；純 Python 是合法的第一級後端（正確性參考實作） |
| P6 | **完成語意統一用 `trans_id`** | 直接沿用 `lcd_bus` 的 `write() → trans_id` / `wait(tid)`，學習成本為零 |

---

## 2. 模組與檔案佈局

```
mp_LEDdriver/
├── led_bus/                      ← 低階：匯流排（對應 lcd_bus）
│   ├── __init__.py               ← WS2812Bus / PARLIOBus 的 Python 後端 + 自動選擇
│   ├── _encode.py                ← 位元轉置 + LUT（viper，檔案尾自檢）
│   ├── _py_i80.py                ← 純 Python i80 後端（參考實作）
│   └── _py_parlio.py             ← 純 Python parlio 後端（參考實作）
├── led/                          ← 高階：像素控制器（對應 PixelController 世界觀）
│   ├── __init__.py
│   ├── LEDController.py
│   ├── LEDCommander.py
│   ├── LEDMathMethod.py
│   └── BufferReader.py
├── mp_led/                       ← C user module（第二階段才動）
└── ...
```

**命名理由**：`led_bus` ↔ `lcd_bus` 一眼看出是同一家族、同一文法。
C module 端維持 `mp_led` → `import _led`（第二階段）。

---

## 3. 低階 API：`led_bus`

### 3.1 類別總表

| 類別 | 底層 | lane | 可用晶片 | 狀態 |
|---|---|---|---|---|
| `led_bus.WS2812Bus` | 依 `bus=` 決定 | 1–16 | S3 / P4 | **本輪設計重點** |
| `led_bus.PARLIOBus` | `parlio_tx_unit` | 1–16（P4 ×2 unit → 32） | P4 / C6 / H2 / C5 | 本輪設計重點 |
| `led_bus.APABus` | SPI + DMA | 1 | S3 / P4 | 第二階段 |
| `led_bus.PCA9685Bus` | I2C bulk | 16 通道/晶片 | S3 / P4 | 第二階段 |
| `led_bus.PWMBus` | LEDC | 8（S3/P4） | S3 / P4 | 第二階段 |

### 3.2 `WS2812Bus` 建構子

```python
led_bus.WS2812Bus(
    pins,                      # int 或 tuple/list of int（1 個 = 單條，多個 = 平行多條）
    q,                         # int：每條顆數；或 tuple：各條不等長
    *,
    write=0b1111,              # 🔥 通道遮罩：0b1111=RGBW / 0b0111=RGB / 0b1000=單色
                               #    也可用字串 "rgbw" / "rgb" / "w"（沿用 mp_Net-Core 白名單）
    order="GRBW",              # "GRB" / "GRBW" / "RGBW" / "BGRW" …（決定硬體輸出順序）
    bus="auto",                # "auto" | "python" | "i80" | "parlio" | "rmt"
    wr=None,                   # i80 的 WR/CLK 腳；None = 自動挑
    chunk=0,                   # 每筆 DMA 的最大 bytes；0 = 自動（依 bus 上限）
    queue_depth=2,             # 佇列深度（I80 預設 4、PARLIO 預設 2）
    double_buffer=True,        # 雙緩衝
    brightness=4095,           # 全域亮度 0–4095（12-bit，不掉精度）
    gamma=None,                # None = 不做 gamma；或 float / 預算好的 256-byte LUT
    neutral=0,                 # dStay：停止/熄燈時回填的值
    timing=None,               # None = WS2812B 標準；可傳 (T0H,T0L,T1H,T1L) ns 或 "ws2811"
    overclock=1.0,             # 協定時鐘倍率（WS2812 可超到 ~1.5，直接提高顯示上限）
    mode="auto",               # 🔥 顯示模式："auto" | "skip" | "block" | "latest"（見 §3.3.1）
)
```

**`write` 遮罩的意義**（見 `DESIGN.md` §1.2）：

| 值 | 字串 | 編碼成本 | 適用 |
|---|---|---|---|
| `0b1111` | `"rgbw"` | 4 次查表/像素 | SK6812 RGBW |
| `0b0111` | `"rgb"` | 3 次查表/像素（省 25%） | WS2812B 標準 |
| `0b1000` | `"w"` | **1 次查表/像素（省 75%）** | 單色燈帶、PCA9685、電機 |
| `0b0100` | `"g"` | 1 次 | 只驅動綠通道的場景 |

> **⭐ 重點**：像素緩衝**永遠是 4 bytes/LED**（你定的規格，方便效果轉換），但編碼器只讀遮罩指定的通道 → **記憶體統一，成本不統一**。單色效果因此便宜 4 倍。

**與 `lcd_bus.I80Bus` 的對應關係**（可直接照抄的慣例）：

| `lcd_bus.I80Bus` | `led_bus.WS2812Bus` | 說明 |
|---|---|---|
| `data=(d0..d15)` | `pins=(p0..p15)` | tuple 長度 = lane 數 |
| `wr=10` | `wr=10` | I80 的 WR 腳 |
| `dc=-1` | — | LED 用途不需要 DC（永遠 data） |
| `cs=-1` | — | 同上 |
| `freq=10_000_000` | `overclock=1.0`（內部換算） | WS2812 的 clock 由協定時序反推，不直接指定 |
| `queue_depth=4` | `queue_depth=4` | 同語意 |

### 3.3 方法（與 `lcd_bus` 文法一致）

| 方法 | 回傳 | 說明 |
|---|---|---|
| `bus.buf` | `memoryview` | **像素緩衝**（writable），**每個 LED 固定 4 bytes `[R,G,B,W]`**，形狀 `[lane][q][4]`。給 viper `ptr8` 用 |
| `bus.buf16` | `memoryview('H')` | 每顆的 12-bit 亮度（給單色效果引擎用，對應 W 通道） |
| `bus.view(lane)` | `memoryview` | 單條燈帶的像素 view（給現有 `PixelController`） |
| `bus.views()` | `list` | 所有燈帶的 view |
| `bus.write_mask` | property | 讀寫通道遮罩 `0b1111` / `0b0111` / `0b1000`（**可在執行期切換**，單色效果切到 `0b1000` 省 4 倍） |
| `bus.brightness` | property | 讀寫全域亮度 0–4095 |
| `bus.set_neutral(v)` | `None` | 設 dStay 中性值 |
| `bus.fill(rgbw)` | `None` | 全部填同色（加速，不走 Python 迴圈） |
| `bus.clear()` | `None` | 填中性值（不是無腦填 0，見 `mp_Net-Core` 的教訓） |
| **`bus.show(*, lanes=None, wait=False)`** | `trans_id` 或 `None` | 🔥 轉置 + 送出。`lanes` 可只更新部分燈帶 |
| `bus.is_busy()` | `bool` | 與 `lcd_bus` 同名同義 |
| `bus.pending()` | `int` | 佇列中未完成的筆數 |
| `bus.wait(trans_id, timeout_ms=-1)` | `bool` | 等特定筆 |
| `bus.wait_all(timeout_ms=-1)` | `None` | 等全部 |
| `bus.lane_count()` | `int` | lane 數 |
| `bus.stats()` | `dict` | `{'frames','encode_us','dma_us','blocked_us','dropped','leds_per_sec'}` |
| `bus.deinit()` | `None` | 釋放硬體 |

> **`stats()` 用 `leds_per_sec` 而不是 `fps`**：因為 fps 單獨看沒有意義，`leds_per_sec = 顆數 × fps` 才是可比較的指標（你指出的重點。註：**16 lane 的像素吞吐固定在 533,333 顆/秒**，顆數與 fps 成反比）。

### 3.3.1 顯示模式：核心是「非中斷地不斷顯示」

> **🔧 修正**：我先前把「50 fps」當成設計目標是誤讀。**驅動器的職責是「不斷讀取 RGBW 緩衝然後顯示」**，fps 是推導出來的結果（見 `DESIGN.md` §2.4）。

#### 三件事各自獨立

```
① 顯示節奏 = 硬體上限（free-running）   ← 驅動器保證，永遠最快
② 應用寫入節奏 = 效果計算速度           ← 應用自由決定，愛多快就多快
③ 同步策略 = 撕裂 vs 丟幀              ← show() 的 mode 引數決定
```

#### `show()` 的 `mode` 引數（把 ③ 交給使用者選）

```python
bus.show()                       # 預設 mode="auto"：能送就送，送不了就等最舊那塊
bus.show(mode="skip")            # 🔥 顯示優先：上一幀還在跑就丟棄這幀、立即返回、不擋 Python
bus.show(mode="block")           # 應用優先：一定等到送完才返回（= 舊 neopixel.write() 語意）
bus.show(mode="latest")          # 覆蓋優先：允許覆蓋尚未送出的那塊（最省 RAM，可能撕裂）
```

| mode | 顯示停頓 | Python 被擋 | 撕裂 | 適用 |
|---|---|---|---|---|
| `"auto"` | 無 | 只等最舊那塊 | 無 | 通用 |
| **`"skip"`** | **無** | **完全不擋** | 無 | 🔥 **「非中斷地不斷顯示」的主模式** |
| `"block"` | 無 | 擋到送完 | 無 | 相容舊行為 |
| `"latest"` | 無 | 完全不擋 | ⚠️ 有 | 單緩衝、極致省 RAM |

**`mode="skip"` 的語意**：

```
show(mode="skip"):
    if 上一幀還在傳輸:
        dropped += 1          # 記統計，不報錯
        return None           # 立即返回
    else:
        編碼 → 入隊 → return trans_id
```

→ **顯示永遠以硬體上限跑，應用算多快就寫多快，兩者完全解耦。** 掉幀可觀測（`stats()['dropped']`）但不出錯。

#### 緩衝區所有權：什麼時候可以安全寫 `.buf`

| mode | 緩衝區數 | Python 何時可寫 |
|---|---|---|
| `"auto"` / `"block"` / `"skip"` | 2 | **隨時**（顯示讀舊塊，Python 寫新塊） |
| `"latest"` | 1 | 隨時可寫，但顯示正在讀時會**撕裂** |

> **建議預設**：雙緩衝 + `mode="skip"` —— 唯一同時滿足「不擋 Python」「不撕裂」「不浪費」的組合。

#### 完全自主的顯示（`daemon` 模式，P2 選配）

```python
bus.start(interval_ms=0)   # 0 = free-running：DMA 送完立刻接下一個，零 Python 介入
...                        # 應用只管寫 bus.buf
bus.stop()
```

實作 = 在 C 層 DMA 完成回呼裡直接鏈下一個 transfer。先用 `show(mode="skip")` 把顯式模型定清楚，daemon 之後再加（API 不衝突）。

> **`show()` 的完成語意**（與 `lcd_bus` 一致）：
> ```
> 編碼 → 入隊 → 回傳 trans_id（mode="skip" 且忙碌時回 None）
> 佇列滿的行為由 mode 決定（auto=等最舊 / skip=丟棄 / block=等送完 / latest=覆蓋）
> show(wait=True)：入隊後等這筆完成（舊語意，供相容用）
> ```
> 純 Python 後端沒有真非同步，所以 `mode` 一律退化為 `"block"`；`stats()['mode_effective']` 會回報實際生效的模式。

### 3.4 純 Python 後端的編碼策略（本輪的技術核心）

**目的**：在沒有 C 的階段，把「位元轉置」的成本壓到可接受，同時當作 C 版本的正確性參考。

#### 3.4.1 硬體後端的資料格式與緩衝區大小

**統一像素格式（你定的規格）**：每個 LED 一律 **4 bytes `[R, G, B, W]`**（見 `DESIGN.md` §1.2）。
所以下面一律以 **RGBW**（4 通道）計算緩衝區。

| 後端 | 每 LED bit | 每 RGBW LED | 666 顆（單／雙緩衝） | DMA 記憶體限制 |
|---|---|---|---|---|
| **I80**（S3） | 3 × 16-bit word | **96 bytes** | **62.4 / 124.9 KB** | ⚠️ **只能內部 SRAM** |
| **PARLIO Wave8**（P4） | 8 pulse × data_width | 40 bytes | 26.0 / 52.1 KB | PSRAM 可 |
| **PARLIO Wave3** | 3 tick × data_width | 12 bytes | 7.8 / 15.6 KB | PSRAM 可 |

> **✅ 已驗證（本輪最重要的成果）**：**緩衝區大小只與「每條燈帶幾顆」有關，與 lane 數無關。**
>
> 我讀 yves driver 的 `initled()` 配置：
> `8 * _nb_components * NUM_LED_PER_STRIP * 3 * 2` = `24 * bpp * q` bytes。
> 注意公式裡**沒有 `numstrips`**。因為 16 lane 共用**同一條 16-bit word 流**，
> 每個 word 的 bit i 就是 lane i（硬體同時廣播）。
>
> 所以「16 條燈帶」不會讓緩衝區變成 16 倍。666 顆 × RGBW 就是 62.4 KB。
>
> **例外**：RMT 每個通道有獨立 DMA buffer，那個才隨 lane 線性成長（所以 16 lane 用 RMT 會爆）。

#### 3.4.2 已逆向驗證的編碼器（`transpose16x1_noinline2`）

我用「逐 bit 對照組」對 yves 的實作做了 2000 組隨機輸入 × 8 bit 的等價驗證，**全部吻合**：

```
輸入：16 個 byte（每個 lane 一個 byte，來自該 lane 該像素的某個顏色分量）
輸出：24 bytes = 12 個 uint16 word（注意：12 個，不是 8 個也不是 24 個）

語意（已驗證）：
  對 bit j（j = 7..0，MSB first）：
      mask = 所有 lane 中「該 lane 的 bit j 為 1」的 OR 遮罩
              mask 的 bit i = lane i
      該 bit 佔用 3 個 uint16 word，內容 = [mask, 0, 0]
```

**驗證程式碼（可直接放進 `tests/`）**：

```python
M = 0xFFFFFFFF; AA = 0x00AA00AA; CC = 0x0000CCCC; FF = 0xF0F0F0F0; FF2 = 0x0F0F0F0F

def _pre(v):
    t = (v ^ (v >> 7)) & AA;   v = (v ^ t ^ (t << 7)) & M
    t = (v ^ (v >> 14)) & CC;  v = (v ^ t ^ (t << 14)) & M
    return v

def transpose16x1(A, B, off):
    """A: 16 bytes（lane 0..15）；把 24 bytes 寫進 B[off:off+24]"""
    y  = int.from_bytes(A[0:4],  'little')   # lanes 0-3
    x  = int.from_bytes(A[4:8],  'little')   # lanes 4-7
    y1 = int.from_bytes(A[8:12], 'little')   # lanes 8-11
    x1 = int.from_bytes(A[12:16],'little')   # lanes 12-15
    x = _pre(x); y = _pre(y); x1 = _pre(x1); y1 = _pre(y1)
    t = ((x & FF) | ((y >> 4) & FF2)) & M;  y  = (((x << 4) & FF) | (y & FF2)) & M;   x = t
    t = ((x1 & FF) | ((y1 >> 4) & FF2)) & M; y1 = (((x1 << 4) & FF) | (y1 & FF2)) & M; x1 = t
    ...  # 8 個 put()，照 yves 原文
```

#### 3.4.3 ⚠️ 尚未確認的關鍵細節（留給硬體驗證）

yves 的 `initled()` 有一行**預先填入哨兵值**：

```c
for (int i = 0; i < NUM_LED_PER_STRIP * _nb_components * 8; i++) {
    led_output[3 * i + 1] = 0xFFFF;      // 每個「3 word 群」的第 2 個 word
}
```

但我的驗證顯示轉置會**覆寫**第 2 個 word 為 `mask`，只有第 3 個 word 保持 0。
所以實際送到硬體的每 bit 波形是 `[mask, mask, 0]`，**這樣 0 碼與 1 碼的前兩段都是高電位，不像標準 WS2812 的 `0b100` / `0b110`**。

**三個可能性**（需要硬體或邏輯分析儀確認）：
1. 我對 word 與 PCLK 的對應理解有誤（16-bit 並列匯流排可能每個 word 吃 1 或 2 個 PCLK）
2. 哨兵值的 index 我讀錯了（`= 0xFFFF` 那行與我理解的 word 邊界不同）
3. 實際 clock 比我推的高（每個 word 只算 1 個 PCLK → 6 PCLK/bit）

**這件事不阻塞 API 設計**（API 只承諾「秀出來正確」），但**會阻塞 C 後端的正確實作**。列入 §9 的 R1。

#### 3.4.4 混色（lane 之間）才是真正的工作

不同 lane 的同一像素有不同顏色。用「已含 lane 遮罩」的 LUT 可以省掉逐次位移：

```
for pixel j in range(q):
    for lane i in range(nlanes):            # ← 這層是成本主因
        for 每個要寫的 component c:          # 由 write 遮罩決定（RGBW=4 / RGB=3 / W=1）
            words = LUT[c][ buf[i, j, c] ]   # 24 個 word 的 view
            for t in range(24):
                out[base + t] |= words[t] << i
```

**成本隨 `write` 遮罩變化**（這是 4-bytes 統一格式白送的好處）：

| 遮罩 | 查表次數（16 lane × 666 顆） | 逐 word OR+shift | viper 預估 |
|---|---|---|---|
| RGBW (15) | 42,624 | **1,022,976** | 200–500 ms |
| RGB (7) | 31,968 | 767,232 | 150–380 ms |
| 單色 W (8) | 10,656 | **255,744** | **50–128 ms** |

> **🔧 修正**：我先前寫「可達協定上限 166 fps」是**錯的**。
> **真正的問題不是 fps，是「驅動器能不能一直跑在硬體上限」。**
> 傳輸時間固定（`顆數 × 30 µs`），而編碼必須完全藏在傳輸背後；純 Python 的編碼量（16 lane × 666 顆 = 100 萬次 OR+shift）要 **200–500 ms**，遠超 20 ms 的傳輸時間 → **顯示會被編碼拖慢到硬體上限的 1/20 以下**，這就違反了「非中斷地不斷顯示」。
>
> **所以：純 Python 做不到「16 lane 自由跑」，這是數學問題不是調校問題。**
> 純 Python 版本的價值因此是：**定型 API + 驗證正確性 + 驗證硬體後端**，可在小規模（單 lane、≤666 顆）下真正 free-running。

#### 3.4.5 已知可大幅降成本的優化

| 優化 | 效果 | 難度 |
|---|---|---|
| **per-lane 展開表**（把 `<< i` 預先做進表）→ 消掉 100 萬次 shift | 省 1/3 | 低（表變大 24 KB/lane） |
| **髒像素追蹤**：只重編碼變化過的像素 | 靜態場景可省 90%+ | 中 |
| **髒 lane 追蹤**：`show(lanes=(0,))` | 只動 1 條時省 15/16 | 低 |
| **`write` 遮罩**：單色效果只查 W | **省 4 倍** | 低（已納入設計） |
| **改用 `array('I')` 一次處理 2 個 word** | 省一半迴圈 | 低 |
| **C 後端** | **200–500×** | 本專案第二階段 |

#### 3.4.6 性能預估總表（改用「顯示能否 free-running」判定）

判準：**編碼時間 vs 傳輸時間**。編碼能藏在傳輸背後 → 顯示 free-running；不能 → 顯示被編碼拖慢。

| 實作 | 編碼時間（16 lane × 666 顆） | vs 傳輸 20 ms | 顯示能否 free-running |
|---|---|---|---|
| 純 Python 逐 bit | >10 s | 500× | ❌ |
| **viper + 共享 LUT（RGBW）** | **200–500 ms** | 10–25× | ❌ 多 lane 不行 |
| viper + 共享 LUT（**單 lane**，666 顆） | 12–32 ms | ≈1–1.6× | ⚠️ 勉強；單 lane 是純 Python 的合理規模 |
| viper + `write=0b1000`（單色，16 lane） | 50–128 ms | 2.5–6× | ❌ |
| viper + 髒像素 / 髒 lane 追蹤 | 依場景 | 可 <1× | ✅ 靜態或局部動畫 |
| **C 後端（i80）** | **< 1 ms（目標）** | **1/20** | ✅ **完全 free-running** |

> **本輪（純 Python）的驗收標準**：
> ① 顏色正確 ② order 正確 ③ 時序正確 ④ `show(mode=...)` 語意正確 ⑤ 與 `PixelController` 接得上
> ⑥ **單 lane 666 顆下，顯示能 free-running**（編碼藏在 20 ms 傳輸背後）
>
> **多 lane 的 free-running 由 C 後端負責** —— 那才是這個驅動真正要解決的問題。

---

## 4. 高階 API：`led`（你的世界觀，保持不變）

### 4.1 最小使用（單條）

```python
import led

# 最簡：一個 pin、一個顆數
strip = led.WS2812(5, 60)
strip[0] = (255, 0, 0)        # neopixel 風格賦值（RGB）
strip.buf[3:6] = b"\x00\xff\x00"   # 或直接寫 bytes（GRB）
strip.show()

# 或 viper 風格
strip.buf16[0] = 2048         # 12-bit 亮度（單色燈帶）
strip.show()
```

### 4.2 你的實際場景（14 條燈帶）

```python
import led

# ── 主要用法：一條 bus、14 條燈帶 ──────────────────────────────
bus = led.WS2812(
    pins=(1, 2, 3, 4, 7, 8, 9, 10, 11, 12, 13, 16, 17, 18),
    q=(200, 10, 10, 10, 10, 100, 50, 50, 10, 10, 10, 10, 10, 10),
    order="GRB",
    brightness=4095,
)
# → 內部自動選後端：S3→"i80"、P4→"parlio"；沒有 C 模組時→"python"（viper）

# 寫資料（兩種風格都支援）
bus.fill((0, 0, 0))
bus.view(0)[:3] = b"\x00\xff\x00"        # 第 1 條第 1 顆 = 綠
bus.buf[1, 0, 0] = 0                     # 或座標式（如果在 3.8 以上的 mpy）
bus.views()[4][:6] = b"\xff\x00\x00\x00\x00\xff"

# 顯示
tid = bus.show()                          # 非阻塞
bus.wait_all()                            # 需要時才等

# 只要更新某一條
bus.show(lanes=(0,))
```

### 4.3 與 `mp_Net-Core` 的 `PixelController` 接軌

**這是設計的硬要求**：`bus.view(lane)` 回傳的東西必須能直接餵給現有 `PixelController`。

```python
# mp_Net-Core/slave/lib/sw/PixelController.py 的 _convert 完全不用改：
class PixelController:
    def __init__(self, pixel_type, pixel_io_cfg):
        self.hw = pixel_io_cfg['pixel_IO']   # ← 這裡現在傳 bus.view(lane) 的持有者

    @micropython.viper
    def _convert(self, source, offset: int, n: int, tid: int):
        dst = self.hw.buf                    # ← 現在指向 led_bus 的像素緩衝
        ...
    def st_show(self):
        self.hw.write()                      # ← 或 self.hw.show()
```

**相容層**：`led.WS2812(...)` 回傳的物件**同時**提供兩種介面：

| 舊介面（`neopixel`） | 新介面（`led_bus`） | 實作 |
|---|---|---|
| `.buf` | `.buf` | 同一個 `memoryview` ✅ |
| `.write()` | `.show()` | `write()` 是 `show()` 的別名 ✅ |
| `.n` | `.q` | 別名 ✅ |
| `__setitem__` / `__getitem__` | 同 | 同 ✅ |
| — | `.buf16` / `.views()` / `.wait_all()` | 新增 |

→ **`ws2812_drv.py` 只要把 `neopixel.NeoPixel(pin, q)` 換成 `led.WS2812(pin, q)`，其他一行不動。**

### 4.4 `LEDCommander`（沿用舊 API）

```python
from led import LEDCommander, init_led, init_rgb

led_list = init_led(config)      # 吃你現有的 config dict（向後相容）
rgb_list = init_rgb(config)

ledC = LEDCommander(led_list, rgb_list)
ledC.init_all()

ledC.run_Pattern(led_init, gap_Time=20, run_time=200, encoder=4095, debug=True)
ledC.show_all(3, bright=4095)
```

`LEDCommander` 內部改動（性能關鍵）：

```python
@micropython.native
def show_all(self, channel=3, bright=4095):
    # 舊版：逐 pixel 乘亮度 → 逐 controller show（序列）
    # 新版：設定一次全域亮度 → 一次 show（平行）
    for c in self.all_controllers:
        c.brightness = bright
    for b in self.buses:            # 通常是 1 個
        b.show()
```

**全域亮度不再逐 pixel 乘**：改成 C/viper 層在 `load16()` 內做 `(v * bri) >> 12`，或走 gamma LUT 的 index 偏移。

---

## 5. 緩衝格式：4 bytes 統一 vs `neopixel` 的 3 bytes

你定的規格是**每個 LED 一律 4 bytes `[R,G,B,W]`**，而 `neopixel.NeoPixel.buf` 是 **3 bytes/px（`bpp=3`，GRB 順序）**。這是一個**必須明確處理的差異**。

| | `neopixel`（舊） | 本專案（新） |
|---|---|---|
| 每 px | 3 bytes | **4 bytes** |
| 通道 | `buf[ORDER[i]]`，ORDER=(1,0,2,3) → GRB | **固定 R,G,B,W 順序** |
| 舊程式碼相容 | — | 需下面三條路之一 |

**三條相容路徑**：

| 路徑 | 做法 | 改動量 | 適用 |
|---|---|---|---|
| **① 轉接層（推薦）** | `led.neopixel_compat(pin, q)` 回傳一個 3-bytes 外觀、內部 stride-4 的物件；`PixelController` 完全不用改 | **0 行** | 舊程式碼直接搬 |
| ② 改 `PixelController._convert` | 把 `d_idx = i * bpp` 改成 `i * 4`，並寫 R/G/B 三個 offset | 改 3 行 | 你願意動 `mp_Net-Core` |
| ③ 直通模式 | 用 `load()` / `load16()` 餵 4-byte 資料，order 由 `led_bus` 處理 | 改呼叫端 | 新程式碼 |

> **⚠️ 陷阱**：**order 重排只能在一處發生**。① 走「`led_bus` 的 order 設 `"RGB"`（= raw 順序），由 `PixelController` 重排」；③ 走「`led_bus` 依 `order="GRBW"` 重排」。兩個都做 → 顏色 double-swap。

**建議**：預設走 ①，並在 `led` 套件裡提供 `init_led(config, compat=True)` 一鍵切換。

---

## 6. 後端選擇邏輯（`bus="auto"`）

```
bus="auto" 時：
  ├─ 有 C 模組 _led？
  │    ├─ 是 → 依晶片：
  │    │        S3 → "i80"（lane ≤ 16）
  │    │        P4 → "parlio"（lane ≤ 16/unit，>16 → 開第 2 unit）
  │    │        兩者都不可用（lane > 上限）→ 拆成多個 bus 實例
  │    └─ 否 → "python"（viper + LUT 後端，見 §3.4）
  └─ 指定 bus="i80" 但硬體不支援 → raise（不靜默降級）

顯式指定永遠優先：bus="rmt" / bus="i80" / bus="parlio" / bus="python"
```

**lane 數超過單一 bus 上限時的拆分**（你的 P4 情境）：

```python
# P4：2 個 PARLIO unit → 32 lane
bus = led.WS2812(pins=tuple(range(1, 33)), q=200, bus="parlio")
bus.bus_count()   # → 2（內部開了 2 個 unit）
bus.show()        # 兩個 unit 由 C 層協調同時啟動（零相位差）
```

---

## 7. 錯誤處理（照你的三套策略）

| 情境 | 策略 | 範例 |
|---|---|---|
| **設定問題**（lane 數不符、order 不合法、q 是 0） | 收集 warning、**不 raise**、`check_conflicts()` 回 list | 對齊 `effect_core.py` |
| **API 誤用**（`show()` 在 deinit 後、`wait()` 給錯 tid） | **raise**，英文訊息 | `raise RuntimeError("led_bus: bus deinitialized")` |
| **週邊 I/O**（i80 bus 建不起來、I2C NAK） | try/except + log，不中斷 init | 對齊 `pca9685.setup()` |

---

## 8. 本輪要你確認的 API 決策

### A1 — 模組命名
- **A（我的建議）**：`led_bus`（低階）+ `led`（高階）—— 與 `lcd_bus` 平行
- B：`_led`（C）+ `led`（Python）—— 你原本在 `DESIGN.md` 裡的草案
- C：全部叫 `led_driver`

### A2 — `show()` 的回傳值
- **A（建議）**：回 `trans_id`（跟 `lcd_bus.write()` 一樣）。不需要的人忽略即可
- B：回 `None`，要看狀態用 `is_busy()`

### A3 — `.buf` 的形狀
- **A（建議）**：`[lane][q][bpp]` 平鋪（1 條燈帶時等於 `neopixel` 的 layout）→ **舊程式碼零修改**
- B：`[q][lane][bpp]`（像素優先）→ cache 較好但破壞相容

### A4 — `show(lanes=...)` 要不要做
- **A（建議）**：要。你的場景 14 條燈帶常常只有幾條在動（髒通道最佳化）
- B：不要，永遠全部更新（簡單）

### A5 — 純 Python 後端的目標
- **A（建議）**：viper + 預算 LUT，目標 ~2.5 ms/幀（14×200），可與 DMA 重疊達協定上限
- B：只要會動就好（純 Python 逐 bit，~30 ms/幀），純當 API 驗證

### A6 — RGBW 的處理
- **✅ 已定案（你決定）**：**每個 LED 一律 4 bytes `[R,G,B,W]`**，不隨燈珠型別改變，理由是方便不同效果之間轉換
- 我的補充：編碼器用 **`write` 遮罩**決定讀哪幾個通道（`0b1111`/`0b0111`/`0b1000`），所以「統一格式」對編碼器是**零額外成本**，單色效果反而便宜 4 倍
- **待你確認**：預設遮罩要 `0b1111`（RGBW）還是 `0b0111`（RGB）？取決於你實際用的燈珠。

---

## 附錄：`mp_lcd_bus` 慣例對照表（實作時照抄）

| 項目 | `lcd_bus` 做法 | `led_bus` 沿用 |
|---|---|---|
| 建構子 | `mp_arg_parse_all_kw_array` + `MP_ARG_REQUIRED` / `MP_ARG_KW_ONLY` | ✅ |
| tuple 決定 lane 數 | `mp_obj_get_array` + 檢查長度 | ✅（1–16 且需為 2 的冪或任意值由硬體決定） |
| 未管理的腳位 | `-1` | ✅（`wr=None` 內部轉 `-1`） |
| 非同步寫入 | `write(buf) → trans_id` | ✅（`show() → trans_id`） |
| 狀態查詢 | `is_busy()` / `pending()` | ✅ |
| 等待 | `wait(tid, timeout_ms=-1)` / `wait_all()` | ✅ |
| 大 buffer 分塊 | `I80_MAX_CHUNK 32768`，自動切、等空槽、回第一個 tid | ✅（`chunk` 參數可覆寫） |
| queue 滿的語意 | 單筆 → **raise**；分塊 → **wait** | ✅ 同 |
| GC 防護 | `ref_bufs[slot] = obj` 一路保護到 `on_color_done` | ✅ **必抄** |
| 完成回呼 | `on_color_done` → 清 `pending[]`、`queue_head++`、`queue_count--` | ✅ |
| 硬體清理 | `s_last_i80_bus` / `i80_needs_cleanup` + `mp_hal_delay_ms(50)` | ✅（同晶片只能有一組 i80） |
| `mp_buffer_info_t` | `mp_get_buffer_raise(obj, &bufinfo, MP_BUFFER_READ)` | ✅ |
| 記憶體對齊 | `psram_trans_align = 64` / `sram_trans_align = 64` | ✅（你選內部 SRAM，用 `sram_trans_align`） |
| 資源保護 | — | 🆕 加 `heap_caps` 統計與 `_led.info()` |
