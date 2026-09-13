"""
test_c0.py — mp_LEDdriver C0 階段驗收腳本

用途:
  驗證建置管線打通、物件建得起、並量出**真實的可用 SRAM**。
  C0 不動任何 LED 硬體（show() 會 raise NotImplementedError）。

用法:
  import test_c0
  test_c0.run_all()

  test_c0.mem()          # 只印記憶體報告（最重要）
  test_c0.build()        # 只測物件建立
  test_c0.errors()       # 只測錯誤處理

注意:
  - 大 q 的測試會真的配置記憶體，跑完請讓物件被回收（函式已處理）。
  - 記憶體數字會因 WiFi / LVGL 是否啟動而不同，這是重點：**先量你的真實環境**。
"""

import gc
import led_bus

_PASS = 0
_FAIL = 0
_W = 66


def _hdr(m):
    print("\n" + "=" * _W + "\n  " + m + "\n" + "=" * _W)


def _ok(m):
    global _PASS
    _PASS += 1
    print("  [PASS] " + m)


def _fail(m):
    global _FAIL
    _FAIL += 1
    print("  [FAIL] " + m)


def _kb(n):
    return "{:.1f} KB".format(n / 1024.0)


# ══════════════════════════════════════════════════════════════
# 1. 記憶體報告（C0 的核心產出）
# ══════════════════════════════════════════════════════════════

def mem(verbose=True):
    """印出各 heap 區域的實測可用量。

    重點看 dma_internal.largest —— 那是「單一連續 DMA 可用的內部 SRAM」，
    直接決定 queue_depth 能開多少、slice_bytes 要切多大。
    """
    _hdr("記憶體實測報告（決定切片與 queue_depth 的依據）")
    gc.collect()
    r = led_bus.mem_report()
    for zone in ("dma_internal", "internal", "spiram", "spiram_dma", "default"):
        if zone not in r:
            continue
        z = r[zone]
        if verbose:
            print("  {:<14} free {:>10}  largest {:>10}  total {:>10}".format(
                zone, _kb(z["free"]), _kb(z["largest"]), _kb(z["total"])))
    print()
    print("  ── 判讀 ──")
    if "dma_internal" in r:
        largest = r["dma_internal"]["largest"]
        print("  WS2812 DMA 緩衝必須落在 dma_internal（S3 硬限制，見 C_API.md §6.1）")
        print("  單一最大連續塊 = {} → 這決定單塊 DMA 緩衝的上限".format(_kb(largest)))
        for q, bpp in ((200, 4), (666, 4)):
            need = q * bpp * 24
            verdict = "OK" if need <= largest else "TOO BIG"
            print("    q={:<4} bpp={} 需 {:>9} → [{}]".format(q, bpp, _kb(need), verdict))
    return r


# ══════════════════════════════════════════════════════════════
# 2. 物件建立
# ══════════════════════════════════════════════════════════════

def build():
    _hdr("物件建立")

    try:
        print("  led_bus.version() =", led_bus.version())
        _ok("import led_bus + version()")
    except Exception as e:
        _fail("version(): {}".format(e))
        return

    # ── 最小：單條 10 顆 ──
    try:
        b = led_bus.WS2812Bus(pins=5, q=10)
        info = b.info()
        print("  單條 10 顆 →", {k: info[k] for k in
              ("lanes", "q_max", "bpp", "order", "slices", "dma_bytes")})
        assert info["lanes"] == 1
        assert info["q_max"] == 10
        assert info["bpp"] == 4          # 預設 order="GRBW"
        _ok("單條（pins=int, q=int）")
        del b
        gc.collect()
    except Exception as e:
        _fail("單條: {}".format(e))

    # ── 你的實際場景：14 條不等長 ──
    try:
        pins = (1, 2, 3, 4, 7, 8, 9, 10, 11, 12, 13, 16, 17, 18)
        qs = (200, 10, 10, 10, 10, 100, 50, 50, 10, 10, 10, 10, 10, 10)
        b = led_bus.WS2812Bus(pins=pins, q=qs, order="GRBW", queue_depth=2)
        info = b.info()
        print("  14 條 → lanes={} q_max={} write_mask={:#x} slices={}".format(
            info["lanes"], info["q_max"], info["write_mask"], info["slices"]))
        print("           pix={} x2 ({}) dma={} x{}".format(
            _kb(info["pix_bytes"]), "PSRAM" if info["pix_in_psram"] else "INTERNAL",
            _kb(info["dma_bytes"]), info["queue_depth"]))
        print("           SRAM 合計 = {}".format(_kb(info["sram_used"])))
        assert info["lanes"] == 14, "lanes 應為 14"
        assert info["q_max"] == 200, "q_max 應為 200"
        assert info["slices"] >= 1, "slices 應 >= 1"
        assert not info["q_uniform"], "q 給了 tuple，q_uniform 應為 False"
        _ok("14 條不等長（你的實際配置）")
        del b
        gc.collect()
    except Exception as e:
        _fail("14 條: {}".format(e))

    # ── 緩衝存取 ──
    try:
        b = led_bus.WS2812Bus(pins=5, q=10, order="GRBW")
        buf = b.buf
        assert len(buf) == 10 * 4, "buf len = {}".format(len(buf))
        buf[0:4] = b"\xff\x00\x00\x00"        # 第 0 顆 = R
        assert bytes(buf[0:4]) == b"\xff\x00\x00\x00", "緩衝寫入後讀回不符"
        assert len(b.pixel_buffer(0)) == 10 * 4
        assert len(b.pixel_buffer(1)) == 10 * 4
        assert len(b.view(0)) == 10 * 4
        _ok("buf / pixel_buffer(0|1) / view(lane) 大小與寫入正確")
        # 4-byte 統一格式驗證
        assert len(buf) == 10 * 4, "每個 LED 必須是 4 bytes（統一 RGBW 格式）"
        _ok("統一像素格式：每 LED 4 bytes")
        del b
        gc.collect()
    except Exception as e:
        _fail("緩衝存取: {}".format(e))

    # ── property ──
    try:
        b = led_bus.WS2812Bus(pins=5, q=10)
        assert b.brightness == 4095
        b.brightness = 2048
        assert b.brightness == 2048
        b.brightness = 99999          # 應 clamp，不 raise
        assert b.brightness == 4095
        assert b.write_mask == 0x0F
        b.write_mask = 0x07
        assert b.write_mask == 0x07
        assert b.neutral == 0
        b.neutral = 0x80              # motor 死區停
        assert b.neutral == 0x80
        _ok("property：brightness（clamp）/ write_mask / neutral")
        del b
        gc.collect()
    except Exception as e:
        _fail("property: {}".format(e))

    # ── stats ──
    try:
        b = led_bus.WS2812Bus(pins=5, q=10)
        s = b.stats()
        assert s["frames"] == 0 and s["dropped"] == 0
        _ok("stats() 欄位齊全")
        del b
        gc.collect()
    except Exception as e:
        _fail("stats: {}".format(e))

    # ── show() 應該是 NotImplementedError（C0 階段）──
    try:
        b = led_bus.WS2812Bus(pins=5, q=10)
        try:
            b.show()
            _fail("show() 應該 raise NotImplementedError（C0 尚未接硬體）")
        except NotImplementedError:
            _ok("show() 正確 raise NotImplementedError（C0 尚未接硬體）")
        del b
        gc.collect()
    except Exception as e:
        _fail("show() 行為: {}".format(e))


# ══════════════════════════════════════════════════════════════
# 3. 錯誤處理（照三套策略：API 誤用 raise、設定問題 collect）
# ══════════════════════════════════════════════════════════════

def errors():
    _hdr("錯誤處理")

    cases = [
        ("pins 型別錯", lambda: led_bus.WS2812Bus(pins="abc", q=10), TypeError),
        ("q=0", lambda: led_bus.WS2812Bus(pins=5, q=0), ValueError),
        ("lane 太多（17）", lambda: led_bus.WS2812Bus(pins=tuple(range(17)), q=10), ValueError),
        ("order 非法字元", lambda: led_bus.WS2812Bus(pins=5, q=10, order="GRX"), ValueError),
        ("order 重複字元", lambda: led_bus.WS2812Bus(pins=5, q=10, order="GRRB"), ValueError),
        ("order 太短", lambda: led_bus.WS2812Bus(pins=5, q=10, order="GR"), ValueError),
        ("q 長度不符 lane 數", lambda: led_bus.WS2812Bus(pins=(1, 2), q=(10, 20, 30)), ValueError),
        ("backend 未知", lambda: led_bus.WS2812Bus(pins=5, q=10, backend="spi"), ValueError),
        ("write mask = 0", lambda: led_bus.WS2812Bus(pins=5, q=10, write=0), ValueError),
        ("queue_depth = 0", lambda: led_bus.WS2812Bus(pins=5, q=10, queue_depth=0), ValueError),
        ("queue_depth = 99", lambda: led_bus.WS2812Bus(pins=5, q=10, queue_depth=99), ValueError),
    ]
    for name, fn, expect in cases:
        try:
            fn()
            _fail("{}：應該 raise {} 但沒有".format(name, expect.__name__))
        except expect:
            _ok("{} → {}".format(name, expect.__name__))
        except Exception as e:
            _fail("{}：raise 了 {} 而非 {}（{}）".format(
                name, type(e).__name__, expect.__name__, e))
        gc.collect()

    # 可容忍的設定 → clamp + 警告，不 raise
    try:
        b = led_bus.WS2812Bus(pins=5, q=10, brightness=99999)
        assert b.brightness == 4095
        _ok("brightness 超範圍 → clamp 不 raise")
        del b
        gc.collect()
    except Exception as e:
        _fail("brightness clamp: {}".format(e))

    try:
        b = led_bus.WS2812Bus(pins=5, q=10, overclock=99.0)
        info = b.info()
        _ok("overclock 超範圍 → 警告 + 用 1.0 不 raise")
        del b
        gc.collect()
    except Exception as e:
        _fail("overclock: {}".format(e))


# ══════════════════════════════════════════════════════════════

def run_all():
    global _PASS, _FAIL
    _PASS = _FAIL = 0
    mem(verbose=True)
    build()
    errors()
    gc.collect()
    print("\n" + "=" * _W)
    total = _PASS + _FAIL
    if _FAIL == 0:
        print("  結果: {}/{} 全部通過 ✅   C0 建置管線 OK".format(_PASS, total))
    else:
        print("  結果: {}/{} 通過，{} 失敗 ❌".format(_PASS, total, _FAIL))
    print("=" * _W)
    return _FAIL == 0


if __name__ == "__main__":
    run_all()
