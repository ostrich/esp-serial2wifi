#include "ui_storage.h"
#include <esp8266.h>
#if FLASH_EXTERNAL_UI
#include "asset_digest.h"
#include "espfs.h"
#include "ui_identity.h"
// Keep the identity in the executable image for package validation, even though
// the complete filesystem lives in an SPI-read bank outside the mapped window.
static const char identity[] = "ELUI-SHA256:" UI_SHA256_HEX;
static uint32_t ICACHE_FLASH_ATTR le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t ICACHE_FLASH_ATTR bank(uint8_t slot) {
  return slot ? FLASH_UI2_START : FLASH_UI1_START;
}
static bool ICACHE_FLASH_ATTR readBank(uint8_t slot, uint32_t offset, void *data, unsigned count) {
  uint32_t at = bank(slot) + offset;
  return flashRegionContains(slot ? FLASH_REGION_UI2 : FLASH_REGION_UI1, at, count) &&
         spi_flash_read(at, data, count) == SPI_FLASH_RESULT_OK;
}
bool ICACHE_FLASH_ATTR uiStorageHeader(const uint8_t *p, uint32_t total, uint8_t slot,
                                       uint32_t *app, uint32_t *ui) {
  if (os_memcmp(p, "ELUPD001", 8) || le32(p + 16) != slot)
    return false;
  *app = le32(p + 8);
  *ui = le32(p + 12);
  if (*app < 1024 || *app > FLASH_APP1_SIZE || (*app & 15) != 4 || *ui < 16 || (*ui & 3) ||
      *ui > FLASH_UI1_SIZE - UI_BANK_HEADER_BYTES)
    return false;
  if (total != 1024 + ((*app + 1023) & ~1023U) + ((*ui + 1023) & ~1023U))
    return false;
  for (unsigned i = 52; i < 1024; i++)
    if (p[i])
      return false;
  return true;
}
bool ICACHE_FLASH_ATTR uiStorageValidate(uint8_t slot, uint32_t length,
                                         const uint8_t expected[32]) {
  if (slot > 1 || length < 16 || (length & 3) || length > FLASH_UI1_SIZE - UI_BANK_HEADER_BYTES)
    return false;
  AssetDigest hash;
  uint32_t scratch[64];
  uint8_t digest[32];
  assetDigestInit(&hash);
  for (uint32_t at = 0; at < length;) {
    unsigned n = length - at;
    if (n > sizeof scratch)
      n = sizeof scratch;
    if (!readBank(slot, 4096 + at, scratch, n))
      return false;
    assetDigestFeed(&hash, scratch, n);
    at += n;
  }
  assetDigestFinish(&hash, digest);
  if (os_memcmp(digest, expected, 32))
    return false;
  // Validate every extent before ESPFS can traverse it. SHA-256 is integrity,
  // not authorization: malformed but correctly hashed filesystems still fail.
  for (uint32_t at = 0; at + 16 <= length;) {
    EspFsHeader h;
    if (!readBank(slot, 4096 + at, &h, sizeof h) || h.magic != ESPFS_MAGIC || h.compression)
      return false;
    if (h.flags & FLAG_LASTFILE)
      return h.flags == FLAG_LASTFILE && !h.nameLen && !h.fileLenComp && !h.fileLenDecomp &&
             at + 16 == length;
    if (h.flags & ~FLAG_GZIP || h.nameLen < 4 || h.nameLen > 256 || (h.nameLen & 3) ||
        h.fileLenComp < 0 || h.fileLenDecomp < 0)
      return false;
    uint32_t available = length - at - 16;
    if ((unsigned)h.nameLen > available)
      return false;
    available -= h.nameLen;
    if ((uint32_t)h.fileLenComp > available || ((h.fileLenComp + 3U) & ~3U) > available)
      return false;
    uint32_t names[64];
    if (!readBank(slot, 4096 + at + 16, names, h.nameLen))
      return false;
    const char *name = (const char *)names;
    bool zero = false;
    for (unsigned n = 0; n < (unsigned)h.nameLen; n++)
      if (!name[n]) {
        zero = true;
        break;
      }
    if (!zero || !name[0])
      return false;
    at += 16 + h.nameLen + ((h.fileLenComp + 3U) & ~3U);
  }
  return false;
}
bool ICACHE_FLASH_ATTR uiStorageCommit(uint8_t slot, uint32_t length, const uint8_t digest[32]) {
  uint32_t header[12] = {0};
  os_memcpy(header, "ELUI001", 8);
  header[2] = length;
  os_memcpy(header + 3, digest, 32);
  // Header sector was erased before upload. Publish only after readback/hash.
  return flashRegionWrite(slot ? FLASH_REGION_UI2 : FLASH_REGION_UI1, bank(slot), header,
                          sizeof header);
}
bool ICACHE_FLASH_ATTR uiStorageMount(void) {
  unsigned slot = system_upgrade_userbin_check();
  uint32_t header[12];
  uint8_t expected[32];
  if (slot > 1 || !readBank(slot, 0, header, sizeof header) || os_memcmp(header, "ELUI001", 8) ||
      header[11])
    return false;
  // Parse the compiled hexadecimal digest; referencing identity keeps it linked.
  for (unsigned i = 0; i < 32; i++) {
    char a = identity[12 + 2 * i], b = identity[13 + 2 * i];
    expected[i] = (a <= '9' ? a - '0' : a - 'a' + 10) * 16 + (b <= '9' ? b - '0' : b - 'a' + 10);
  }
  if (os_memcmp(header + 3, expected, 32) || !uiStorageValidate(slot, header[2], expected))
    return false;
  return espFsInit(firmwareFsCtx, (void *)(bank(slot) + 4096), ESPFS_FLASH) == ESPFS_INIT_RESULT_OK;
}
#endif
