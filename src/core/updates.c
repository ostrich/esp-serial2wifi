#include <esp8266.h>
#include "updates.h"
#include "api_request.h"
#include "operation.h"
#include "cgiflash.h"
#include "ui_storage.h"
#include "flash_layout.h"
#include "serbridge.h"
#include "serial_api.h"
#ifdef WEBSERVER
#include "cgiwebserversetup.h"
#include "web-server.h"
#include "espfsformat.h"
#endif
extern char *firmwareVersion;
static bool ICACHE_FLASH_ATTR idle(HttpdConnData *c) {
  if (deviceOperationBusy() || programmingCB || serbridgeInMCUFlashing() || serialInputPending() ||
      serbridge_client_count()) {
    apiError(c, 409, "SERIAL_BUSY",
             "Close serial TCP clients and finish MCU operations before changing flash.");
    return false;
  }
  return true;
}
static void ICACHE_FLASH_ATTR firmwareInfo(ApiJson *j, const void *unused) {
  (void)unused;
  unsigned slot = system_upgrade_userbin_check();
  apiJsonLiteral(j, "{\"schema\":1,\"version\":");
  apiJsonString(j, firmwareVersion, 96);
  apiJsonLiteral(j, ",\"sdk\":");
  apiJsonString(j, system_get_sdk_version(), 32);
  apiJsonLiteral(j, ",\"image_abi\":");
  apiJsonString(j, SERIAL2WIFI_IMAGE_ABI, 48);
  apiJsonLiteral(j, ",\"flash_bytes\":");
  apiJsonUnsigned(j, flashPhysicalSize());
  apiJsonLiteral(j, ",\"active_slot\":");
  if (slot < 2)
    apiJsonUnsigned(j, slot + 1);
  else
    apiJsonLiteral(j, "null");
  apiJsonLiteral(j, ",\"target_slot\":");
  if (slot < 2)
    apiJsonUnsigned(j, 2 - slot);
  else
    apiJsonLiteral(j, "null");
  apiJsonLiteral(j, ",\"max_image_bytes\":");
  apiJsonUnsigned(j, FLASH_EXTERNAL_UI ? UI_PACKAGE_LIMIT
                                       : (slot == 0 ? FLASH_APP2_SIZE : FLASH_APP1_SIZE));
  apiJsonLiteral(j, ",\"external_ui\":");
  apiJsonBool(j, FLASH_EXTERNAL_UI);
  apiJsonLiteral(j, ",\"ota_supported\":");
  apiJsonBool(j, firmwareOTASupported());
  apiJsonLiteral(j, ",\"reason\":");
  apiJsonString(j, firmwareOTAReason(), 120);
  apiJsonLiteral(j, ",\"cross_sdk_ota\":false,\"automatic_rollback\":false}");
}
int ICACHE_FLASH_ATTR apiFirmwareInfo(HttpdConnData *c) {
  if (!apiRead(c))
    return HTTPD_CGI_DONE;
  return apiResponse(c, 200, firmwareInfo, NULL);
}
int ICACHE_FLASH_ATTR apiFirmwareUpload(HttpdConnData *c) {
  if (c->conn && !c->cgiPrivData && (!apiWrite(c) || !idle(c)))
    return HTTPD_CGI_DONE;
  return cgiUploadFirmware(c);
}
int ICACHE_FLASH_ATTR apiFirmwareActivate(HttpdConnData *c) {
  char action[24];
  ApiField f = {"action", action, sizeof action, 0};
  if (!apiForm(c, &f, 1))
    return HTTPD_CGI_DONE;
  if (os_strcmp(action, "activate_firmware"))
    return apiError(c, 422, "INVALID_ACTION",
                    "Explicitly choose activation of the uploaded firmware.");
  if (!idle(c))
    return HTTPD_CGI_DONE;
  return cgiRebootFirmware(c);
}
#ifdef WEBSERVER
// One name per response keeps even maximally escaped filenames within the HTTP
// response budget. Every raw read is checked against the custom-page region.
typedef struct {
  uint32_t index, count, used;
  bool valid, truncated, found;
  char name[256];
  uint32_t bytes;
} Pages;
static bool ICACHE_FLASH_ATTR readPages(uint32_t address, void *data, unsigned length) {
  return flashRegionContains(FLASH_REGION_PAGES, address, length) &&
         spi_flash_read(address, data, length) == SPI_FLASH_RESULT_OK;
}
static void ICACHE_FLASH_ATTR scanPages(Pages *p) {
  uint32_t at = FLASH_PAGES_START;
  for (unsigned n = 0; n < 256; n++) {
    uint32_t raw[4];
    if (!readPages(at, raw, sizeof raw))
      return;
    EspFsHeader *h = (EspFsHeader *)raw;
    if (h->magic != ESPFS_MAGIC)
      return;
    if (h->flags & FLAG_LASTFILE) {
      p->valid = true;
      p->used = at - FLASH_PAGES_START + sizeof(*h);
      return;
    }
    if (h->nameLen <= 0 || h->nameLen > 256 || (h->nameLen & 3) || h->fileLenComp < 0 ||
        h->fileLenDecomp < 0 || h->compression != COMPRESS_NONE)
      return;
    uint32_t nameAt = at + sizeof(*h), body = nameAt + h->nameLen,
             length = (uint32_t)h->fileLenComp;
    if (!flashRegionContains(FLASH_REGION_PAGES, nameAt, h->nameLen) ||
        (length && !flashRegionContains(FLASH_REGION_PAGES, body, length)))
      return;
    if (n == p->index) {
      uint32_t name[64];
      if (!readPages(nameAt, name, h->nameLen))
        return;
      char *s = (char *)name;
      unsigned size = 0;
      while (size < (unsigned)h->nameLen && s[size])
        size++;
      if (!size || size == (unsigned)h->nameLen)
        return;
      os_memcpy(p->name, s, size);
      p->name[size] = 0;
      p->bytes = h->fileLenDecomp;
      p->found = true;
    }
    p->count++;
    uint32_t step = (length + 3) & ~3U;
    if (step < length || (step && !flashRegionContains(FLASH_REGION_PAGES, body, step)))
      return;
    at = body + step;
  }
  p->truncated = true;
  p->valid = true;
}
static void ICACHE_FLASH_ATTR pagesInfo(ApiJson *j, const void *data) {
  const Pages *p = data;
  apiJsonLiteral(j, "{\"schema\":1,\"supported\":true,\"capacity_bytes\":");
  apiJsonUnsigned(j, FLASH_PAGES_SIZE);
  apiJsonLiteral(j, ",\"html_prefix_bytes\":");
  apiJsonUnsigned(j, webServerHTMLHeaderLength());
  apiJsonLiteral(j, ",\"valid\":");
  apiJsonBool(j, p->valid);
  apiJsonLiteral(j, ",\"used_bytes\":");
  if (p->valid && !p->truncated)
    apiJsonUnsigned(j, p->used);
  else
    apiJsonLiteral(j, "null");
  apiJsonLiteral(j, ",\"count\":");
  apiJsonUnsigned(j, p->count);
  apiJsonLiteral(j, ",\"truncated\":");
  apiJsonBool(j, p->truncated);
  apiJsonLiteral(j, ",\"index\":");
  apiJsonUnsigned(j, p->index);
  apiJsonLiteral(j, ",\"next\":");
  if (p->index + 1 < p->count)
    apiJsonUnsigned(j, p->index + 1);
  else
    apiJsonLiteral(j, "null");
  apiJsonLiteral(j, ",\"files\":[");
  if (p->valid && p->found) {
    apiJsonLiteral(j, "{\"name\":");
    apiJsonString(j, p->name, sizeof p->name);
    apiJsonLiteral(j, ",\"bytes\":");
    apiJsonUnsigned(j, p->bytes);
    apiJsonLiteral(j, "}");
  }
  apiJsonLiteral(j, "]}");
}
#endif
int ICACHE_FLASH_ATTR apiPagesInfo(HttpdConnData *c) {
  if (!apiRead(c))
    return HTTPD_CGI_DONE;
#ifdef WEBSERVER
  Pages pages = {0};
  const char *path = c->url, *prefix = "/api/v1/pages";
  size_t n = os_strlen(prefix);
  if (os_strncmp(path, prefix, n))
    return apiError(c, 404, "PAGES_PATH", "Unknown pages path.");
  path += n;
  if (*path) {
    if (*path++ != '/')
      return apiError(c, 422, "PAGE_INDEX", "Use the inventory cursor.");
    if (!*path)
      return apiError(c, 422, "PAGE_INDEX", "Use the inventory cursor.");
    while (*path) {
      if (*path < '0' || *path > '9' || pages.index > 255)
        return apiError(c, 422, "PAGE_INDEX", "Use the inventory cursor.");
      pages.index = pages.index * 10 + *path++ - '0';
    }
    if (pages.index > 255)
      return apiError(c, 422, "PAGE_INDEX", "Use the inventory cursor.");
  }
  if (webServerUploadBusy())
    return apiError(c, 409, "UPLOAD_ACTIVE", "Wait for custom-page replacement to finish.");
  scanPages(&pages);
  return apiResponse(c, 200, pagesInfo, &pages);
#else
  return apiError(c, 404, "FEATURE_UNAVAILABLE", "Custom pages are not compiled.");
#endif
}
int ICACHE_FLASH_ATTR apiPagesUpload(HttpdConnData *c) {
#ifdef WEBSERVER
  return cgiWebServerSetupUpload(c);
#else
  return apiError(c, 404, "FEATURE_UNAVAILABLE", "Custom pages are not compiled.");
#endif
}
int ICACHE_FLASH_ATTR apiPagesClear(HttpdConnData *c) {
  char action[24];
  ApiField f = {"action", action, sizeof action, 0};
  if (!apiForm(c, &f, 1))
    return HTTPD_CGI_DONE;
  if (os_strcmp(action, "remove_all_pages"))
    return apiError(c, 422, "INVALID_ACTION", "Choose Remove all custom pages explicitly.");
#ifdef WEBSERVER
  if (!idle(c))
    return HTTPD_CGI_DONE;
  if (webServerUploadBusy())
    return apiError(c, 409, "UPLOAD_ACTIVE", "Wait for custom-page replacement to finish.");
  if (!deviceOperationBegin(c))
    return apiError(c, 409, "DEVICE_BUSY", "Wait for the current device operation.");
  uint32_t raw[4] = {ESPFS_MAGIC, FLAG_LASTFILE, 0, 0};
  if (!flashRegionErase(FLASH_REGION_PAGES, FLASH_PAGES_START) ||
      !flashRegionWrite(FLASH_REGION_PAGES, FLASH_PAGES_START, raw, sizeof raw)) {
    deviceOperationEnd(c);
    return apiError(c, 503, "CLEAR_FAILED",
                    "Custom-page removal failed; reload the inventory before retrying.");
  }
  deviceOperationEnd(c);
  WEB_Init();
  Pages pages = {0};
  scanPages(&pages);
  return apiResponse(c, 200, pagesInfo, &pages);
#else
  return apiError(c, 404, "FEATURE_UNAVAILABLE", "Custom pages are not compiled.");
#endif
}
