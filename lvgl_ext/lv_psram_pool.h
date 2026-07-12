// Backs LVGL's builtin TLSF object pool from the PSRAM heap.
//
// Wired in via the top-level CMakeLists.txt as LVGL's LV_MEM_POOL_INCLUDE +
// LV_MEM_POOL_ALLOC hooks, so lv_mem_init() calls lv_psram_pool_alloc() once
// at boot to get LV_MEM_SIZE bytes of backing store instead of using a static
// internal-RAM array. See sdkconfig.defaults (CONFIG_LV_MEM_SIZE_KILOBYTES)
// and docs/debugging/wifi-nav-pool-exhaustion.md for why the pool moved to
// PSRAM. A static EXT_RAM_BSS_ATTR array won't link on this pre-rev-v3
// ESP32-P4 (--enable-non-contiguous-regions discards .sbss), hence this
// runtime allocation. Defined static inline so no extra component/translation
// unit is needed - it compiles straight into LVGL's lv_mem_core_builtin.c.
#pragma once

#include <stddef.h>

#include "esp_heap_caps.h"

static inline void *lv_psram_pool_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}
