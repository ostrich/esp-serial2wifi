// Copyright 2015 by Thorsten von Eicken, see LICENSE
/* Configuration stored in flash */

#include <esp8266.h>
#include "config.h"
#include "espfs.h"
#include "crc16.h"
#include "flash_layout.h"

FlashConfig flashConfig;
FlashConfig flashDefault = {
  .seq = 33, .magic = 0, .crc = 0,
  .reset_pin    = MCU_RESET_PIN, .isp_pin = MCU_ISP_PIN,
  .conn_led_pin = LED_CONN_PIN, .ser_led_pin = LED_SERIAL_PIN,
  .baud_rate    = 115200,
  .hostname     = "esp-serial2wifi\0",
  .staticip     = 0,
  .netmask      = 0x00ffffff,
  .gateway      = 0,
  .log_mode     = 1, // debug UART off; ROM boot output cannot be suppressed
  .swap_uart    = 0,
  .slip_enable  = 0, .mqtt_enable = 0, .mqtt_status_enable = 0,
  .mqtt_timeout = 2, .mqtt_clean_session = 1,
  .mqtt_port    = 1883, .mqtt_keepalive = 60,
  .reserved_host  = "\0", .reserved_password = "\0", .reserved_username = "\0", .mqtt_clientid = "\0",
  .mqtt_username  = "\0", .mqtt_password = "\0", .mqtt_status_topic = "\0",
  .mqtt_host      = "\0",
  .sys_descr 	  = "\0",
  .rx_pullup	  = 1,
  .sntp_server  = "\0",
  .syslog_host = "\0", .syslog_minheap = 8192, .syslog_filter = 7, .syslog_showtick = 1, .syslog_showdate = 0,
  .mdns_enable = 1, .mdns_servername = "http\0", .timezone_offset = 0,
  .data_bits	= EIGHT_BITS,
  .parity	= NONE_BITS,
  .stop_bits	= ONE_STOP_BIT,
  .uart_format_version = 1,
};

typedef union {
  FlashConfig fc;
  uint8_t     block[1024];
} FlashFull;

// magic number to recognize thet these are our flash settings as opposed to some random stuff
#define FLASH_MAGIC  (0xaa55)

// size of the setting sector
#define FLASH_SECT   (4096)

// address where to flash the settings: if we have >512KB flash then there are 16KB of reserved
// space at the end of the first flash partition, we use the upper 8KB (2 sectors). If we only
// have 512KB then that space is used by the SDK and we use the 8KB just before that.
static uint32_t ICACHE_FLASH_ATTR flashAddr(void) {
  return FLASH_CONFIG_START;
}

static int flash_pri; // primary flash sector (0 or 1, or -1 for error)



bool ICACHE_FLASH_ATTR configSave(void) {
  FlashFull ff;
  os_memset(&ff, 0, sizeof(ff));
  os_memcpy(&ff, &flashConfig, sizeof(FlashConfig));
  // UINT32_MAX is reserved for an uncommitted copy.
  uint32_t seq = ff.fc.seq >= 0xfffffffeU ? 0 : ff.fc.seq + 1;
  // erase secondary
  uint32_t addr = flashAddr() + (1-flash_pri)*FLASH_SECT;
  if (!flashRegionErase(FLASH_REGION_CONFIG, addr))
    goto fail; // no harm done, give up
  // calculate CRC
  ff.fc.seq = seq;
  ff.fc.magic = FLASH_MAGIC;
  ff.fc.crc = 0;
  ff.fc.crc = crc16_data((unsigned char*)&ff, sizeof(ff), 0);
  // write primary with incorrect seq
  ff.fc.seq = 0xffffffff;
  if (!flashRegionWrite(FLASH_REGION_CONFIG, addr, &ff, sizeof(ff)))
    goto fail; // no harm done, give up
  // fill in correct seq
  ff.fc.seq = seq;
  if (!flashRegionWrite(FLASH_REGION_CONFIG, addr, &ff, sizeof(uint32_t)))
    goto fail; // most likely failed, but no harm if successful
  flashConfig.seq = seq;
  // now that we have safely written the new version, erase old primary
  addr = flashAddr() + flash_pri*FLASH_SECT;
  flash_pri = 1-flash_pri;
  if (!flashRegionErase(FLASH_REGION_CONFIG, addr))
    return true; // no back-up but we're OK
  // write secondary
  ff.fc.seq = 0xffffffff;
  if (!flashRegionWrite(FLASH_REGION_CONFIG, addr, &ff, sizeof(ff)))
    return true; // no back-up but we're OK
  ff.fc.seq = seq;
  flashRegionWrite(FLASH_REGION_CONFIG, addr, &ff, sizeof(uint32_t));
  return true;
fail:
#ifdef CONFIG_DBG
  os_printf("*** Failed to save config ***\n");
#endif
  return false;
}

void ICACHE_FLASH_ATTR configWipe(void) {
  flashRegionErase(FLASH_REGION_CONFIG, flashAddr());
  flashRegionErase(FLASH_REGION_CONFIG, flashAddr()+FLASH_SECT);
}

static int ICACHE_FLASH_ATTR selectPrimary(FlashFull *fc0, FlashFull *fc1);

bool ICACHE_FLASH_ATTR configRestore(void) {
  FlashFull ff0, ff1;
  // read both flash sectors
  if (spi_flash_read(flashAddr(), (void *)&ff0, sizeof(ff0)) != SPI_FLASH_RESULT_OK)
    os_memset(&ff0, 0, sizeof(ff0)); // clear in case of error
  if (spi_flash_read(flashAddr()+FLASH_SECT, (void *)&ff1, sizeof(ff1)) != SPI_FLASH_RESULT_OK)
    os_memset(&ff1, 0, sizeof(ff1)); // clear in case of error
  // figure out which one is good
  flash_pri = selectPrimary(&ff0, &ff1);
  // if neither is OK, we revert to defaults
  if (flash_pri < 0) {
    os_memcpy(&flashConfig, &flashDefault, sizeof(FlashConfig));
#ifdef CHIP_IN_HOSTNAME
    os_snprintf(flashConfig.hostname, sizeof(flashConfig.hostname), "esp-serial2wifi-%06x", system_get_chip_id() & 0xffffff);
#endif
    os_memcpy(&flashConfig.mqtt_clientid, &flashConfig.hostname, os_strlen(flashConfig.hostname));
    os_memcpy(&flashConfig.mqtt_status_topic, &flashConfig.hostname, os_strlen(flashConfig.hostname));
    flash_pri = 0;
    return false;
  }
  // copy good one into global var
  os_memcpy(&flashConfig, flash_pri == 0 ? &ff0.fc : &ff1.fc, sizeof(FlashConfig));
  return true;
}

static int ICACHE_FLASH_ATTR selectPrimary(FlashFull *ff0, FlashFull *ff1) {
  // check CRC of ff0
  uint16_t crc = ff0->fc.crc;
  ff0->fc.crc = 0;
  bool ff0_crc_ok = ff0->fc.magic == FLASH_MAGIC && ff0->fc.uart_format_version == 1 && ff0->fc.seq != 0xffffffffU &&
      crc16_data((unsigned char*)ff0, sizeof(FlashFull), 0) == crc;
#ifdef CONFIG_DBG
  os_printf("FLASH chk=0x%04x crc=0x%04x full_sz=%d sz=%d chip_sz=%d\n",
      crc16_data((unsigned char*)ff0, sizeof(FlashFull), 0),
      crc,
      sizeof(FlashFull),
      sizeof(FlashConfig),
      getFlashSize());
#endif

  // check CRC of ff1
  crc = ff1->fc.crc;
  ff1->fc.crc = 0;
  bool ff1_crc_ok = ff1->fc.magic == FLASH_MAGIC && ff1->fc.uart_format_version == 1 && ff1->fc.seq != 0xffffffffU &&
      crc16_data((unsigned char*)ff1, sizeof(FlashFull), 0) == crc;

  // decided which we like better
  if (ff0_crc_ok)
    if (!ff1_crc_ok || (int32_t)(ff0->fc.seq - ff1->fc.seq) >= 0)
      return 0; // use first sector as primary
    else
      return 1; // second sector is newer
  else
    return ff1_crc_ok ? 1 : -1;
}

// returns the flash chip's size, in BYTES
const size_t ICACHE_FLASH_ATTR
getFlashSize() {
  return flashPhysicalSize();
}

const uint32_t getUserPageSectionStart()
{
  return FLASH_PAGES_START;
}

const uint32_t getUserPageSectionEnd()
{
  return FLASH_PAGES_START + FLASH_PAGES_SIZE;
}
