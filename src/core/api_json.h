#ifndef SERIAL2WIFI_API_JSON_H
#define SERIAL2WIFI_API_JSON_H
#include <stddef.h>
#include <stdint.h>

typedef int (*ApiJsonEmit)(void *context, const char *data, size_t length);
typedef struct {
  ApiJsonEmit emit; /* NULL counts bytes without writing. */
  void *context;
  size_t length;
  int okay;
} ApiJson;

#ifndef ICACHE_RODATA_ATTR
#define ICACHE_RODATA_ATTR
#endif
/* ESP flash permits aligned word loads. Pad each literal so even the last word
 * is within its object; never feed these pointers to byte-reading libc calls. */
#define API_FLASH_STORAGE(name, text)                                                              \
  static const char name[(sizeof(text) + 3) & ~3U] ICACHE_RODATA_ATTR                              \
  __attribute__((aligned(4))) = text
#define apiJsonLiteral(writer, text)                                                               \
  do {                                                                                             \
    API_FLASH_STORAGE(_literal, text);                                                             \
    apiJsonFlash(writer, _literal, sizeof(text) - 1);                                              \
  } while (0)
void apiFlashCopy(char *destination, const void *source, size_t length);
void apiJsonFlash(ApiJson *writer, const void *source, size_t length);

void apiJsonInit(ApiJson *writer, ApiJsonEmit emit, void *context);
void apiJsonRaw(ApiJson *writer, const char *value);
void apiJsonString(ApiJson *writer, const char *value, size_t limit);
void apiJsonStringN(ApiJson *writer, const char *value, size_t length);
void apiJsonUnsigned(ApiJson *writer, uint32_t value);
void apiJsonSigned(ApiJson *writer, int32_t value);
void apiJsonMillis(ApiJson *writer, uint32_t seconds, uint16_t millis);
void apiJsonBase64(ApiJson *, const void *, unsigned, uint8_t (*)(const void *, unsigned));
void apiJsonBool(ApiJson *writer, int value);
#endif
