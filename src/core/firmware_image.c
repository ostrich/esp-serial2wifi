#include <esp8266.h>
#include "flash_layout.h"
#include "firmware_image.h"

static uint32_t ICACHE_FLASH_ATTR little32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool ICACHE_FLASH_ATTR firmwareImageInit(FirmwareImage *image, uint32_t length, uint8_t target) {
  os_memset(image, 0, sizeof(*image));
  uint32_t limit = target == 0 ? FLASH_APP1_SIZE : FLASH_APP2_SIZE;
  if (target > 1 || length < 1024 || length > limit || (length & 15) != 4) return false;
  image->length = length;
  image->target = target;
  image->crc = 0xffffffffU;
  image->checksum = 0xef;
  return true;
}

bool ICACHE_FLASH_ATTR firmwareImageFeed(FirmwareImage *image, const uint8_t *data, uint32_t length) {
  static const char marker[] = SERIAL2WIFI_IMAGE_ABI;
  if (image->failed || !data || length > image->length - image->seen) return false;
  for (uint32_t n = 0; n < length; n++) {
    uint32_t at = image->seen++;
    uint8_t byte = data[n];
#if FLASH_EXTERNAL_UI
    if (!image->ui_digest_found) {
      static const char prefix[]="ELUI-SHA256:";
      if (image->ui_marker_position==sizeof(prefix)-1) {
        if ((byte>='0'&&byte<='9')||(byte>='a'&&byte<='f')) {
          image->ui_digest[image->ui_digest_bytes++]=byte;
          if(image->ui_digest_bytes==64)image->ui_digest_found=true;
        } else {image->ui_marker_position=0;image->ui_digest_bytes=0;}
      } else if(byte==(uint8_t)prefix[image->ui_marker_position])image->ui_marker_position++;
      else image->ui_marker_position=byte==(uint8_t)prefix[0]?1:0;
    }
#endif
    if (at >= image->length - 4) {
      image->footer[at - (image->length - 4)] = byte;
      continue;
    }
    image->crc ^= byte;
    for (unsigned bit = 0; bit < 8; bit++)
      image->crc = (image->crc >> 1) ^ (0xedb88320U & (0U - (image->crc & 1)));
    if (!image->marker_found) {
      if (byte == (uint8_t)marker[image->marker_position]) image->marker_position++;
      else image->marker_position = byte == (uint8_t)marker[0] ? 1 : 0;
      if (image->marker_position == sizeof(marker) - 1) image->marker_found = true;
    }
    if (at < sizeof(image->outer)) {
      image->outer[at] = byte;
      if (at == sizeof(image->outer) - 1) {
        uint32_t entry = little32(image->outer + 4);
        uint32_t irom_size = little32(image->outer + 12);
        if (image->outer[0] != 0xea || image->outer[1] != 4 || image->outer[2] != 0 ||
            image->outer[3] != image->target || (entry & 3) || entry < 0x40100000U || entry >= 0x40108000U ||
            little32(image->outer + 8) != 0 || (irom_size & 15) ||
            irom_size < 16 || irom_size > image->length - 64) goto bad;
        image->inner_offset = 16 + irom_size;
        image->header_ok = true;
      }
    } else if (at >= image->inner_offset && at < image->inner_offset + 8) {
      image->inner[at - image->inner_offset] = byte;
      if (at == image->inner_offset + 7 &&
          (image->inner[0] != 0xe9 || image->inner[1] != 3 || image->inner[2] > 3 ||
           (image->inner[3] >> 4) != FLASH_LAYOUT_MAP ||
           ((image->inner[3] & 15) > 2 && (image->inner[3] & 15) != 15) ||
           little32(image->inner + 4) != little32(image->outer + 4))) goto bad;
    } else if (at >= image->inner_offset + 8) {
      if (image->remaining) {
        image->checksum ^= byte;
        image->remaining--;
      } else if (image->segments < 3) {
        image->segment[image->segment_header_bytes++] = byte;
        if (image->segment_header_bytes == 8) {
          uint32_t address = little32(image->segment);
          uint32_t count = little32(image->segment + 4);
          uint32_t base = image->segments == 0 ? 0x40100000U : 0x3ffe8000U;
          uint32_t end = image->segments == 0 ? 0x40108000U : 0x3fffc000U;
          uint32_t entry = little32(image->outer + 4);
          if ((address & 3) || (count & 3) || address < base || address >= end ||
              image->seen > image->length - 5 ||
              count > end - address || count > image->length - 5 - image->seen ||
              (image->segments == 0 && (entry < address || entry - address >= count)) ||
              (image->segments == 2 && address < image->previous_end)) goto bad;
          image->previous_end = address + count;
          image->remaining = count;
          image->segments++;
          image->segment_header_bytes = 0;
        }
      } else if (at == image->length - 5) {
        if (byte != image->checksum) goto bad;
        image->checksum_ok = true;
      } else if (byte != 0 || image->length - 5 - at > 16) goto bad;
    }
  }
  return true;
bad:
  image->failed = true;
  return false;
}

bool ICACHE_FLASH_ATTR firmwareImageFinish(const FirmwareImage *image) {
  uint32_t crc = image->crc ^ 0xffffffffU;
  // The historical vendor tool serializes a transformed signed Python 2 CRC.
  crc = crc & 0x80000000U ? ~crc : crc + 1;
  return !image->failed && image->header_ok && image->marker_found && image->checksum_ok &&
      image->seen == image->length && image->segments == 3 && image->remaining == 0 &&
      image->segment_header_bytes == 0 && little32(image->footer) == crc;
}
