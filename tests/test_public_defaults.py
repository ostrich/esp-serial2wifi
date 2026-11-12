#!/usr/bin/env python3
"""Compile the real first-boot setup helper; no SDK, radio or attached devices."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'.build-tests/defaults';OUT.mkdir(parents=True,exist_ok=True)
(OUT/'esp8266.h').write_text(r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define ICACHE_FLASH_ATTR
#define AUTH_OPEN 0
#define SOFTAP_MODE 2
#define os_memset memset
#define os_snprintf snprintf
#define os_strlen strlen
struct softap_config {uint8_t ssid[32],password[64],ssid_len,channel,authmode,ssid_hidden,max_connection;uint16_t beacon_interval;};
static uint32_t system_get_chip_id(void){return 0x123456;}
static bool modeOkay=true,apOkay=true;static unsigned mode,apCalls;static struct softap_config saved;
static bool wifi_set_opmode(unsigned value){mode=value;return modeOkay;}
static bool wifi_softap_set_config(struct softap_config *config){saved=*config;apCalls++;return apOkay;}
''')
(OUT/'test.c').write_text('#include <esp8266.h>\n#include "'+str(ROOT/'src/core/wifi_setup.c')+'"\n'+r'''
int main(void){
 assert(wifiSetupDefaults()&&mode==2&&apCalls==1);
 assert(!strcmp((char*)saved.ssid,"ESP-Serial2WiFi-123456")&&saved.ssid_len==strlen((char*)saved.ssid));
 assert(saved.authmode==AUTH_OPEN&&!saved.password[0]&&!saved.ssid_hidden);
 assert(saved.channel==1&&saved.max_connection==4&&saved.beacon_interval==100);
 modeOkay=false;assert(!wifiSetupDefaults()&&apCalls==1);
 modeOkay=true;apOkay=false;assert(!wifiSetupDefaults()&&apCalls==2);
 puts("PASS first boot: unique standalone setup AP, no shared credentials, bounded SDK failures");
}
''')
subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(OUT),str(OUT/'test.c'),'-o',str(OUT/'test')],check=True)
subprocess.run([str(OUT/'test')],check=True)
source=(ROOT/'src/core/main.c').read_text();assert 'restoreOk || wifiSetupDefaults()' in source and '!restoreOk && defaultsReady && !configSave()' in source
make=(ROOT/'Makefile').read_text()
for name in ('MCU_RESET_PIN','MCU_ISP_PIN','LED_CONN_PIN','LED_SERIAL_PIN'):assert name+' ?= -1' in make
config=(ROOT/'src/core/config.c').read_text();assert '.log_mode     = 1' in config and '.sntp_server  = "\\0"' in config
assert 'CHIP_IN_HOSTNAME ?= 1' in make
print('PASS saved settings bypass setup; default pins/debug UART/time/MQTT/SLIP do not assume local wiring/services')
