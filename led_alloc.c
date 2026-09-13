/*
 * led_alloc.c — mp_LEDdriver：記憶體配置策略實作
 *
 * 目標:
 *   提供兩個語意明確的配置器，讓 C_API.md §6 的「哪種緩衝放哪種記憶體」
 *   決策變成程式碼，而不是散落在各後端裡。
 *
 * 用法 (Python): 無（內部使用）
 *
 * 注意:
 *   - ESP32：全部使用 heap_caps_aligned_alloc + 明確 free，**不經 GC**。
 *     呼叫端必須保證 __del__ 會釋放（見 led_bus_deinit）。
 *   - 非 ESP32（例如 Unix port 測試）：退回 malloc/free，語意相同但量不到
 *     真實 SRAM —— 這是刻意的，讓 C 模組的 Python API 能在 PC 上驗證。
 *   - aligned_alloc 不保證 zero-init；需要清零請自行 memset。
 *     本檔刻意不自動清零 —— 呼叫端才知道哪些緩衝需要（例如 I80 的
 *     哨兵值 0xFFFF 必須在清零後才填，見 C_API.md §3.4.2 的註記）。
 */

#include "led_alloc.h"

#include <stdlib.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "led_bus";
#else
#include <stdio.h>
#define ESP_LOGW(tag, ...) fprintf(stderr, "W [%s] " __VA_ARGS__), fprintf(stderr, "\n")
#define ESP_LOGE(tag, ...) fprintf(stderr, "E [%s] " __VA_ARGS__), fprintf(stderr, "\n")
static const char *TAG = "led_bus";
#endif

led_alloc_t led_alloc_pix(size_t size)
{
    led_alloc_t a = { .ptr = NULL, .size = size, .is_dma = false };
    if (size == 0) {
        return a;
    }

#if defined(ESP_PLATFORM)
    /* 像素緩衝：PSRAM 優先（大、頻寬需求低），fallback 內部 SRAM。
       MALLOC_CAP_8BIT 必要 —— PSRAM 配置一定要帶，否則拿不到。 */
    a.ptr = heap_caps_aligned_alloc(LED_ALLOC_ALIGN, size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (a.ptr == NULL) {
        a.ptr = heap_caps_aligned_alloc(LED_ALLOC_ALIGN, size,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (a.ptr != NULL) {
            ESP_LOGW(TAG, "pix buffer fell back to INTERNAL SRAM (%u bytes)", (unsigned)size);
        }
    }
    if (a.ptr == NULL) {
        ESP_LOGE(TAG, "pix alloc failed: %u bytes (PSRAM+INTERNAL both exhausted)", (unsigned)size);
    }
#else
    /* 非 ESP32：退回 malloc。測試用途。 */
    a.ptr = malloc(size);
#endif
    return a;
}

led_alloc_t led_alloc_dma(size_t size)
{
    led_alloc_t a = { .ptr = NULL, .size = size, .is_dma = true };
    if (size == 0) {
        return a;
    }

#if defined(ESP_PLATFORM)
    /* DMA 編碼緩衝：**內部 SRAM 為唯一正解**。
       理由（C_API.md §6.1）：
         - WS2812 時序零容忍，PSRAM 與 flash 共用 SPI bus 會造成 DMA 餓死
         - S3 的內部 SRAM 不經 data cache → 零 cache 維護成本
       只加 MALLOC_CAP_DMA（不帶 SPIRAM），確保一定落在內部。 */
    a.ptr = heap_caps_aligned_alloc(LED_ALLOC_ALIGN, size,
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (a.ptr == NULL) {
        ESP_LOGE(TAG, "dma alloc failed: %u bytes (need INTERNAL+DMA, largest free = %u)",
                 (unsigned)size,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
#else
    a.ptr = malloc(size);
#endif
    return a;
}

void led_alloc_free(led_alloc_t *a)
{
    if (a != NULL && a->ptr != NULL) {
#if defined(ESP_PLATFORM)
        heap_caps_free(a->ptr);
#else
        free(a->ptr);
#endif
        a->ptr = NULL;
        a->size = 0;
    }
}
