"""Synthetic flash and HTTP/SDK interfaces shared by native storage tests."""
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
HTTP = r'''
#pragma once
typedef struct {int len,buffLen,received;char *buff;} HttpdPostData;
typedef struct {void *conn,*cgiPrivData;HttpdPostData *post;int requestType,code;} HttpdConnData;
#define HTTPD_METHOD_POST 1
#define HTTPD_CGI_DONE 1
#define HTTPD_CGI_MORE 0
static void httpdStartResponse(HttpdConnData *c,int code) {c->code=code;}
static void httpdHeader(HttpdConnData *c,const char *name,const char *value) {(void)c;(void)name;(void)value;}
static void httpdEndHeaders(HttpdConnData *c) {(void)c;}
'''
SDK = r'''
#include <stdio.h>
#include <stdlib.h>
#define os_memset memset
#define os_zalloc test_alloc
#define os_free test_free
typedef unsigned char uint8;
typedef int ETSTimer;
typedef void os_timer_func_t(void *);
#define UPGRADE_FLAG_FINISH 2
static unsigned current_slot,upgrade_flag,scheduled,allocations,allocation_failure;
static void *test_alloc(size_t n) {if(allocation_failure)return NULL;allocations++;return calloc(1,n);}
static void test_free(void *p) {assert(allocations);allocations--;free(p);}
static uint8 system_upgrade_userbin_check(void) {return current_slot;}
static void system_upgrade_flag_set(unsigned flag) {upgrade_flag=flag;}
static void system_upgrade_reboot(void) {}
static void os_timer_disarm(ETSTimer *t) {(void)t;}
static void os_timer_setfn(ETSTimer *t,os_timer_func_t *f,void *p) {(void)t;(void)f;(void)p;}
static void os_timer_arm(ETSTimer *t,unsigned n,int repeat) {(void)t;assert(n==2000&&!repeat);scheduled++;}
'''
