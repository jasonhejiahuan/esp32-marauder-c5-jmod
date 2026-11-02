#include "configs.h"
#if defined(MARAUDER_C5) && (defined(MARAUDER_WAVESHARE_C5_RGB) || defined(MARAUDER_XIAO_C5))
#include <esp_attr.h>
#include <esp_private/cache_utils.h>

// Board-scoped backport of ESP-IDF's early MSPI cache guard. The Arduino 3.3.6
// IDF libraries switch the shared flash/PSRAM clock without this guard. Keep
// both the wrapper and its callees in IRAM; do not log or allocate here.
// Build with --wrap=mspi_timing_enter_{low,high}_speed_mode. No SDK files change.
extern "C" void __real_mspi_timing_enter_low_speed_mode(bool control_spi1);
extern "C" void __real_mspi_timing_enter_high_speed_mode(bool control_spi1);

extern "C" void IRAM_ATTR __wrap_mspi_timing_enter_low_speed_mode(bool control_spi1) {
  uint32_t cache_state = 0;
  spi_flash_disable_cache(0, &cache_state);
  __real_mspi_timing_enter_low_speed_mode(control_spi1);
  spi_flash_restore_cache(0, cache_state);
}

extern "C" void IRAM_ATTR __wrap_mspi_timing_enter_high_speed_mode(bool control_spi1) {
  uint32_t cache_state = 0;
  spi_flash_disable_cache(0, &cache_state);
  __real_mspi_timing_enter_high_speed_mode(control_spi1);
  spi_flash_restore_cache(0, cache_state);
}
#endif
