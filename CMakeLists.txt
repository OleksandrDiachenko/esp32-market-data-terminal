# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Firmware version embedded in esp_app_desc_t.version, compared against
# GitHub release tags by Phase 10's OTA client - see
# docs/decisions/0006-ota-firmware-update.md.
set(PROJECT_VER_FILE ${CMAKE_CURRENT_LIST_DIR}/version.txt)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Back LVGL's object pool (CONFIG_LV_MEM_SIZE_KILOBYTES) from the PSRAM heap at
# boot instead of a static internal-RAM array. The 128KB internal pool was
# exhausted by a large Settings > Wi-Fi list (~2KB/network) on top of the
# dashboard, which crashed/froze the LVGL task (see sdkconfig.defaults and
# docs/debugging/wifi-nav-pool-exhaustion.md). A static PSRAM pool
# (EXT_RAM_BSS_ATTR) won't link on this pre-rev-v3 ESP32-P4
# (--enable-non-contiguous-regions discards .sbss), so LVGL's builtin TLSF is
# pointed at a runtime PSRAM allocation via its LV_MEM_POOL_ALLOC hook
# (lv_psram_pool_alloc, a static inline in lvgl_ext/lv_psram_pool.h). Set as
# global build properties (before project()) so the managed lvgl component
# sees the header and the hook. Both defines are object-like on purpose - CMake
# silently drops function-like (parenthesized) preprocessor definitions.
idf_build_set_property(INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/lvgl_ext" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_POOL_INCLUDE=\"lv_psram_pool.h\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_POOL_ALLOC=lv_psram_pool_alloc" APPEND)

project(esp32-market-data-terminal)
