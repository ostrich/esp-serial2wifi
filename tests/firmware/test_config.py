#!/usr/bin/env python3
"""Run actual config.c against synthetic flash; never use real credentials."""
import _paths
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    source = ROOT
    out = ROOT / ".build-tests/config" / source.name
    out.mkdir(parents=True, exist_ok=True)
    (out / "esp8266.h").write_text(r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define ICACHE_FLASH_ATTR
#define os_memset memset
#define os_memcpy memcpy
#define os_memcmp memcmp
#define os_sprintf sprintf
#define os_snprintf snprintf
#define os_strlen strlen
#define os_printf(...) ((void)0)
#define MCU_RESET_PIN -1
#define MCU_ISP_PIN -1
#define LED_CONN_PIN -1
#define LED_SERIAL_PIN -1
typedef uint32_t uint32;
typedef uint8_t uint8;
#define BIT(n) (1U<<(n))
#define BIT2 BIT(2)
#define BIT3 BIT(3)
#define BIT4 BIT(4)
#define BIT5 BIT(5)
#include "uart_hw.h"
#define FIRMWARE_SIZE 503808
#define SPI_FLASH_RESULT_OK 0
enum flash_size_map { FLASH_SIZE_4M_MAP_256_256=0,
 FLASH_SIZE_8M_MAP_512_512=2, FLASH_SIZE_16M_MAP_512_512=3,
 FLASH_SIZE_32M_MAP_512_512=4, FLASH_SIZE_16M_MAP_1024_1024=5,
 FLASH_SIZE_32M_MAP_1024_1024=6 };
static unsigned char flash[0x400000];
static int cut=-1, operations;
static enum flash_size_map system_get_flash_size_map(void) { return 4; }
static uint32_t system_get_chip_id(void) { return 0x123456; }
static uint32_t spi_flash_get_id(void) { return 0x1640ef; }
static int spi_flash_read(uint32_t a, void *p, size_t n) {
 assert(a+n<=sizeof flash); memcpy(p,flash+a,n); return 0;
}
static int fault(void) { return cut>=0 && operations++>=cut; }
static int spi_flash_write(uint32_t a, const void *p, size_t n) {
 assert(a+n<=sizeof flash); if(fault())return 1;
 for(size_t i=0;i<n;i++) { unsigned char b=((const unsigned char *)p)[i];
  assert((flash[a+i]&b)==b); flash[a+i]&=b; } return 0;
}
static int spi_flash_erase_sector(uint32_t s) {
 assert(s*4096+4096<=sizeof flash); if(fault())return 1;
 memset(flash+s*4096,255,4096); return 0;
}
''')
    subprocess.run(["python3", str(source / "tools/lib/flash_layout.py"), "4MB",
                    "--header", str(out / "generated_flash_layout.h")], check=True)
    with (out / "esp8266.h").open("a") as stub:
        stub.write(r"""
#define SPI_FLASH_SEC_SIZE 4096
typedef enum {SYSTEM_PARTITION_BOOTLOADER=1,SYSTEM_PARTITION_OTA_1,
 SYSTEM_PARTITION_OTA_2,SYSTEM_PARTITION_RF_CAL,SYSTEM_PARTITION_PHY_DATA,
 SYSTEM_PARTITION_SYSTEM_PARAMETER,SYSTEM_PARTITION_CUSTOMER_BEGIN=100} partition_type_t;
typedef struct {partition_type_t type;uint32_t addr,size;} partition_item_t;
static bool system_partition_table_regist(const partition_item_t *p,uint32_t n,uint32_t map) {
 (void)p;(void)n;return map==4;
}
""")
    includes = '\n'.join('#include ' + json.dumps(str(source / p))
                         for p in ('src/serial/crc16.c','src/core/flash_layout.c','src/core/config.c'))
    code = includes + r'''
static void fixture(const char *user,const char *password) {
 memset(flash,255,sizeof flash); FlashFull f={0}; f.fc=flashDefault;
 f.fc.seq=10; f.fc.magic=FLASH_MAGIC;
 strcpy(f.fc.hostname,"before");
 strcpy(f.fc.mqtt_username,user);
 strcpy(f.fc.mqtt_password,password);
 f.fc.crc=0; f.fc.crc=crc16_data((unsigned char *)&f,sizeof f,0);
 memcpy(flash+0x7e000,&f,sizeof f); memcpy(flash+0x7f000,&f,sizeof f);
}
int main(void) {
 fixture("current-user","current-password"); assert(configRestore());
 assert(flashConfig.ap_address==0); // Zero selects the default AP address.
 flashConfig.ap_address=0xfe071e0a;assert(configSave());memset(&flashConfig,0,sizeof flashConfig);
 assert(configRestore()&&flashConfig.ap_address==0xfe071e0a);
 assert(strcmp(flashConfig.mqtt_username,"current-user")==0);
 assert(strcmp(flashConfig.mqtt_password,"current-password")==0);
 for(int boundary=0;boundary<20;boundary++) {
  cut=-1;fixture("current-user","current-password");assert(configRestore());
  strcpy(flashConfig.hostname,"after");flashConfig.ap_address=0xfe071e0a;cut=boundary;operations=0;
  configSave();cut=-1;memset(&flashConfig,0,sizeof flashConfig);
  assert(configRestore());
  assert(strcmp(flashConfig.hostname,"before")==0 || strcmp(flashConfig.hostname,"after")==0);
  assert(flashConfig.ap_address==(!strcmp(flashConfig.hostname,"after")?0xfe071e0a:0));
  assert(strcmp(flashConfig.mqtt_username,"current-user")==0);
  assert(strcmp(flashConfig.mqtt_password,"current-password")==0);
 }

 memset(flash,255,sizeof flash);assert(!configRestore());
 #ifdef CHIP_IN_HOSTNAME
 assert(strcmp(flashConfig.hostname,"esp-serial2wifi-123456")==0);
#else
 assert(strcmp(flashConfig.hostname,"esp-serial2wifi")==0);
#endif
 assert(flashConfig.reset_pin==-1&&flashConfig.isp_pin==-1&&flashConfig.conn_led_pin==-1&&flashConfig.ser_led_pin==-1);
 assert(flashConfig.log_mode==1&&!flashConfig.sntp_server[0]&&!flashConfig.mqtt_enable&&!flashConfig.slip_enable);
 assert(strcmp(flashConfig.mqtt_clientid,flashConfig.hostname)==0);
 assert(strcmp(flashConfig.mqtt_status_topic,flashConfig.hostname)==0);
 // A configured hostname is persisted user configuration, not a default to replace.
 strcpy(flashConfig.hostname,"operator-device");strcpy(flashConfig.mqtt_clientid,"operator-client");strcpy(flashConfig.mqtt_status_topic,"operator/topic");
 assert(configSave());memset(&flashConfig,0,sizeof flashConfig);assert(configRestore());
 assert(!strcmp(flashConfig.hostname,"operator-device")&&!strcmp(flashConfig.mqtt_clientid,"operator-client")&&!strcmp(flashConfig.mqtt_status_topic,"operator/topic"));
 memset(flash,0,sizeof flash);assert(!configRestore());
 fixture("current-user","current-password");
 FlashFull wrap;memcpy(&wrap,flash+0x7e000,sizeof wrap);
 wrap.fc.seq=0xfffffffeU;wrap.fc.crc=0;
 wrap.fc.crc=crc16_data((unsigned char *)&wrap,sizeof wrap,0);
 memcpy(flash+0x7e000,&wrap,sizeof wrap);memcpy(flash+0x7f000,&wrap,sizeof wrap);
 assert(configRestore());assert(configSave());assert(flashConfig.seq==0);
 assert(configRestore());assert(flashConfig.seq==0);
 assert(configSave());assert(flashConfig.seq==1);
 assert(configRestore());assert(flashConfig.seq==1);

 fixture("current-user","current-password");FlashFull unsupported;
 memcpy(&unsupported,flash+0x7e000,sizeof unsupported);unsupported.fc.uart_format_version=0;
 unsupported.fc.crc=0;unsupported.fc.crc=crc16_data((unsigned char *)&unsupported,sizeof unsupported,0);
 memcpy(flash+0x7e000,&unsupported,sizeof unsupported);memcpy(flash+0x7f000,&unsupported,sizeof unsupported);
 assert(!configRestore()&&flashConfig.uart_format_version==1);
 assert(!strcmp(flashConfig.mqtt_username,"")&&!strcmp(flashConfig.mqtt_password,""));
 assert(configSave()&&configRestore());
 puts("PASS unsupported configuration records are rejected rather than migrated");
 for(int bits=0;bits<=3;bits++)for(int parity=0;parity<=2;parity++)for(int stop=1;stop<=3;stop++) {
  flashConfig.data_bits=bits;flashConfig.parity=parity;flashConfig.stop_bits=stop;
  flashConfig.uart_format_version=1;assert(configSave());memset(&flashConfig,0,sizeof flashConfig);
  assert(configRestore());assert(flashConfig.data_bits==bits&&flashConfig.parity==parity&&flashConfig.stop_bits==stop);
  unsigned register_bits=CALC_UARTMODE(bits,parity,stop);
  assert(((register_bits>>2)&3)==(unsigned)bits&&((register_bits>>4)&3)==(unsigned)stop);
  assert((register_bits&3)==(parity==0?0:parity==1?3:2));
 }
 puts("PASS UART register encoding, all 36 persisted formats including five-bit and Odd");
 puts("PASS current credentials and failed-write boundaries preserve a valid configuration");
}
'''
    (out / "test.c").write_text(code)
    for chip in (False,True):
     subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Wno-unused-function',
                    '-Wno-ignored-qualifiers', '-fsanitize=address,undefined',
                    '-I' + str(out), '-I' + str(source/'src/core'),
                    '-I' + str(source/'src/espfs'), '-I' + str(source / 'include'), '-I' + str(source/'src/serial'),
                    *(['-DCHIP_IN_HOSTNAME'] if chip else []),str(out / 'test.c'), '-o', str(out / 'test')], check=True)
     subprocess.run([str(out / 'test')], check=True)


if __name__ == '__main__':
    main()
