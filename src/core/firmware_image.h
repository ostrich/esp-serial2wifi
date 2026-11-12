#ifndef SERIAL2WIFI_FIRMWARE_IMAGE_H
#define SERIAL2WIFI_FIRMWARE_IMAGE_H

typedef struct {
  uint32_t length, seen, crc, inner_offset, remaining, previous_end;
  uint8_t outer[16], inner[8], segment[8], footer[4];
  uint8_t target, segments, segment_header_bytes, checksum, marker_position;
  uint8_t ui_marker_position, ui_digest_bytes;
  char ui_digest[64];
  bool ui_digest_found;
  bool header_ok, marker_found, checksum_ok, failed;
} FirmwareImage;

bool firmwareImageInit(FirmwareImage *image, uint32_t length, uint8_t target);
bool firmwareImageFeed(FirmwareImage *image, const uint8_t *data, uint32_t length);
bool firmwareImageFinish(const FirmwareImage *image);

#endif
