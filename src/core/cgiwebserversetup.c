#ifdef WEBSERVER
// Copyright (c) 2015 by Thorsten von Eicken, see LICENSE

#include <esp8266.h>
#include <osapi.h>
#include "cgi.h"
#include "cgioptiboot.h"
#include "multipart.h"
#include "espfsformat.h"
#include "config.h"
#include "flash_layout.h"
#include "web-server.h"
#include "api_request.h"
#include "operation.h"
#include "serbridge.h"
#include "serial_api.h"

int header_position = 0; // flash offset of the file header
int upload_position = 0; // flash offset where to store page upload
unsigned ICACHE_FLASH_ATTR webServerHTMLHeaderLength(void);
int html_header_len = 0; // length of the HTML header added to the file

// this is the header to add if user uploads HTML file
const char *HTML_HEADER =
    "<!doctype html><html><head><title>ESP Serial2WiFi</title>"
    "<link rel=stylesheet href=\"/pure.css\"><link rel=stylesheet href=\"/style.css\">"
    "<meta name=viewport content=\"width=device-width, initial-scale=1\"><script src=\"/ui.js\">"
    "</script><script src=\"/userpage.js\"></script></head><body><div id=layout>    ";

unsigned ICACHE_FLASH_ATTR webServerHTMLHeaderLength(void) { return os_strlen(HTML_HEADER) & ~3U; }

// this method is for flash writing and erasing the page
// write is incremental, so whenever a page border is reached, the next page will be erased
int ICACHE_FLASH_ATTR webServerSetupWriteFlash(int addr, void *data, int length) {
  if (addr < 0 || length <= 0 || !data ||
      !flashRegionContains(FLASH_REGION_PAGES, (uint32_t)addr, (uint32_t)length))
    return 1;
  uint32_t begin = (uint32_t)addr, end = begin + (uint32_t)length;
  uint32_t aligned_end = (end + 3) & ~3U;
  for (uint32_t cursor = begin & ~3U; cursor < aligned_end;) {
    uint32_t count = aligned_end - cursor;
    uint32_t sector_left = SPI_FLASH_SEC_SIZE - cursor % SPI_FLASH_SEC_SIZE;
    if (count > 256)
      count = 256;
    if (count > sector_left)
      count = sector_left;
    // An unaligned continuation must not erase a sector already partly written.
    if (cursor % SPI_FLASH_SEC_SIZE == 0 && cursor >= begin &&
        !flashRegionErase(FLASH_REGION_PAGES, cursor))
      return 1;
    uint32_t block[64];
    if (spi_flash_read(cursor, block, count) != SPI_FLASH_RESULT_OK)
      return 1;
    uint32_t first = cursor < begin ? begin : cursor;
    uint32_t last = cursor + count > end ? end : cursor + count;
    os_memcpy((uint8_t *)block + first - cursor, (uint8_t *)data + first - begin, last - first);
    if (!flashRegionWrite(FLASH_REGION_PAGES, cursor, block, count))
      return 1;
    cursor += count;
  }
  return 0;
}

// multipart callback for uploading user defined pages
int ICACHE_FLASH_ATTR webServerSetupMultipartCallback(MultipartCmd cmd, char *data, int dataLen,
                                                      int position) {
  switch (cmd) {
  case FILE_UPLOAD_START:
    upload_position = getUserPageSectionStart();
    header_position = upload_position;
    break;
  case FILE_START: {
    html_header_len = 0;

    // write the starting block on esp-fs
    EspFsHeader hdr;
    hdr.magic = 0xFFFFFFFF; // espfs magic is invalid during upload
    hdr.flags = 0;
    hdr.compression = 0;

    int len = dataLen + 1;
    while ((len & 3) != 0)
      len++;

    hdr.nameLen = len;
    hdr.fileLenComp = hdr.fileLenDecomp = 0xFFFFFFFF;

    header_position = upload_position;
    if (webServerSetupWriteFlash(upload_position, (uint32_t *)(&hdr), sizeof(EspFsHeader)))
      return 1;
    upload_position += sizeof(EspFsHeader);

    char nameBuf[len];
    os_memset(nameBuf, 0, len);
    os_memcpy(nameBuf, data, dataLen);

    if (webServerSetupWriteFlash(upload_position, (uint32_t *)(nameBuf), len))
      return 1;
    upload_position += len;

    // add header to HTML files
    if ((dataLen > 5) && (os_strcmp(data + dataLen - 5, ".html") ==
                          0)) // if the file ends with .html, wrap into an espfs image
    {
      html_header_len = os_strlen(HTML_HEADER) & ~3; // upload only 4 byte aligned part
      char buf[html_header_len];
      os_memcpy(buf, HTML_HEADER, html_header_len);
      if (webServerSetupWriteFlash(upload_position, (uint32_t *)(buf), html_header_len))
        return 1;
      upload_position += html_header_len;
    }
  } break;
  case FILE_DATA:
    if (webServerSetupWriteFlash(upload_position, data, dataLen))
      return 1;
    upload_position += dataLen;
    break;
  case FILE_DONE: {
    // write padding after the file
    uint8_t pad_cnt = (4 - position) & 3;
    if (pad_cnt) {
      uint32_t pad = 0;
      if (webServerSetupWriteFlash(upload_position, &pad, pad_cnt))
        return 1;
      upload_position += pad_cnt;
    }

    EspFsHeader hdr;
    hdr.magic = ESPFS_MAGIC;
    hdr.fileLenComp = hdr.fileLenDecomp = position + html_header_len;

    // restore ESPFS magic
    if (!flashRegionWrite(FLASH_REGION_PAGES, header_position + ((char *)&hdr.magic - (char *)&hdr),
                          &hdr.magic, sizeof(uint32_t)))
      return 1;
    // set file size
    if (!flashRegionWrite(FLASH_REGION_PAGES,
                          header_position + ((char *)&hdr.fileLenComp - (char *)&hdr),
                          &hdr.fileLenComp, sizeof(uint32_t)))
      return 1;
    if (!flashRegionWrite(FLASH_REGION_PAGES,
                          header_position + ((char *)&hdr.fileLenDecomp - (char *)&hdr),
                          &hdr.fileLenDecomp, sizeof(uint32_t)))
      return 1;
  } break;
  case FILE_UPLOAD_DONE: {
    // write the termination block

    EspFsHeader hdr;
    hdr.magic = ESPFS_MAGIC;
    hdr.flags = 1;
    hdr.compression = 0;
    hdr.nameLen = 0;
    hdr.fileLenComp = hdr.fileLenDecomp = 0;

    if (webServerSetupWriteFlash(upload_position, (uint32_t *)(&hdr), sizeof(EspFsHeader)))
      return 1;
    upload_position += sizeof(EspFsHeader);

    WEB_Init(); // reload the content
  } break;
  }
  return 0;
}

static MultipartCtx *webServerContext;
static HttpdConnData *pageUploadOwner;
bool ICACHE_FLASH_ATTR webServerUploadBusy(void) { return pageUploadOwner != NULL; }

int ICACHE_FLASH_ATTR cgiWebServerSetupUpload(HttpdConnData *connData) {
  if (!connData->conn) {
    if (pageUploadOwner == connData) {
      multipartDestroyContext(webServerContext);
      webServerContext = NULL;
      pageUploadOwner = NULL;
      deviceOperationEnd(connData);
    }
    return HTTPD_CGI_DONE;
  }
  if (pageUploadOwner && pageUploadOwner != connData) {
    errorResponse(connData, 409, "Another custom-page upload is active");
    return HTTPD_CGI_DONE;
  }
  if (!pageUploadOwner) {
    if (!apiWrite(connData))
      return HTTPD_CGI_DONE;
    if (programmingCB || serbridgeInMCUFlashing() || serialInputPending() ||
        serbridge_client_count() || !deviceOperationBegin(connData)) {
      errorResponse(connData, 409, "Another device operation is active");
      return HTTPD_CGI_DONE;
    }
  }
  if (!webServerContext) {
    webServerContext = multipartCreateContext(webServerSetupMultipartCallback);
    if (!webServerContext) {
      deviceOperationEnd(connData);
      errorResponse(connData, 503, "Not enough memory for upload");
      return HTTPD_CGI_DONE;
    }
  }
  pageUploadOwner = connData;
  int result = multipartProcess(webServerContext, connData);
  if (result == HTTPD_CGI_DONE) {
    multipartDestroyContext(webServerContext);
    webServerContext = NULL;
    pageUploadOwner = NULL;
    deviceOperationEnd(connData);
  }
  return result;
}
#endif
