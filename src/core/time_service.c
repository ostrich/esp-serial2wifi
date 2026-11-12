// Copyright 2015 by Thorsten von Eicken, see LICENSE
#include <esp8266.h>
#include "config.h"
#include "sntp.h"
#include "time_service.h"

void ICACHE_FLASH_ATTR timeServiceInit() {
  if (flashConfig.sntp_server[0] != '\0') {
    sntp_stop();
    if (true == sntp_set_timezone(flashConfig.timezone_offset)) {
      sntp_setservername(0, flashConfig.sntp_server);
      sntp_init();
    }
  }
}
