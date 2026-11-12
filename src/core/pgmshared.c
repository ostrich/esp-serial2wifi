// Copyright (c) 2015 by Thorsten von Eicken, see LICENSE
// Copyright (c) 2016-2017 by Danny Backx

#include <esp8266.h>
#include <osapi.h>
#include "cgi.h"
#include "config.h"
#include "uart.h"
#include "stk500v2.h"
#include "serbridge.h"
#include "serled.h"

#include "pgmshared.h"

struct optibootData *optibootData;

char responseBuf[RESP_SZ]; // buffer to accumulate responses from optiboot
short responseLen = 0;     // amount accumulated so far
char errMessage[ERR_MAX];  // error message

// verify that N chars are hex characters
bool ICACHE_FLASH_ATTR checkHex(char *buf, short len) {
  while (len--) {
    char c = *buf++;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
      continue;
    DBG("OB non-hex\n");
    os_sprintf(errMessage, "Non hex char in POST record: '%c'/0x%02x", c, c);
    return false;
  }
  return true;
}

// get hex value of some hex characters
uint32_t ICACHE_FLASH_ATTR getHexValue(char *buf, short len) {
  uint32_t v = 0;
  while (len--) {
    v = (v<<4) | (uint32_t)(*buf & 0xf);
    if (*buf > '9') v += 9;
    buf++;
  }
  return v;
}

// verify checksum
static bool ICACHE_FLASH_ATTR verifyChecksum(char *buf, short len) {
  uint8_t sum = 0;
  while (len >= 2) {
    sum += (uint8_t)getHexValue(buf, 2);
    buf += 2;
    len -= 2;
  }
  return sum == 0;
}

// We've not built one function that works on both, but kept the different functions.
// This calls one or the other
static bool ICACHE_FLASH_ATTR programPage() {
  if (optibootData->mega)
    return megaProgramPage();
  return optibootProgramPage();
}

static bool ICACHE_FLASH_ATTR invalidRecord(unsigned reason) {
  os_sprintf(errMessage,"Invalid or unsupported HEX record (code %u)",reason);return false;
}
static bool ICACHE_FLASH_ATTR drainPages(bool partial) {
  while(optibootData->pageLen&&(partial||optibootData->pageLen>=optibootData->pgmSz)) {
    unsigned before=optibootData->pageLen;
    if(!programPage())return false;
    if(optibootData->pageLen>=before)return invalidRecord(5);
  }return true;
}
// Process one complete Intel HEX record. Validate before reading fields or
// appending payload; malformed records must never be copied into error buffers.
bool ICACHE_FLASH_ATTR processRecord(char *buf, short len) {
  if(!optibootData||!optibootData->pageBuf||!optibootData->pgmSz||
      optibootData->pgmSz>PGM_PAGE_BYTES||optibootData->eof)return invalidRecord(4);
  if(len<11||len>521||buf[0]!=':'||!checkHex(buf+1,len-1))return invalidRecord(1);
  unsigned count=getHexValue(buf+1,2);
  if((unsigned)len!=11+count*2)return invalidRecord(1);
  buf++;len--;
  if(!verifyChecksum(buf,len))return invalidRecord(2);
  unsigned kind=getHexValue(buf+6,2),recordAddress=getHexValue(buf+2,4);
  if((kind==1&&(count||recordAddress))||((kind==2||kind==4)&&(count!=2||recordAddress))||
      (kind==5&&(count!=4||recordAddress)))return invalidRecord(1);
  // dispatch based on record type
  uint8_t type = getHexValue(buf+6, 2);
  switch (type) {
  case 0x00: { // Intel HEX data record
    //DBG("OB REC data %ld pglen=%d\n", getHexValue(buf, 2), optibootData->pageLen);
    uint32_t addr = getHexValue(buf+2, 4);
    // check whether we need to program previous record(s)
    if (optibootData->pageLen > 0 &&
        addr != ((optibootData->address+optibootData->pageLen)&0xffff)) {
      //DBG("OB addr chg\n");
      //DBG("processRecord addr chg, len %d, addr 0x%04x\n", optibootData->pageLen, addr);
      if (!drainPages(true)) return false;
    }
    // set address, unless we're adding to the end (programPage may have changed pageLen)
    if (optibootData->pageLen == 0) {
      optibootData->address = (optibootData->address & 0xffff0000) | addr;
      //DBG("OB set-addr 0x%lx\n", optibootData->address);
    }
    // append record
    uint16_t recLen = getHexValue(buf, 2);
    if((unsigned)optibootData->pageLen+recLen>PGM_PAGE_BUFFER_BYTES)return invalidRecord(5);
    for (uint16_t i=0; i<recLen; i++)
      optibootData->pageBuf[optibootData->pageLen++] = getHexValue(buf+8+2*i, 2);
    // Drain every full page. A maximum-size record can complete more than one;
    // draining only one lets successive records grow beyond the staging buffer.
    if(!drainPages(false))return false;
    break; }
  case 0x01: // Intel HEX EOF record
    DBG("OB EOF\n");
    // program any remaining partial page
#if 1
    if (optibootData->pageLen > 0) {
      // DBG("processRecord remaining partial page, len %d, addr 0x%04x\n", optibootData->pageLen, optibootData->address + optibootData->segment);
      if (!drainPages(true)) return false;
    }
    optibootData->eof = true;
#else
    if (optibootData->pageLen > 0) {
      // HACK : fill up with 0xFF
      while (optibootData->pageLen < 256) {
        optibootData->pageBuf[optibootData->pageLen++] = 0xFF;
      }
      if (!drainPages(true)) return false;
    }
    optibootData->eof = true;
#endif
    break;
  case 0x04: // Intel HEX address record
    DBG("OB address 0x%x\n", getHexValue(buf+8, 4) << 16);
    // program any remaining partial page
    if (optibootData->pageLen > 0) {
      DBG("processRecord 0x04 remaining partial page, len %d, addr 0x%04x\n",
        optibootData->pageLen, optibootData->address + optibootData->segment);
      if (!drainPages(true)) return false;
    }
    uint32_t upper=getHexValue(buf+8,4);
    if(!optibootData->mega&&upper)return invalidRecord(6);
    optibootData->address=upper<<16;optibootData->segment=0;
    break;
  case 0x05: // Intel HEX start address (MDK-ARM only)
    // ignore, there's no way to tell optiboot that...
    break;
  case 0x02: // Intel HEX extended segment address record
    if(!drainPages(true))return false;
    uint32_t segment=getHexValue(buf+8,4)<<4;
    if(!optibootData->mega&&segment)return invalidRecord(6);
    optibootData->segment=segment;optibootData->address=0;
    return true;
  default:
    return invalidRecord(3);
  }
  return true;
}
