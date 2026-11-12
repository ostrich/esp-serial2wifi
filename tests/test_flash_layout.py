#!/usr/bin/env python3
"""Geometry checks plus actual flash write guards under ASan/UBSan."""
import _paths
import copy
import subprocess
import tempfile
import unittest
from pathlib import Path
import flash_layout

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / '.build-tests/layout'

STUB = r'''
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define ICACHE_FLASH_ATTR
#define os_memcpy memcpy
#define os_memcmp memcmp
#define SPI_FLASH_SEC_SIZE 4096
#define SPI_FLASH_RESULT_OK 0
typedef enum {SYSTEM_PARTITION_BOOTLOADER=1,SYSTEM_PARTITION_OTA_1,
 SYSTEM_PARTITION_OTA_2,SYSTEM_PARTITION_RF_CAL,SYSTEM_PARTITION_PHY_DATA,
 SYSTEM_PARTITION_SYSTEM_PARAMETER,SYSTEM_PARTITION_CUSTOMER_BEGIN=100} partition_type_t;
typedef struct {partition_type_t type;uint32_t addr,size;} partition_item_t;
static uint32_t test_map,capacity;
static unsigned writes,erases,registrations,fail,fail_write,fail_read,corrupt_read;
static uint8_t memory[0x400000];
static uint32_t system_get_flash_size_map(void) {return test_map;}
static uint32_t spi_flash_get_id(void) {return capacity<<16|0x4068;}
static bool system_partition_table_regist(const partition_item_t *p,uint32_t n,uint32_t map) {
 assert(map==test_map);registrations++;
 for(unsigned i=0;i<n;i++) {
  assert(p[i].size && p[i].addr+p[i].size<=(1U<<capacity));
  for(unsigned j=0;j<i;j++)assert(p[i].addr>=p[j].addr+p[j].size||p[j].addr>=p[i].addr+p[i].size);
 } return true;
}
static int spi_flash_write(uint32_t a,const uint32_t *p,uint32_t n) {
 assert(!(a&3)&&!(n&3)&&!((uintptr_t)p&3));writes++;
 if(fail||fail_write)return 1;memcpy(memory+a,p,n);return 0;
}
static int spi_flash_erase_sector(uint32_t s) {
 erases++;if(fail)return 1;memset(memory+s*4096,255,4096);return 0;
}
static int spi_flash_read(uint32_t a,uint32_t *p,uint32_t n) {
 if(fail_read)return 1;memcpy(p,memory+a,n);if(corrupt_read&&n)*p^=1;return 0;
}
'''
TEST = r'''
int main(void) {
 test_map=FLASH_LAYOUT_MAP;capacity=0;
 for(uint32_t n=FLASH_LAYOUT_BYTES;n>1;n>>=1)capacity++;
 assert(flashLayoutRegister());assert(registrations==1);
 assert(flashPhysicalSize()==FLASH_LAYOUT_BYTES);
 memset(memory,255,sizeof memory);
 uint8_t source[12]={9,1,2,3,4,8,7,6,5};
 assert(flashRegionWrite(FLASH_REGION_CONFIG,FLASH_CONFIG_START,source+1,4));
 assert(!memcmp(memory+FLASH_CONFIG_START,source+1,4));
 assert(flashRegionWrite(FLASH_REGION_CONFIG,FLASH_CONFIG_START+FLASH_CONFIG_SIZE-4,source,4));
 unsigned oldwrites=writes,olderases=erases;
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,FLASH_CONFIG_START+1,source,4));
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,FLASH_CONFIG_START,source,3));
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,FLASH_CONFIG_START,source,0));
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,FLASH_CONFIG_START+FLASH_CONFIG_SIZE-4,source,8));
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,0xfffffffcU,source,8));
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,0,source,4));
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,FLASH_PAGES_START,source,4));
 assert(!flashRegionWrite(FLASH_REGION_APP1,FLASH_RF_START,source,4));
 assert(!flashRegionWrite(FLASH_REGION_APP2,FLASH_PARAMS_START,source,4));
 assert(!flashRegionErase(FLASH_REGION_CONFIG,FLASH_CONFIG_START+4));
 assert(!flashRegionErase(FLASH_REGION_CONFIG,FLASH_CONFIG_START+FLASH_CONFIG_SIZE));
 assert(writes==oldwrites&&erases==olderases);
 assert(flashRegionErase(FLASH_REGION_CONFIG,FLASH_CONFIG_START));
 assert(memory[FLASH_CONFIG_START]==255);
 fail=1;assert(!flashRegionErase(FLASH_REGION_CONFIG,FLASH_CONFIG_START));
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,FLASH_CONFIG_START,source,4));
 fail=0;corrupt_read=1;
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,FLASH_CONFIG_START,source,4));
 corrupt_read=0;fail_read=1;
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,FLASH_CONFIG_START,source,4));
 fail_read=0;test_map=99;
 assert(!flashLayoutRegister());assert(registrations==1);
 assert(!flashRegionWrite(FLASH_REGION_CONFIG,FLASH_CONFIG_START,source,4));
 test_map=FLASH_LAYOUT_MAP;capacity--;
 assert(!flashLayoutRegister());assert(registrations==1);
 assert(!flashRegionErase(FLASH_REGION_CONFIG,FLASH_CONFIG_START));
}
'''


class LayoutTests(unittest.TestCase):
    def test_preserved_addresses_and_rejected_geometry(self):
        for target in flash_layout.TARGETS:
            with self.subTest(target=target):
                value = flash_layout.layout(target)
                self.assertEqual(value['regions']['config'][0], 0x7a000 if target == '512KB' else 0x7e000)
                self.assertEqual(value['regions']['params'][0], value['bytes'] - 0x3000)
                for change in ('overlap', 'range', 'alignment'):
                    bad = copy.deepcopy(value)
                    bad['regions']['config'] = {'overlap': (0, 4096),
                        'range': (value['bytes'], 4096), 'alignment': (1, 4096)}[change]
                    with self.assertRaises(ValueError):
                        flash_layout.validate(bad)
        self.assertEqual(flash_layout.layout('2MB')['map'], 3)

    def test_actual_write_guards(self):
        PRIVATE.mkdir(parents=True, exist_ok=True)
        for target in flash_layout.TARGETS:
            with self.subTest(target=target), tempfile.TemporaryDirectory(dir=PRIVATE) as name:
                temp = Path(name)
                (temp / 'esp8266.h').write_text(STUB)
                (temp / 'generated_flash_layout.h').write_text(flash_layout.header(flash_layout.layout(target)))
                (temp / 'test.c').write_text('#include "' + str(ROOT / 'src/core/flash_layout.c') + '"\n' + TEST)
                subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-I' + str(temp), str(temp / 'test.c'), '-o', str(temp / 'test')], check=True)
                subprocess.run([str(temp / 'test')], check=True)

    def test_actual_custom_page_writer(self):
        source = (ROOT / 'src/core/cgiwebserversetup.c').read_text()
        start = source.index('int ICACHE_FLASH_ATTR webServerSetupWriteFlash')
        end = source.index('// multipart callback', start)
        writer = source[start:end]
        code = r'''
int main(void) {
 test_map=FLASH_LAYOUT_MAP;capacity=0;
 for(uint32_t n=FLASH_LAYOUT_BYTES;n>1;n>>=1)capacity++;
 memset(memory,255,sizeof memory);
 uint8_t payload[9000];for(unsigned i=0;i<sizeof payload;i++)payload[i]=(i*7)%251;
 unsigned offset=0;unsigned chunks[]={1,3,5,4000,7,23,4961};
 for(unsigned i=0;i<sizeof chunks/sizeof chunks[0];i++) {
  assert(webServerSetupWriteFlash(FLASH_PAGES_START+offset,payload+offset,chunks[i])==0);
  offset+=chunks[i];assert(!memcmp(memory+FLASH_PAGES_START,payload,offset));
 }
 assert(offset==sizeof payload);
 unsigned oldwrites=writes,olderases=erases;
 assert(webServerSetupWriteFlash(-1,payload,4)==1);
 assert(webServerSetupWriteFlash(FLASH_PAGES_START-1,payload,4)==1);
 assert(webServerSetupWriteFlash(FLASH_PAGES_START+FLASH_PAGES_SIZE-1,payload,4)==1);
 assert(writes==oldwrites&&erases==olderases);
 assert(memory[FLASH_CONFIG_START]==255&&memory[FLASH_PARAMS_START]==255);
}
'''
        PRIVATE.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=PRIVATE) as name:
            temp = Path(name)
            (temp / 'esp8266.h').write_text(STUB)
            (temp / 'generated_flash_layout.h').write_text(flash_layout.header(flash_layout.layout('4MB')))
            (temp / 'test.c').write_text('#include "' + str(ROOT / 'src/core/flash_layout.c') + '"\n' + writer + code)
            subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                '-I' + str(temp), str(temp / 'test.c'), '-o', str(temp / 'test')], check=True)
            subprocess.run([str(temp / 'test')], check=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
