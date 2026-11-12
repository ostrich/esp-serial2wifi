#ifndef SERIAL2WIFI_FLASH_LAYOUT_H
#define SERIAL2WIFI_FLASH_LAYOUT_H
#include <stdint.h>
#include <stdbool.h>

#include "generated_flash_layout.h"

typedef enum {
  FLASH_REGION_APP1,
  FLASH_REGION_APP2,
  FLASH_REGION_CONFIG,
  FLASH_REGION_PAGES,
  FLASH_REGION_UI1,
  FLASH_REGION_UI2
} FlashRegion;

bool flashLayoutRegister(void);
uint32_t flashPhysicalSize(void);
bool flashRegionContains(FlashRegion region, uint32_t address, uint32_t length);
bool flashRegionErase(FlashRegion region, uint32_t address);
bool flashRegionWrite(FlashRegion region, uint32_t address, const void *data, uint32_t length);

#endif
