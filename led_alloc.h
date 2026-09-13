/*
 * led_alloc.h — mp_LEDdriver：記憶體配置策略（單一入口）
 *
 * 目標:
 *   把「哪一種緩衝該放哪一種記憶體」這個決策收斂到唯一一個檔案。
 *   對齊 C_API.md §2 與 §6 的定案：
 *     - 像素緩衝 (pix)  → PSRAM 優先（大、頻寬需求只 2.1 MB/s）
 *     - DMA 編碼緩衝 (dma) → 內部 SRAM（S3 的硬限制，見 §6.1）
 *
 * 用法 (Python): 無 —— 本檔只被 C 內部使用
 *
 * 注意:
 *   - 這些配置**不受 MicroPython GC 管理**，必須在 __del__ 明確釋放。
 *     對齊 mp_heap_caps 的「軟重置會洩漏」警告，見 led_bus_deinit()。
 *   - 64-byte 對齊：I80 DMA 的 sram_trans_align / psram_trans_align 需求。
 */

#ifndef _MP_LED_BUS_LED_ALLOC_H_
#define _MP_LED_BUS_LED_ALLOC_H_

#include <stddef.h>
#include <stdbool.h>

/* 對齊需求：I80 的 sram_trans_align = 64、psram_trans_align = 64
   （見 mp_lcd_bus 的 i80_bus.c 與 yves driver 的 LCD_DRIVER_PSRAM_DATA_ALIGNMENT） */
#define LED_ALLOC_ALIGN 64

typedef struct {
    void  *ptr;
    size_t size;
    bool   is_dma;      /* true = MALLOC_CAP_DMA|INTERNAL；false = PSRAM 或一般 */
} led_alloc_t;

/* 像素緩衝：PSRAM 優先，fallback 內部 SRAM。
   回傳 .ptr == NULL 表示配置失敗。 */
led_alloc_t led_alloc_pix(size_t size);

/* DMA 編碼緩衝：**只允許內部 SRAM 且必須 DMA 可達**。
   S3 上這是硬限制（PSRAM 會與 flash 搶 SPI bus 而打斷 WS2812 時序）。 */
led_alloc_t led_alloc_dma(size_t size);

/* 釋放（ptr == NULL 時 no-op） */
void led_alloc_free(led_alloc_t *a);

/* ── 緩衝大小計算（照 C_API.md §6.3 / §7.8 的公式） ────────────────── */

/* 單一 lane 的像素緩衝：q * 4 bytes（統一 RGBW 4-byte 格式） */
static inline size_t led_pix_size(int q_max, int lanes) {
    return (size_t)q_max * 4u * (size_t)lanes;
}

/* I80 編碼緩衝（WS2812）：q * bpp * 24 bytes
   由來：每 bit 3 個 16-bit word = 6 bytes，每 LED 24 bit，
         乘上通道數 bpp（RGBW = 4）→ 24 * bpp * q。
         **與 lane 數無關**（16 lane 共用同一條 16-bit word 流，見 §6.3）。 */
static inline size_t led_dma_size_ws2812(int q_max, int bpp) {
    return (size_t)q_max * (size_t)bpp * 24u;
}

#endif /* _MP_LED_BUS_LED_ALLOC_H_ */
