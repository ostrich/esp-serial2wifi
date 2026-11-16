#include <esp8266.h>
#include "flash_layout.h"

static const partition_item_t partitions[] = {
    {SYSTEM_PARTITION_BOOTLOADER, FLASH_BOOT_START, FLASH_BOOT_SIZE},
    {SYSTEM_PARTITION_OTA_1, FLASH_APP1_START, FLASH_APP1_SIZE},
#if FLASH_LAYOUT_OTA_CAPABLE
    {SYSTEM_PARTITION_OTA_2, FLASH_APP2_START, FLASH_APP2_SIZE},
#endif
    {SYSTEM_PARTITION_RF_CAL, FLASH_RF_START, FLASH_RF_SIZE},
    {SYSTEM_PARTITION_PHY_DATA, FLASH_PHY_START, FLASH_PHY_SIZE},
    {SYSTEM_PARTITION_SYSTEM_PARAMETER, FLASH_PARAMS_START, FLASH_PARAMS_SIZE},
    {(partition_type_t)(SYSTEM_PARTITION_CUSTOMER_BEGIN + 1), FLASH_CONFIG_START,
     FLASH_CONFIG_SIZE},
    {(partition_type_t)(SYSTEM_PARTITION_CUSTOMER_BEGIN + 2), FLASH_PAGES_START, FLASH_PAGES_SIZE},
#if FLASH_EXTERNAL_UI
    {(partition_type_t)(SYSTEM_PARTITION_CUSTOMER_BEGIN + 3), FLASH_UI1_START, FLASH_UI1_SIZE},
    {(partition_type_t)(SYSTEM_PARTITION_CUSTOMER_BEGIN + 4), FLASH_UI2_START, FLASH_UI2_SIZE},
#endif
};

uint32_t ICACHE_FLASH_ATTR flashPhysicalSize(void) {
  uint8_t capacity = (spi_flash_get_id() >> 16) & 0xff;
  return capacity >= 19 && capacity <= 22 ? (1U << capacity) : 0;
}

bool ICACHE_FLASH_ATTR flashLayoutRegister(void) {
  if (system_get_flash_size_map() != FLASH_LAYOUT_MAP || flashPhysicalSize() != FLASH_LAYOUT_BYTES)
    return false;
  return system_partition_table_regist(partitions, sizeof(partitions) / sizeof(partitions[0]),
                                       FLASH_LAYOUT_MAP);
}

bool ICACHE_FLASH_ATTR flashRegionContains(FlashRegion region, uint32_t address, uint32_t length) {
  uint32_t start, size;
  if (system_get_flash_size_map() != FLASH_LAYOUT_MAP ||
      flashPhysicalSize() != FLASH_LAYOUT_BYTES || length == 0)
    return false;
  switch (region) {
  case FLASH_REGION_APP1:
    start = FLASH_APP1_START;
    size = FLASH_APP1_SIZE;
    break;
  case FLASH_REGION_APP2:
    start = FLASH_APP2_START;
    size = FLASH_APP2_SIZE;
    break;
  case FLASH_REGION_CONFIG:
    start = FLASH_CONFIG_START;
    size = FLASH_CONFIG_SIZE;
    break;
  case FLASH_REGION_PAGES:
    start = FLASH_PAGES_START;
    size = FLASH_PAGES_SIZE;
    break;
  case FLASH_REGION_UI1:
    start = FLASH_UI1_START;
    size = FLASH_UI1_SIZE;
    break;
  case FLASH_REGION_UI2:
    start = FLASH_UI2_START;
    size = FLASH_UI2_SIZE;
    break;
  default:
    return false;
  }
  return length <= size && address >= start && address - start <= size - length;
}

bool ICACHE_FLASH_ATTR flashRegionErase(FlashRegion region, uint32_t address) {
  if (address % SPI_FLASH_SEC_SIZE || !flashRegionContains(region, address, SPI_FLASH_SEC_SIZE))
    return false;
  return spi_flash_erase_sector(address / SPI_FLASH_SEC_SIZE) == SPI_FLASH_RESULT_OK;
}

bool ICACHE_FLASH_ATTR flashRegionWrite(FlashRegion region, uint32_t address, const void *data,
                                        uint32_t length) {
  if (!data || (address & 3) || (length & 3) || !flashRegionContains(region, address, length))
    return false;
  // Multipart source pointers need not be aligned. Keep each SDK write aligned
  // and bounded, and propagate errors before attempting another chunk.
  uint32_t aligned[64], verified[64];
  const uint8_t *source = data;
  while (length) {
    uint32_t count = length > sizeof(aligned) ? sizeof(aligned) : length;
    os_memcpy(aligned, source, count);
    if (spi_flash_write(address, aligned, count) != SPI_FLASH_RESULT_OK)
      return false;
    if (spi_flash_read(address, verified, count) != SPI_FLASH_RESULT_OK ||
        os_memcmp(aligned, verified, count) != 0)
      return false;
    source += count;
    address += count;
    length -= count;
  }
  return true;
}
