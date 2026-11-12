/*
Some flash handling cgi routines. Used for reading the existing flash and updating the ESPFS image.
*/

/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 * Heavily modified and enhanced by Thorsten von Eicken in 2015
 * ----------------------------------------------------------------------------
 */

#include <esp8266.h>
#include "cgi.h"
#include "cgiflash.h"
#include "flash_layout.h"
#include "firmware_image.h"
#include "ui_storage.h"
#include "api_request.h"
#include "operation.h"
#include "serbridge.h"
#include "serial_api.h"

#ifndef SERIAL2WIFI_OTA_ENABLED
#define SERIAL2WIFI_OTA_ENABLED 0
#endif
#ifndef SERIAL2WIFI_OTA_TESTING
#define SERIAL2WIFI_OTA_TESTING 0
#endif

// Normal OTA is limited to the hardware-tested SDK3 4MB layout.
// Other layouts require an explicit test build; physical geometry is checked below.
static char *ICACHE_FLASH_ATTR otaError(void) {
  if (!FLASH_LAYOUT_OTA_CAPABLE) return "Wi-Fi updates are not supported on this flash layout";
  if (!SERIAL2WIFI_OTA_TESTING && !(SERIAL2WIFI_OTA_ENABLED && FLASH_LAYOUT_MAP == 4 && FLASH_LAYOUT_BYTES == 4194304U)) return "Wi-Fi updates are disabled in this firmware";
  if (system_get_flash_size_map() != FLASH_LAYOUT_MAP || flashPhysicalSize() != FLASH_LAYOUT_BYTES)
    return "Flash geometry mismatch";
  return NULL;
}

const char *ICACHE_FLASH_ATTR firmwareOTAReason(void) { return otaError(); }

bool ICACHE_FLASH_ATTR firmwareOTASupported(void) { return otaError() == NULL; }

typedef struct {
  FirmwareImage image;
  FlashRegion region;
  uint32_t address, written;
  uint8_t slot;
  bool touched;
#if FLASH_EXTERNAL_UI
  uint32_t total, ui_length, ui_offset;
  uint8_t ui_digest[32];
  bool ui_touched;
#endif
} Upload;

static Upload *active_upload;
static int verified_slot = -1;
static bool activating;
static ETSTimer flash_reboot_timer;
#define UPLOAD_REJECTED ((void *)1)

static void ICACHE_FLASH_ATTR releaseUpload(HttpdConnData *connData, Upload *upload, bool invalidate) {
  if (upload && (void *)upload != UPLOAD_REJECTED) {
#if FLASH_EXTERNAL_UI
    if(invalidate&&upload->ui_touched)flashRegionErase(upload->slot?FLASH_REGION_UI2:FLASH_REGION_UI1,upload->slot?FLASH_UI2_START:FLASH_UI1_START);
#endif
    if (invalidate && upload->touched) flashRegionErase(upload->region, upload->address);
    if (active_upload == upload) active_upload = NULL;
    os_free(upload);
  }
  deviceOperationEnd(connData);
  connData->cgiPrivData = NULL;
}

static int ICACHE_FLASH_ATTR uploadError(HttpdConnData *connData, Upload *upload, int code, char *message) {
  releaseUpload(connData, upload, true);
  connData->cgiPrivData = UPLOAD_REJECTED;
  errorResponse(connData, code, message);
  return HTTPD_CGI_DONE;
}



int ICACHE_FLASH_ATTR cgiUploadFirmware(HttpdConnData *connData) {
  Upload *upload = connData->cgiPrivData;
  if (!connData->conn) { releaseUpload(connData, upload, true); return HTTPD_CGI_DONE; }
  if ((void *)upload == UPLOAD_REJECTED) return HTTPD_CGI_DONE;
  if(activating)return uploadError(connData,upload,409,"Firmware activation is pending");
  if(!upload&&!apiWrite(connData))return HTTPD_CGI_DONE;
  char *error = otaError();
  if (error) return uploadError(connData, upload, 400, error);
  if (!connData->post || connData->requestType != HTTPD_METHOD_POST ||
      !connData->post->buff || connData->post->buffLen <= 0 ||
      connData->post->received < connData->post->buffLen)
    return uploadError(connData, upload, 400, "Invalid upload request");
  uint32_t count = connData->post->buffLen;
  uint32_t received = connData->post->received;
  uint32_t total = connData->post->len;
  uint32_t offset = received - count;
  if (received > total || (count & 3) || (offset % 1024) || count > 1024 ||
      (received != total && count != 1024))
    return uploadError(connData, upload, 400, "Invalid upload chunk geometry");
  if (!upload) {
    if (offset != 0) return uploadError(connData, NULL, 400, "Missing upload start");
    if(active_upload||programmingCB||serbridgeInMCUFlashing()||serialInputPending()||serbridge_client_count()||!deviceOperationBegin(connData))
      return uploadError(connData,NULL,409,"Another device operation is active");
    uint8 current = system_upgrade_userbin_check();
    if (current > 1) return uploadError(connData, NULL, 409, "Unknown active slot");
    verified_slot = -1;
    upload = os_zalloc(sizeof(*upload));
    if (!upload) return uploadError(connData, NULL, 503, "Not enough memory for upload");
    upload->slot = 1 - current;
    upload->region = upload->slot == 0 ? FLASH_REGION_APP1 : FLASH_REGION_APP2;
    upload->address = upload->slot == 0 ? FLASH_APP1_START : FLASH_APP2_START;
#if FLASH_EXTERNAL_UI
    uint32_t app_length;
    if(count!=1024||!uiStorageHeader((uint8_t *)connData->post->buff,total,upload->slot,&app_length,&upload->ui_length))
      return uploadError(connData,upload,400,"Choose a complete firmware/UI update package for this slot");
    upload->total=total;upload->ui_offset=1024+((app_length+1023)&~1023U);
    os_memcpy(upload->ui_digest,connData->post->buff+20,32);
    if (!firmwareImageInit(&upload->image, app_length, upload->slot))
#else
    if (!firmwareImageInit(&upload->image, total, upload->slot))
#endif
      return uploadError(connData, upload, 400, "Unsupported image size or target");
    verified_slot = -1;
    active_upload = upload;
    connData->cgiPrivData = upload;
  }
#if FLASH_EXTERNAL_UI
  if (offset != upload->written || total != upload->total ||
      system_upgrade_userbin_check() != 1 - upload->slot)
    return uploadError(connData, upload, 409, "Upload state or slot changed");
  if (offset >= 1024 && offset < upload->ui_offset) {
    uint32_t app_at = offset - 1024, n = upload->image.length - app_at;
    if (n > count)
      n = count;
    for (unsigned i = n; i < count; i++)
      if (connData->post->buff[i])
        return uploadError(connData, upload, 400, "Invalid application padding");
    if (!firmwareImageFeed(&upload->image, (uint8_t *)connData->post->buff, n))
      return uploadError(connData, upload, 400, "Invalid application image");
    if (app_at + n == upload->image.length) {
      if (!firmwareImageFinish(&upload->image) || !upload->image.ui_digest_found)
        return uploadError(connData, upload, 400, "Incomplete application or missing UI identity");
      static const char hex[] = "0123456789abcdef";
      for (unsigned i = 0; i < 32; i++)
        if (upload->image.ui_digest[2 * i] != hex[upload->ui_digest[i] >> 4] ||
            upload->image.ui_digest[2 * i + 1] != hex[upload->ui_digest[i] & 15])
          return uploadError(connData, upload, 400, "Application and UI versions do not match");
    }
    uint32_t address = upload->address + app_at;
    upload->touched = true;
    if ((address % 4096 == 0 && !flashRegionErase(upload->region, address)) ||
        !flashRegionWrite(upload->region, address, connData->post->buff, n))
      return uploadError(connData, upload, 500, "Application write verification failed");
  } else if (offset >= upload->ui_offset) {
    uint32_t at = offset - upload->ui_offset, n = upload->ui_length - at;
    if (n > count)
      n = count;
    for (unsigned i = n; i < count; i++)
      if (connData->post->buff[i])
        return uploadError(connData, upload, 400, "Invalid UI padding");
    FlashRegion region = upload->slot ? FLASH_REGION_UI2 : FLASH_REGION_UI1;
    uint32_t bank = upload->slot ? FLASH_UI2_START : FLASH_UI1_START;
    if (!at) {
      upload->ui_touched = true;
      if (!flashRegionErase(region, bank))
        return uploadError(connData, upload, 500, "UI invalidation failed");
    }
    uint32_t address = bank + 4096 + at;
    if ((address % 4096 == 0 && !flashRegionErase(region, address)) ||
        !flashRegionWrite(region, address, connData->post->buff, n))
      return uploadError(connData, upload, 500, "UI write verification failed");
  }
  upload->written += count;
  if (received != total)
    return HTTPD_CGI_MORE;
  if (!uiStorageValidate(upload->slot, upload->ui_length, upload->ui_digest) ||
      !uiStorageCommit(upload->slot, upload->ui_length, upload->ui_digest))
    return uploadError(connData, upload, 400, "UI integrity or filesystem validation failed");
#else
  if (offset != upload->written || total != upload->image.length ||
      system_upgrade_userbin_check() != 1 - upload->slot ||
      !flashRegionContains(upload->region, upload->address + offset, count))
    return uploadError(connData, upload, 409, "Upload state or slot changed");
  if (!firmwareImageFeed(&upload->image, (uint8_t *)connData->post->buff, count) ||
      (received == total && !firmwareImageFinish(&upload->image)))
    return uploadError(connData, upload, 400, "Image header, SDK/layout identity or checksum is invalid");
  uint32_t address = upload->address + offset;
  upload->touched = true;
  if ((address % SPI_FLASH_SEC_SIZE == 0 && !flashRegionErase(upload->region, address)) ||
      !flashRegionWrite(upload->region, address, connData->post->buff, count))
    return uploadError(connData, upload, 500, "Flash erase/write verification failed");
  upload->written += count;
  if (received != total) return HTTPD_CGI_MORE;
#endif
  verified_slot = upload->slot;
  releaseUpload(connData, upload, false);
  httpdStartResponse(connData, 200);
  httpdHeader(connData, "Content-Length", "0");
  httpdEndHeaders(connData);
  return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR cgiRebootFirmware(HttpdConnData *connData) {
  if (!connData->conn) return HTTPD_CGI_DONE;
  if(!apiWrite(connData))return HTTPD_CGI_DONE;
  char *error = otaError();
  if (error) { errorResponse(connData, 400, error); return HTTPD_CGI_DONE; }
  if (activating||active_upload || verified_slot < 0 || system_upgrade_userbin_check() != 1 - verified_slot) {
    errorResponse(connData, 409, "No complete verified upload in this boot; upload again or recover serially");
    return HTTPD_CGI_DONE;
  }
  if(programmingCB||serbridgeInMCUFlashing()||serialInputPending()||serbridge_client_count()||!deviceOperationBegin(&flash_reboot_timer)) {errorResponse(connData,409,"Another device operation is active");return HTTPD_CGI_DONE;}
  activating=true;
  httpdStartResponse(connData, 200);
  httpdHeader(connData, "Content-Length", "0");
  httpdEndHeaders(connData);
  system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
  os_timer_disarm(&flash_reboot_timer);
  os_timer_setfn(&flash_reboot_timer, (os_timer_func_t *)system_upgrade_reboot, NULL);
  os_timer_arm(&flash_reboot_timer, 2000, 0);
  return HTTPD_CGI_DONE;
}
