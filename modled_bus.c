/*
 * modled_bus.c — mp_LEDdriver: ESP32 native multi-LED bus driver for MicroPython
 *
 * 目標:
 *   用 ESP-IDF 原生週邊驅動多種 LED 協定，重點是「非中斷地不斷讀取 RGBW
 *   緩衝然後顯示」。WS2812 用 LCD_CAM I80（16 lane 平行）/ PARLIO（P4）；
 *   APA102 用原生 SPI；PCA9685 用原生 I2C。PWM LED 用 MicroPython 原生
 *   machine.PWM（不在此模組）。
 *
 * 用法 (Python):
 *   import led_bus
 *   print(led_bus.version())
 *
 *   bus = led_bus.WS2812Bus(pins=(1,2,3,4), q=200)
 *   print(bus.info())        # 後端、lane 數、緩衝大小、實測 SRAM
 *   print(bus.stats())
 *   bus.buf[0:4] = b'\xff\x00\x00\x00'    # 4-byte RGBW 統一格式
 *
 * 注意:
 *   - 只在 ESP32 系有效（ESP-IDF 才有這些週邊）；其他 port 不會編譯本檔
 *     （micropython.cmake 有 ESP_PLATFORM guard）。
 *   - **本階段 (C0) 只建立物件與配置緩衝，尚未驅動硬體**：
 *     show() 會 raise NotImplementedError，直到 C1 接上 I80。
 *     這樣可以先量出真實的可用 SRAM，作為後續切片決策的依據。
 *   - 記憶體配置走 heap_caps_aligned_alloc（非 GC 管理），必須靠 __del__ 釋放。
 *     對齊 mp_heap_caps 的「軟重置會洩漏」警告 —— 本模組同理。
 *
 * 技術棧優先序（見 C_API.md §1.4.2）：
 *   1. ESP-IDF 原生驅動 > 2. MicroPython 原生 > 3. 自寫 C > 4. 不用 Arduino
 */

#include <stdio.h>
#include <string.h>

#include "py/mpconfig.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/objarray.h"
#include "py/binary.h"
#include "py/mphal.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"   /* esp_ptr_external_ram */
#include "esp_log.h"
#include "soc/soc_caps.h"
#else
/* 非 ESP32（Unix port）：只為了讓 MicroPython API 可測。
   硬體相關能力一律回報 0/unsupported。 */
#include <stdio.h>
#define ESP_LOGI(tag, ...) fprintf(stderr, "I [%s] " __VA_ARGS__), fprintf(stderr, "\n")
#endif

#include "led_alloc.h"

static const char *TAG = "led_bus";

#ifndef LED_BUS_VERSION
#define LED_BUS_VERSION "0.0.0-dev"
#endif

/* ══════════════════════════════════════════════════════════════════
 * 常數與幾何
 * ══════════════════════════════════════════════════════════════════ */

#define LED_MAX_LANES          16      /* S3/P4 的 I80 bus_width 上限 */
#define LED_MAX_QUEUE_DEPTH     8
#define LED_DEFAULT_QUEUE       2      /* C0 先用 2；C2 依實測 SRAM 調整 */
#define LED_DEFAULT_BRIGHTNESS  4095   /* 12-bit 全域亮度（見 §3.2） */

/* 統一像素格式：每個 LED 一律 4 bytes [R,G,B,W]（見 C_API.md §1.2） */
#define LED_BYTES_PER_LED       4

/* ══════════════════════════════════════════════════════════════════
 * Helper functions
 * ══════════════════════════════════════════════════════════════════ */

/* 解析 order 字串 → 位元遮罩 + 通道數 (bpp)
 *
 * order 只決定「硬體輸出順序」，不改變像素緩衝的 4-byte 格式。
 * 例： "GRBW" → bpp=4, mask=R|G|B|W；"GRB" → bpp=3。
 * 回傳 bpp；字元不合法或重複 → raise（這是 API 誤用，見三套錯誤策略）。
 */
static int led_parse_order(const char *order, uint8_t *mask_out)
{
    uint8_t mask = 0;
    int bpp = 0;
    for (const char *p = order; *p != '\0'; ++p) {
        uint8_t bit;
        switch (*p) {
            case 'R': case 'r': bit = 0x01; break;
            case 'G': case 'g': bit = 0x02; break;
            case 'B': case 'b': bit = 0x04; break;
            case 'W': case 'w': bit = 0x08; break;
            default:
                mp_raise_msg_varg(&mp_type_ValueError,
                    MP_ERROR_TEXT("led_bus: invalid character '%c' in order (use R/G/B/W)"), (int)*p);
        }
        if (mask & bit) {
            mp_raise_msg_varg(&mp_type_ValueError,
                MP_ERROR_TEXT("led_bus: duplicated character '%c' in order"), (int)*p);
        }
        mask |= bit;
        bpp++;
    }
    if (bpp < 3) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("led_bus: order needs at least 3 channels, got %d"), bpp);
    }
    *mask_out = mask;
    return bpp;
}

/* 解析 pins → lane 陣列。支援 int（單條）或 tuple/list（多條）。
   回傳 lane 數（1..LED_MAX_LANES）。 */
static int led_parse_pins(mp_obj_t pins_obj, int *pins_out)
{
    if (mp_obj_is_int(pins_obj)) {
        mp_int_t p = mp_obj_get_int(pins_obj);
        if (p < 0) {
            mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("led_bus: pin must be >= 0"));
        }
        pins_out[0] = (int)p;
        return 1;
    }

    size_t n = 0;
    mp_obj_t *items = NULL;
    if (mp_obj_is_type(pins_obj, &mp_type_tuple) || mp_obj_is_type(pins_obj, &mp_type_list)) {
        mp_obj_get_array(pins_obj, &n, &items);
    } else {
        mp_raise_msg(&mp_type_TypeError,
            MP_ERROR_TEXT("led_bus: pins must be an int or a tuple/list of ints"));
    }

    if (n == 0 || n > LED_MAX_LANES) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("led_bus: lane count must be 1-%d, got %u"),
            LED_MAX_LANES, (unsigned)n);
    }
    for (size_t i = 0; i < n; ++i) {
        mp_int_t p = mp_obj_get_int(items[i]);
        if (p < 0) {
            mp_raise_msg_varg(&mp_type_ValueError,
                MP_ERROR_TEXT("led_bus: pin[%u] must be >= 0"), (unsigned)i);
        }
        pins_out[i] = (int)p;
    }
    return (int)n;
}

/* 解析 q → 每 lane 顆數。支援 int（全部相同）或 tuple/list（各條不等長）。
   回傳 q_max（用於緩衝大小計算）；並把值填進 q_out。 */
static int led_parse_q(mp_obj_t q_obj, int lanes, int *q_out, int *uniform_out)
{
    if (mp_obj_is_int(q_obj)) {
        mp_int_t q = mp_obj_get_int(q_obj);
        if (q < 1) {
            mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("led_bus: q must be >= 1"));
        }
        for (int i = 0; i < lanes; ++i) {
            q_out[i] = (int)q;
        }
        if (uniform_out) {
            *uniform_out = 1;
        }
        return (int)q;
    }

    size_t n = 0;
    mp_obj_t *items = NULL;
    if (mp_obj_is_type(q_obj, &mp_type_tuple) || mp_obj_is_type(q_obj, &mp_type_list)) {
        mp_obj_get_array(q_obj, &n, &items);
    } else {
        mp_raise_msg(&mp_type_TypeError,
            MP_ERROR_TEXT("led_bus: q must be an int or a tuple/list of ints"));
    }
    if ((int)n != lanes) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("led_bus: q has %u entries but there are %d lanes"),
            (unsigned)n, lanes);
    }
    int q_max = 0;
    for (int i = 0; i < lanes; ++i) {
        mp_int_t q = mp_obj_get_int(items[i]);
        if (q < 1) {
            mp_raise_msg_varg(&mp_type_ValueError,
                MP_ERROR_TEXT("led_bus: q[%d] must be >= 1"), i);
        }
        q_out[i] = (int)q;
        if ((int)q > q_max) {
            q_max = (int)q;
        }
    }
    if (uniform_out) {
        *uniform_out = 0;
    }
    return q_max;
}

/* ══════════════════════════════════════════════════════════════════
 * WS2812Bus object
 * ══════════════════════════════════════════════════════════════════ */

typedef struct _mp_led_ws2812_bus_obj_t {
    mp_obj_base_t base;

    /* ── 硬體後端（C1 填入 I80 handle；C0 只記錄意圖）── */
    int             backend_kind;     /* 0 = 未定, 1 = i80, 2 = parlio, 3 = rmt */
    int             lane_count;
    int             data_pins[LED_MAX_LANES];
    int             wr_pin;

    /* ── 幾何 ── */
    int             q_max;
    int             q[LED_MAX_LANES];
    int             q_uniform;
    int             bpp;              /* 通道數 = len(order) */
    uint8_t         order_mask;
    int             order_len;
    char            order_str[8];

    /* ── 顯示參數 ── */
    int             write_mask;       /* 通道遮罩（C5/C6 用） */
    int             brightness;       /* 0-4095 */
    int             neutral;
    int             overclock_x1000;  /* 1.0 → 1000 */

    /* ── 切片（C0 先算出數量，C2 實作）── */
    int             slices;
    size_t          slice_bytes;

    /* ── 緩衝（非 GC 管理）── */
    led_alloc_t     pix[2];           /* 像素：PSRAM 優先 */
    int             pix_front, pix_back;
    led_alloc_t     dma[LED_MAX_QUEUE_DEPTH];  /* 編碼：內部 SRAM，硬限制 */
    int             queue_depth;

    /* ── 統計 ── */
    uint32_t        frames;
    uint32_t        dropped;
    uint32_t        encode_us;
    uint32_t        dma_us;
    uint32_t        blocked_us;

    bool            pix_in_psram;
    bool            initialized;
} mp_led_ws2812_bus_obj_t;

extern const mp_obj_type_t mp_led_ws2812_bus_type;

/* ── 釋放所有非 GC 記憶體 ───────────────────────────────────────── */

static void led_ws2812_release(mp_led_ws2812_bus_obj_t *self)
{
    for (int i = 0; i < 2; ++i) {
        led_alloc_free(&self->pix[i]);
    }
    for (int i = 0; i < LED_MAX_QUEUE_DEPTH; ++i) {
        led_alloc_free(&self->dma[i]);
    }
    self->initialized = false;
}

static mp_obj_t led_ws2812_deinit(mp_obj_t self_in)
{
    led_ws2812_release((mp_led_ws2812_bus_obj_t *)self_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(led_ws2812_deinit_obj, led_ws2812_deinit);

/* ── 建構子 ─────────────────────────────────────────────────────── */

static mp_obj_t led_ws2812_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    (void)type;   /* mp_obj_malloc_with_finaliser 已帶型別 */
    enum {
        ARG_pins, ARG_q,
        ARG_order, ARG_backend, ARG_wr, ARG_write,
        ARG_queue_depth, ARG_neutral, ARG_brightness, ARG_overclock,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pins,       MP_ARG_OBJ | MP_ARG_REQUIRED,      {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_q,          MP_ARG_OBJ | MP_ARG_REQUIRED,      {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_order,      MP_ARG_OBJ | MP_ARG_KW_ONLY,       {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_backend,    MP_ARG_OBJ | MP_ARG_KW_ONLY,       {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_wr,         MP_ARG_INT | MP_ARG_KW_ONLY,       {.u_int = -1} },
        { MP_QSTR_write,      MP_ARG_INT | MP_ARG_KW_ONLY,       {.u_int = 0x0F} },
        { MP_QSTR_queue_depth, MP_ARG_INT | MP_ARG_KW_ONLY,      {.u_int = LED_DEFAULT_QUEUE} },
        { MP_QSTR_neutral,    MP_ARG_INT | MP_ARG_KW_ONLY,       {.u_int = 0} },
        { MP_QSTR_brightness, MP_ARG_INT | MP_ARG_KW_ONLY,       {.u_int = LED_DEFAULT_BRIGHTNESS} },
        { MP_QSTR_overclock,  MP_ARG_OBJ | MP_ARG_KW_ONLY,       {.u_obj = MP_OBJ_NULL} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    mp_led_ws2812_bus_obj_t *self = mp_obj_malloc_with_finaliser(mp_led_ws2812_bus_obj_t, &mp_led_ws2812_bus_type);

    /* 先把所有指標歸零 —— 任何後續 raise 都會走 __del__，必須安全 */
    memset(self->pix, 0, sizeof(self->pix));
    memset(self->dma, 0, sizeof(self->dma));
    self->frames = self->dropped = 0;
    self->encode_us = self->dma_us = self->blocked_us = 0;
    self->pix_front = 0;
    self->pix_back = 1;
    self->backend_kind = 0;
    self->initialized = false;
    self->pix_in_psram = false;

    /* ── 幾何 ── */
    self->lane_count = led_parse_pins(parsed[ARG_pins].u_obj, self->data_pins);
    self->q_max = led_parse_q(parsed[ARG_q].u_obj, self->lane_count, self->q, &self->q_uniform);

    const char *order = "GRBW";
    if (parsed[ARG_order].u_obj != MP_OBJ_NULL && parsed[ARG_order].u_obj != mp_const_none) {
        order = mp_obj_str_get_str(parsed[ARG_order].u_obj);
    }
    self->order_len = (int)strlen(order);
    if (self->order_len >= (int)sizeof(self->order_str)) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("led_bus: order string too long"));
    }
    memcpy(self->order_str, order, self->order_len + 1);
    self->bpp = led_parse_order(order, &self->order_mask);

    /* ── 顯示參數（設定範圍檢查：不合法 raise，可容忍的 clamp + 警告）── */
    self->write_mask = (int)parsed[ARG_write].u_int & 0x0F;
    if (self->write_mask == 0) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("led_bus: write mask must be non-zero"));
    }

    mp_int_t bri = parsed[ARG_brightness].u_int;
    if (bri < 0) {
        bri = 0;
    }
    if (bri > 4095) {
        bri = 4095;
        mp_printf(&mp_plat_print, "led_bus: brightness clamped to 4095\n");
    }
    self->brightness = (int)bri;
    self->neutral = (int)parsed[ARG_neutral].u_int & 0xFF;

    self->overclock_x1000 = 1000;
    if (parsed[ARG_overclock].u_obj != MP_OBJ_NULL && parsed[ARG_overclock].u_obj != mp_const_none) {
        /* 用整數千分比表示，避免浮點（對齊 PixelMathMethod 的「無浮點」原則） */
        mp_float_t oc = mp_obj_get_float(parsed[ARG_overclock].u_obj);
        mp_int_t ocm = (mp_int_t)(oc * 1000.0f + 0.5f);
        if (ocm < 500 || ocm > 2000) {
            mp_printf(&mp_plat_print, "led_bus: overclock out of range [0.5, 2.0], using 1.0\n");
            ocm = 1000;
        }
        self->overclock_x1000 = (int)ocm;
    }

    self->wr_pin = (int)parsed[ARG_wr].u_int;

    /* ── 後端選擇意圖（C1 才真的建硬體）── */
    const char *be = "auto";
    if (parsed[ARG_backend].u_obj != MP_OBJ_NULL && parsed[ARG_backend].u_obj != mp_const_none) {
        be = mp_obj_str_get_str(parsed[ARG_backend].u_obj);
    }
#if defined(ESP_PLATFORM) && defined(SOC_PARLIO_SUPPORTED) && SOC_PARLIO_SUPPORTED
    self->backend_kind = 2;   /* P4 預設 PARLIO */
#elif defined(ESP_PLATFORM) && defined(SOC_LCD_I80_SUPPORTED) && SOC_LCD_I80_SUPPORTED
    self->backend_kind = 1;   /* S3 預設 I80 */
#elif defined(ESP_PLATFORM)
    self->backend_kind = 3;   /* RMT */
#else
    self->backend_kind = 0;   /* unset（Unix port 測試） */
#endif
    if (strcmp(be, "auto") != 0) {
        if (strcmp(be, "i80") == 0) {
            self->backend_kind = 1;
        } else if (strcmp(be, "parlio") == 0) {
            self->backend_kind = 2;
        } else if (strcmp(be, "rmt") == 0) {
            self->backend_kind = 3;
        } else {
            mp_raise_msg_varg(&mp_type_ValueError,
                MP_ERROR_TEXT("led_bus: unknown backend '%s' (auto/i80/parlio/rmt)"), be);
        }
    }

    /* ── queue_depth ── */
    mp_int_t qd = parsed[ARG_queue_depth].u_int;
    if (qd < 1 || qd > LED_MAX_QUEUE_DEPTH) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("led_bus: queue_depth must be 1-%d"), LED_MAX_QUEUE_DEPTH);
    }
    self->queue_depth = (int)qd;

    /* ══════════════════════════════════════════════════════════════
     * 記憶體配置
     * ══════════════════════════════════════════════════════════════ */

    /* 像素緩衝：2 塊（雙層雙緩衝的上層，見 §7.6），PSRAM 優先 */
    size_t pix_size = led_pix_size(self->q_max, self->lane_count);
    for (int i = 0; i < 2; ++i) {
        self->pix[i] = led_alloc_pix(pix_size);
        if (self->pix[i].ptr == NULL) {
            led_ws2812_release(self);
            mp_raise_msg_varg(&mp_type_MemoryError,
                MP_ERROR_TEXT("led_bus: cannot allocate pixel buffer #%d (%u bytes x2); free SRAM/PSRAM first"),
                i, (unsigned)pix_size);
        }
    }
    /* 判斷是否真的落在 PSRAM（用 esp_ptr_external_ram 最準） */
#if defined(ESP_PLATFORM)
    self->pix_in_psram = esp_ptr_external_ram(self->pix[0].ptr);
#endif

    /* DMA 編碼緩衝：queue_depth 塊，**內部 SRAM 硬限制** */
    size_t dma_size = led_dma_size_ws2812(self->q_max, self->bpp);
    for (int i = 0; i < self->queue_depth; ++i) {
        self->dma[i] = led_alloc_dma(dma_size);
        if (self->dma[i].ptr == NULL) {
            led_ws2812_release(self);
#if defined(ESP_PLATFORM)
            mp_raise_msg_varg(&mp_type_MemoryError,
                MP_ERROR_TEXT("led_bus: DMA buffer #%d needs %u bytes of INTERNAL SRAM "
                              "(largest free = %u). Try queue_depth=1, or a smaller q."),
                i, (unsigned)dma_size,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
#else
            mp_raise_msg_varg(&mp_type_MemoryError,
                MP_ERROR_TEXT("led_bus: DMA buffer #%d needs %u bytes (Unix port: host malloc)"),
                i, (unsigned)dma_size);
#endif
        }
    }

    /* ── 切片規劃（C0 只算，C2 實作；公式見 §7.8）── */
    /* slice_bytes = min(I80 單筆上限, budget // 2)，slices = ceil(frame / slice_bytes) */
    const size_t I80_SLICE_MAX = 32768;
    size_t frame = dma_size;
    if (frame <= I80_SLICE_MAX) {
        self->slices = 1;
        self->slice_bytes = frame;
    } else {
        size_t sb = I80_SLICE_MAX;
        self->slices = (int)((frame + sb - 1) / sb);
        self->slice_bytes = sb;
    }

    self->initialized = true;

    ESP_LOGI(TAG, "WS2812Bus: %d lane(s), q_max=%d, bpp=%d, pix=%u B x2, dma=%u B x%d",
             self->lane_count, self->q_max, self->bpp,
             (unsigned)pix_size, (unsigned)dma_size, self->queue_depth);
    return MP_OBJ_FROM_PTR(self);
}

/* ── 緩衝存取：writable memoryview（給 viper ptr8/ptr16 用）──────── */

static mp_obj_t led_ws2812_get_buf(mp_obj_t self_in)
{
    mp_led_ws2812_bus_obj_t *self = (mp_led_ws2812_bus_obj_t *)self_in;
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("led_bus: bus deinitialized"));
    }
    /* 回傳「目前離屏」那塊的 view —— C 決定是哪一塊，上層不需知道 index
       （照 DSIBus.back_buffer() 的設計意圖，見 §0.1） */
    mp_obj_array_t *view = MP_OBJ_TO_PTR(
        mp_obj_new_memoryview(BYTEARRAY_TYPECODE, self->pix[self->pix_back].size,
                              self->pix[self->pix_back].ptr));
    view->typecode |= 0x80;   /* 標記為可寫（照 mp_heap_caps 的手法） */
    return MP_OBJ_FROM_PTR(view);
}
static MP_DEFINE_CONST_FUN_OBJ_1(led_ws2812_get_buf_obj, led_ws2812_get_buf);

static mp_obj_t led_ws2812_pixel_buffer(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_self, ARG_idx };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_idx,  MP_ARG_INT | MP_ARG_KW_ONLY,  {.u_int = -1} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    mp_led_ws2812_bus_obj_t *self = (mp_led_ws2812_bus_obj_t *)parsed[ARG_self].u_obj;
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("led_bus: bus deinitialized"));
    }
    mp_int_t idx = parsed[ARG_idx].u_int;
    int slot;
    if (idx < 0) {
        slot = self->pix_back;          /* 目前離屏那塊 */
    } else if (idx == 0 || idx == 1) {
        slot = (int)idx;                /* 明確指定（照 test_dsi.py 的用法） */
    } else {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("led_bus: pixel buffer index must be 0 or 1, got %d"), (int)idx);
    }
    mp_obj_array_t *view = MP_OBJ_TO_PTR(
        mp_obj_new_memoryview(BYTEARRAY_TYPECODE, self->pix[slot].size, self->pix[slot].ptr));
    view->typecode |= 0x80;
    return MP_OBJ_FROM_PTR(view);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_ws2812_pixel_buffer_obj, 1, led_ws2812_pixel_buffer);

/* 單條燈帶在目前離屏緩衝的 view（給 PixelController 用） */
static mp_obj_t led_ws2812_view(mp_obj_t self_in, mp_obj_t lane_in)
{
    mp_led_ws2812_bus_obj_t *self = (mp_led_ws2812_bus_obj_t *)self_in;
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("led_bus: bus deinitialized"));
    }
    mp_int_t lane = mp_obj_get_int(lane_in);
    if (lane < 0 || lane >= self->lane_count) {
        mp_raise_msg_varg(&mp_type_IndexError,
            MP_ERROR_TEXT("led_bus: lane %d out of range (0-%d)"), (int)lane, self->lane_count - 1);
    }
    size_t stride = (size_t)self->q_max * LED_BYTES_PER_LED;
    uint8_t *base = (uint8_t *)self->pix[self->pix_back].ptr + stride * (size_t)lane;
    size_t len = (size_t)self->q[(int)lane] * LED_BYTES_PER_LED;
    mp_obj_array_t *view = MP_OBJ_TO_PTR(mp_obj_new_memoryview(BYTEARRAY_TYPECODE, len, base));
    view->typecode |= 0x80;
    return MP_OBJ_FROM_PTR(view);
}
static MP_DEFINE_CONST_FUN_OBJ_2(led_ws2812_view_obj, led_ws2812_view);

/* ── show()：C0 尚未接硬體 ──────────────────────────────────────── */

static mp_obj_t led_ws2812_show(mp_obj_t self_in)
{
    mp_led_ws2812_bus_obj_t *self = (mp_led_ws2812_bus_obj_t *)self_in;
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("led_bus: bus deinitialized"));
    }
    /* C1 會在這裡：編碼 → 入隊 → 翻頁 → 非同步送出。
       現階段明確報錯，避免使用者以為燈會亮。 */
    mp_raise_msg(&mp_type_NotImplementedError,
        MP_ERROR_TEXT("led_bus: hardware backend not wired yet (C0 stage). "
                      "Buffers are allocated; use info()/stats() to inspect."));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(led_ws2812_show_obj, led_ws2812_show);

/* ── info() / stats() ──────────────────────────────────────────── */

static mp_obj_t led_ws2812_info(mp_obj_t self_in)
{
    mp_led_ws2812_bus_obj_t *self = (mp_led_ws2812_bus_obj_t *)self_in;

    const char *backend =
        self->backend_kind == 1 ? "i80" :
        self->backend_kind == 2 ? "parlio" :
        self->backend_kind == 3 ? "rmt" : "unset";

    mp_obj_t d = mp_obj_new_dict(20);
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_version), mp_obj_new_str(LED_BUS_VERSION, strlen(LED_BUS_VERSION)));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_backend), mp_obj_new_str(backend, strlen(backend)));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_lanes), mp_obj_new_int(self->lane_count));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_q_max), mp_obj_new_int(self->q_max));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_q_uniform), mp_obj_new_bool(self->q_uniform != 0));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_bpp), mp_obj_new_int(self->bpp));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_order), mp_obj_new_str(self->order_str, self->order_len));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_write_mask), mp_obj_new_int(self->write_mask));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_brightness), mp_obj_new_int(self->brightness));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_queue_depth), mp_obj_new_int(self->queue_depth));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_slices), mp_obj_new_int(self->slices));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_slice_bytes), mp_obj_new_int((mp_int_t)self->slice_bytes));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_pix_bytes), mp_obj_new_int((mp_int_t)self->pix[0].size));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_pix_in_psram), mp_obj_new_bool(self->pix_in_psram));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_dma_bytes), mp_obj_new_int((mp_int_t)self->dma[0].size));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_sram_used),
                      mp_obj_new_int((mp_int_t)(self->dma[0].size * (size_t)self->queue_depth)));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_initialized), mp_obj_new_bool(self->initialized));

#if defined(ESP_PLATFORM)
    /* 實測記憶體 —— 這是 C0 階段最重要的產出 */
    mp_obj_t m = mp_obj_new_dict(6);
    mp_obj_dict_store(m, MP_ROM_QSTR(MP_QSTR_dma_largest),
        mp_obj_new_int((mp_int_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
    mp_obj_dict_store(m, MP_ROM_QSTR(MP_QSTR_dma_free),
        mp_obj_new_int((mp_int_t)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
    mp_obj_dict_store(m, MP_ROM_QSTR(MP_QSTR_internal_largest),
        mp_obj_new_int((mp_int_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    mp_obj_dict_store(m, MP_ROM_QSTR(MP_QSTR_internal_free),
        mp_obj_new_int((mp_int_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    mp_obj_dict_store(m, MP_ROM_QSTR(MP_QSTR_spiram_largest),
        mp_obj_new_int((mp_int_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    mp_obj_dict_store(m, MP_ROM_QSTR(MP_QSTR_spiram_free),
        mp_obj_new_int((mp_int_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_mem), m);
#endif
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(led_ws2812_info_obj, led_ws2812_info);

static mp_obj_t led_ws2812_stats(mp_obj_t self_in)
{
    mp_led_ws2812_bus_obj_t *self = (mp_led_ws2812_bus_obj_t *)self_in;
    mp_obj_t d = mp_obj_new_dict(8);
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_frames), mp_obj_new_int(self->frames));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_dropped), mp_obj_new_int(self->dropped));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_encode_us), mp_obj_new_int(self->encode_us));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_dma_us), mp_obj_new_int(self->dma_us));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_blocked_us), mp_obj_new_int(self->blocked_us));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_leds_per_sec), mp_obj_new_int(0));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(led_ws2812_stats_obj, led_ws2812_stats);

/* ── property 存取器 ────────────────────────────────────────────── */




/* ── property：用 MicroPython 的 C 型別標準範式（attr slot）──────────
 *
 * 照 py/objcomplex.c 的 complex_attr 範式（見 MicroPython 官方 C 型別寫法）：
 *   dest[0] != MP_OBJ_NULL  → 這是 store（寫入），dest[0] 是待寫入值
 *   dest[0] == MP_OBJ_NULL  → 這是 load（讀取），把結果放進 dest[0]
 * C 型別的 attribute 不走 mp_obj_property_t（那只能從 Python class 建立），
 * 而是要提供 attr slot。
 */
static void led_ws2812_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    mp_led_ws2812_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (attr == MP_QSTR_brightness) {
        if (dest[0] == MP_OBJ_NULL) {
            dest[0] = mp_obj_new_int(self->brightness);       /* load */
        } else {
            mp_int_t v = mp_obj_get_int(dest[1]);
            if (v < 0) {
                v = 0;
            }
            if (v > 4095) {
                v = 4095;
            }
            self->brightness = (int)v;
            dest[0] = MP_OBJ_NULL;                            /* store 成功 */
        }
    } else if (attr == MP_QSTR_write_mask) {
        if (dest[0] == MP_OBJ_NULL) {
            dest[0] = mp_obj_new_int(self->write_mask);
        } else {
            mp_int_t v = mp_obj_get_int(dest[1]) & 0x0F;
            if (v == 0) {
                mp_raise_msg(&mp_type_ValueError,
                    MP_ERROR_TEXT("led_bus: write mask must be non-zero"));
            }
            self->write_mask = (int)v;
            dest[0] = MP_OBJ_NULL;
        }
    } else if (attr == MP_QSTR_neutral) {
        if (dest[0] == MP_OBJ_NULL) {
            dest[0] = mp_obj_new_int(self->neutral);
        } else {
            mp_int_t v = mp_obj_get_int(dest[1]);
            if (v < 0) {
                v = 0;
            }
            if (v > 255) {
                v = 255;
            }
            self->neutral = (int)v;
            dest[0] = MP_OBJ_NULL;
        }
    } else if (attr == MP_QSTR_lanes) {
        dest[0] = mp_obj_new_int(self->lane_count);
    } else if (attr == MP_QSTR_q_max) {
        dest[0] = mp_obj_new_int(self->q_max);
    } else if (attr == MP_QSTR_bpp) {
        dest[0] = mp_obj_new_int(self->bpp);
    }
    /* 其他 attr 交給 locals_dict（方法）處理 —— MicroPython 會自動 fallback */
}

/* ── 方法表 ─────────────────────────────────────────────────────── */

static const mp_rom_map_elem_t led_ws2812_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_buf),           MP_ROM_PTR(&led_ws2812_get_buf_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel_buffer),  MP_ROM_PTR(&led_ws2812_pixel_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_back_buffer),   MP_ROM_PTR(&led_ws2812_get_buf_obj) },
    { MP_ROM_QSTR(MP_QSTR_view),          MP_ROM_PTR(&led_ws2812_view_obj) },
    { MP_ROM_QSTR(MP_QSTR_show),          MP_ROM_PTR(&led_ws2812_show_obj) },
    { MP_ROM_QSTR(MP_QSTR_present),       MP_ROM_PTR(&led_ws2812_show_obj) },
    { MP_ROM_QSTR(MP_QSTR_info),          MP_ROM_PTR(&led_ws2812_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats),         MP_ROM_PTR(&led_ws2812_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),        MP_ROM_PTR(&led_ws2812_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),       MP_ROM_PTR(&led_ws2812_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(led_ws2812_locals_dict, led_ws2812_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mp_led_ws2812_bus_type,
    MP_QSTR_WS2812Bus,
    MP_TYPE_FLAG_NONE,
    make_new, led_ws2812_make_new,
    attr, led_ws2812_attr,
    locals_dict, &led_ws2812_locals_dict
);

/* ══════════════════════════════════════════════════════════════════
 * 模組層級
 * ══════════════════════════════════════════════════════════════════ */

static mp_obj_t led_bus_version(void)
{
    static const char v[] = LED_BUS_VERSION;
    return mp_obj_new_str(v, strlen(v));
}
static MP_DEFINE_CONST_FUN_OBJ_0(led_bus_version_obj, led_bus_version);

/* led_bus.mem_report() —— C0 的核心工具：
 * 印出/回傳各 heap 區域的實測可用量，作為切片與 queue_depth 決策依據。 */
static mp_obj_t led_bus_mem_report(void)
{
    mp_obj_t d = mp_obj_new_dict(12);
#if defined(ESP_PLATFORM)
    struct { const char *name; uint32_t caps; } zones[] = {
        { "dma_internal", MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL },
        { "internal",     MALLOC_CAP_INTERNAL },
        { "spiram",       MALLOC_CAP_SPIRAM },
        { "spiram_dma",   MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA },
        { "default",      MALLOC_CAP_DEFAULT },
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(zones); ++i) {
        mp_obj_t z = mp_obj_new_dict(3);
        mp_obj_dict_store(z, MP_ROM_QSTR(MP_QSTR_free),
                          mp_obj_new_int((mp_int_t)heap_caps_get_free_size(zones[i].caps)));
        mp_obj_dict_store(z, MP_ROM_QSTR(MP_QSTR_largest),
                          mp_obj_new_int((mp_int_t)heap_caps_get_largest_free_block(zones[i].caps)));
        mp_obj_dict_store(z, MP_ROM_QSTR(MP_QSTR_total),
                          mp_obj_new_int((mp_int_t)heap_caps_get_total_size(zones[i].caps)));
        mp_obj_dict_store(d, mp_obj_new_str(zones[i].name, strlen(zones[i].name)), z);
    }
#else
    {
        static const char msg[] =
            "mem_report needs ESP_PLATFORM (Unix port uses host malloc, no heap_caps)";
        mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_error),
                          mp_obj_new_str(msg, sizeof(msg) - 1));
    }
#endif
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(led_bus_mem_report_obj, led_bus_mem_report);

static const mp_rom_map_elem_t led_bus_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR_led_bus) },
    { MP_ROM_QSTR(MP_QSTR_WS2812Bus),   MP_ROM_PTR(&mp_led_ws2812_bus_type) },
    { MP_ROM_QSTR(MP_QSTR_version),     MP_ROM_PTR(&led_bus_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_mem_report),  MP_ROM_PTR(&led_bus_mem_report_obj) },
};
static MP_DEFINE_CONST_DICT(led_bus_module_globals, led_bus_module_globals_table);

const mp_obj_module_t mp_module_led_bus = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&led_bus_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_led_bus, mp_module_led_bus);
