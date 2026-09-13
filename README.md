# mp_LEDdriver

MicroPython C user module（`led_bus`）：ESP32 原生多 LED 總線驅動，重點是**非中斷地不斷讀取 RGBW 緩衝然後顯示**。

- **WS2812 / SK6812** → **LCD_CAM I80**（S3/P4，16 lane 平行）／ **PARLIO**（P4）
- **APA102 / SK9822** → **原生 SPI**
- **PCA9685** → **原生 I2C**
- **PWM LED / 電機 / 伺服** → **MicroPython 原生 `machine.PWM`**（不需 C 模組）

> **設計討論稿在 [`C_API.md`](C_API.md)**（架構、緩衝、切片、中斷管理、實作階段）。
> 動工前先讀那份 —— 本檔案只講「怎麼用」與「目前進度」。

---

## 技術棧優先序

| 優先 | 來源 | 應用 |
|---|---|---|
| **1** | **ESP-IDF 原生驅動** | `esp_lcd` I80 · `esp_driver_parlio` · `esp_driver_spi` · `esp_driver_i2c` · `esp_mm` |
| **2** | **MicroPython 原生** | `machine.PWM` · `viper` · `heap_caps` |
| **3** | **自己的 C** | 只有「IDF 沒有、MicroPython 也沒有」才寫：WS2812 波形編碼 + 切片 + `led_mgr` |
| **4** | Arduino / 第三方 lib | ❌ **不用** |

---

## 目前進度

| 階段 | 內容 | 狀態 |
|---|---|---|
| **C0** | 建置管線 + 物件建立 + 記憶體配置 + `info()`/`stats()`/`mem_report()` | ✅ **完成（本版）** |
| C1 | WS2812 單條會亮（驗證編碼器波形） | ⬜ |
| C2 | 16 lane + 雙層雙緩衝 + `show()` 接受語意 + 切片 | ⬜ |
| C3 | `led_mgr` 統一 LED 中斷管理器 | ⬜ |
| C4 | 滿載壓力測試（決定切片策略） | ⬜ |
| C5 | APA102（原生 SPI） | ⬜ |
| C6 | PCA9685（原生 I2C） | ⬜ |
| C7 | `led/` Python 風格層（接軌 `mp_Net-Core`） | ⬜ |

**C0 的產出**：`led_bus.WS2812Bus` 可以建立、可以配置緩衝、可以量出**真實的可用 SRAM**。
`show()` 尚未接硬體，會 raise `NotImplementedError`（明確不讓使用者誤以為燈會亮）。

---

## 建置

### 掛到 `mp_Make-Tools`

`git_config.json`：
```json
"mp_LEDdriver": {
  "dir": "ext_mod/mp_LEDdriver",
  "url": "https://github.com/itdogwowo/mp_LEDdriver.git",
  "ref": "main",
  "recursive": true
}
```

`make_config.json`：
```json
"exmod": {
  "root": "ext_mod",
  "list": [
    "/mp_jpeg/micropython.cmake",
    "/mp_heap_caps/micropython.cmake",
    "/mp_LEDdriver/micropython.cmake"
  ]
}
```

目標晶片：`esp32s3`（`BOARD=ESP32_GENERIC_S3`）與 `esp32p4`（`BOARD=ESP32_GENERIC_P4`）。

### 手動（USER_C_MODULES）

```bash
make -C ports/esp32 BOARD=ESP32_GENERIC_S3 \
     USER_C_MODULES=/path/to/mp_LEDdriver/micropython.cmake
```

> **前置條件**：`ext_mod/mp_LEDdriver` 必須存在（Make-Tools 的 `exmod.root` 是 `ext_mod`）。
> 開發時可先做 symlink：
> ```bash
> ln -sfn /path/to/mp_LEDdriver mp_Make-Tools/ext_mod/mp_LEDdriver
> ```

---

## 使用（C0 階段）

```python
import led_bus

print(led_bus.version())        # '0.1.0-C0'

# ── 最重要：量出真實的可用記憶體（決定 queue_depth 與切片）──
r = led_bus.mem_report()
print(r['dma_internal']['largest'])    # 單一最大連續 DMA 內部 SRAM

# ── 建立 ──
bus = led_bus.WS2812Bus(
    pins=(1,2,3,4,7,8,9,10,11,12,13,16,17,18),   # 14 條
    q=(200,10,10,10,10,100,50,50,10,10,10,10,10,10),
    order="GRBW",
    queue_depth=2,
)
print(bus.info())

# ── 緩衝（統一格式：每 LED 4 bytes [R,G,B,W]）──
bus.buf[0:4] = b'\xff\x00\x00\x00'      # 第 0 條第 0 顆 = R
bus.view(0)[0:4] = b'\x00\xff\x00\x00'  # 單條燈帶的 view
bus.pixel_buffer(1)                     # 明確取第 1 塊（照 test_dsi.py 用法）

# ── 顯示（C0 尚未接硬體）──
# bus.show()   → NotImplementedError
```

---

## 驗收腳本

複製 `tests/test_c0.py` 到裝置，然後：

```python
import test_c0
test_c0.run_all()      # 全部
test_c0.mem()          # 只印記憶體報告
```

**重點觀察**：`dma_internal.largest` —— 那是「單一最大連續可 DMA 的內部 SRAM」，
直接決定 `queue_depth` 能開多少、`slice_bytes` 要切多大。

> ⚠️ 請在**你的真實環境**下量（WiFi 開/關、LVGL 開/關各量一次）——
> 這個數字是 C2 之後所有緩衝決策的依據。

---

## 模擬器調查（QEMU）—— 結論：**不能用來測 LED**

你提到「找一找有沒有模擬器能夠直接使用 bin」。我查完了，結論很明確。

### Espressif 官方 QEMU fork

Espressif 維護一個 QEMU fork，**有 ESP32-S3 支援，也提供 macOS arm64 預編版本**：

```bash
brew install libgcrypt glib pixman sdl2 libslirp     # macOS 依賴
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa qemu-riscv32
idf.py qemu monitor                                   # 只用於 IDF 專案
```

### ❌ 但它沒有建模任何 LED 相關週邊

我讀了 fork 的原始碼（`hw/xtensa/esp32s3.c`，1002 行），實作清單是：

| 已建模 | 未建模（讀回 0、寫入丟棄） |
|---|---|
| CPU、DRAM/IRAM、UART、GPIO、INTC、TIMG、RTC_CNTL、SYSTEM | **LCD_CAM / I80** ← WS2812 的核心 |
| SPI flash（含 PSRAM 初始化）、eFuse、SHA/AES/HMAC/DS、RNG | **RMT**（有註冊但只是 unimp stub） |
| GDMA（DMA 控制器本體）、TWAI、SD | **PARLIO · LEDC · I2S · SPI master · I2C** |

未建模區域的行為（`esp32s3_io_read/write`）：

```c
static uint64_t esp32s3_io_read(...)  { return 0; }          /* 靜默回 0 */
static void esp32s3_io_write(...)     { /* 靜默丟棄 */ }
```

**所以：**

| 想測什麼 | QEMU 能做嗎 |
|---|---|
| WS2812 波形時序、16 lane 平行、`show()` 真的送出 | ❌ **完全不行** |
| APA102（SPI）· PCA9685（I2C） | ❌ **完全不行** |
| C 模組載入、參數解析、`info()`/`stats()`、memoryview、attr slot、錯誤處理 | ✅ 可以 |
| `heap_caps` 配置與真實 SRAM 限制 | ✅ 可以（含 PSRAM 模型） |

> **殘酷的事實**：WS2812 時序**永遠只能在真實硬體上驗證**。
> 這是為什麼 C1 階段的驗收標準是「**邏輯分析儀量一條燈帶的實際波形**」—— 沒有替代品。

### ✅ 實際可行的驗證策略（三層）

| 層 | 工具 | 驗證什麼 | 狀態 |
|---|---|---|---|
| **1** | **`make.py esp32s3` 交叉編譯** | C 碼能編進 MicroPython 韌體 | ✅ 本專案採用 |
| **2** | **Unix port 編譯 + 執行** | Python 面 API 語意（不需硬體） | ⚠️ 有佈局限制，見下 |
| **3** | **實機 + 邏輯分析儀** | 波形時序、多 lane、fps | 🔴 **不可替代** |

**第 2 層的佈局限制（實測學到的）**：MicroPython 的 **make 式**建置（Unix port）用
`$(USER_C_MODULES)/*/micropython.mk` 收集模組，且**要求 C 原始碼在模組子目錄第一層**
（`patsubst $(USER_C_MODULES)/%.c` 才能正確剝離前綴）。而 **cmake 式**建置（ESP32 port / Make-Tools）
要的是 `ext_mod/mp_<name>/micropython.cmake`。**兩者對佈局的要求不同**，本專案以 ESP32 為主，
所以採 cmake 佈局（＝ `mp_lcd_bus` 的佈局）。

**第 1 層的價值**：CI 可自動化（照 `mp_jpeg` 的 `.github/workflows/ESP32.yml`），
matrix = `esp32s3` × `esp32p4`，涵蓋條件編譯（`SOC_LCD_I80_SUPPORTED` vs `SOC_PARLIO_SUPPORTED`）。

---

## API 速覽

| 物件 | 用途 | 狀態 |
|---|---|---|
| `led_bus.WS2812Bus` | WS2812 多 lane | C0：建構 + 緩衝 |
| `led_bus.APA102` | APA102（原生 SPI） | C5 |
| `led_bus.PCA9685` | PCA9685（原生 I2C） | C6 |
| `led_bus.mem_report()` | 各 heap 區域實測 | ✅ |
| `led_bus.version()` | 版本字串 | ✅ |

`WS2812Bus` 方法／屬性：

| 名稱 | 說明 |
|---|---|
| `bus.buf` | 目前離屏的像素緩衝（`memoryview`，可寫，`lane × q × 4` bytes） |
| `bus.pixel_buffer(idx=None)` | 明確取第 0/1 塊；`None` = 離屏那塊 |
| `bus.back_buffer()` | `buf` 的別名（照 `mp_lcd_bus` 的 `DSIBus` 命名） |
| `bus.view(lane)` | 單條燈帶的 view（給 `PixelController`） |
| `bus.show()` / `bus.present()` | 編碼 + 送出（C1 起可用） |
| `bus.info()` / `bus.stats()` | 後端資訊／統計 |
| `bus.brightness` | 全域亮度 0–4095（12-bit，不掉精度） |
| `bus.write_mask` | 通道遮罩 `0b1111`/`0b0111`/`0b1000` |
| `bus.neutral` | dStay 中性值（停止/熄燈時回填） |
| `bus.deinit()` / `__del__` | 釋放非 GC 管理的緩衝 |

---

## 已知限制（C0）

- **`show()` 尚未接硬體** —— C1 才會真的點亮。
- 建構子會**真的配置記憶體**；`q` 太大會得到 `MemoryError`
  （訊息含需要的 bytes 與當下最大可用塊，方便調整）。
- 非 GC 管理的緩衝靠 `__del__` 釋放。**軟重置（Ctrl-D）不會自動回收** ——
  與 `mp_heap_caps` 同理，必要時在 `boot.py` 呼叫 `gc.collect()` 或重建。
- 3.7.0 尚未支援 MicroPython 的 `q_uniform` 最佳化（僅回報旗標）。

---

## 檔案佈局

```
mp_LEDdriver/                  ← C user module 在 repo 根（＝ mp_lcd_bus 的佈局）
├── C_API.md                   ← 設計討論稿（架構/緩衝/切片/中斷/階段）
├── micropython.cmake          ← 建置入口（Python 名：led_bus）
├── micropython.mk
├── modled_bus.c               ← 模組註冊 + WS2812Bus
├── led_alloc.c/.h             ← 記憶體配置策略（pix→PSRAM，dma→內部 SRAM）
├── (C1 起：led_mgr / led_frame / led_slice / led_ws_i80 / led_wave / ...)
├── led/                       ← Python 風格層（C7）
├── tests/
│   └── test_c0.py             ← C0 驗收腳本
└── unix/                      ← Unix port 測試專案（無硬體時跑邏輯測試）
```

> **⚠️ 佈局限制（實測學到的）**：`micropython.cmake` 與 C 原始碼**必須在 repo 根目錄**，
> 不能放在子目錄（例如 `mp_led_bus/`）。因為 `mp_Make-Tools` 的 `exmod.root` + `exmod.list`
> 是「根目錄 + 以斜線開頭的路徑」組合而成，而 API 慣例是 `ext_mod/mp_<name>/micropython.cmake`。
> 放在子目錄會讓 `exmod not found`。**這是 `mp_lcd_bus` 的實際佈局，照它做。**

---

## 授權

MIT
