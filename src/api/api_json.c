#include <esp8266.h>
#include "api_json.h"

void ICACHE_FLASH_ATTR apiJsonInit(ApiJson *writer, ApiJsonEmit emit, void *context) {
  writer->emit = emit;
  writer->context = context;
  writer->length = 0;
  writer->okay = 1;
}

static void ICACHE_FLASH_ATTR output(ApiJson *writer, const char *data, size_t length) {
  writer->length += length;
  if (writer->okay && writer->emit && !writer->emit(writer->context, data, length))
    writer->okay = 0;
}

typedef uint32_t ApiFlashWord __attribute__((__may_alias__));
void ICACHE_FLASH_ATTR apiFlashCopy(char *destination, const void *source, size_t length) {
  const volatile ApiFlashWord *words = source;
  for (size_t i = 0; i < length; i += 4) {
    uint32_t word = words[i / 4];
    for (unsigned byte = 0; byte < 4 && i + byte < length; byte++)
      destination[i + byte] = (char)(word >> (byte * 8));
  }
}
void ICACHE_FLASH_ATTR apiJsonFlash(ApiJson *writer, const void *source, size_t length) {
  if (!writer->emit) {
    writer->length += length;
    return;
  }
  char chunk[32];
  size_t offset = 0;
  while (offset < length) {
    size_t n = length - offset;
    if (n > sizeof(chunk))
      n = sizeof(chunk);
    apiFlashCopy(chunk, (const char *)source + offset, n);
    output(writer, chunk, n);
    offset += n;
  }
}

void ICACHE_FLASH_ATTR apiJsonRaw(ApiJson *writer, const char *value) {
  output(writer, value, os_strlen(value));
}

/* Return a complete Unicode scalar's UTF-8 width; reject overlong encodings,
 * surrogate halves, values above U+10FFFF and truncated byte sequences. */
static unsigned ICACHE_FLASH_ATTR utf8Width(const unsigned char *value, size_t left) {
  unsigned length, i;
  unsigned char first = value[0];
  if (first >= 0xc2 && first <= 0xdf)
    length = 2;
  else if (first >= 0xe0 && first <= 0xef)
    length = 3;
  else if (first >= 0xf0 && first <= 0xf4)
    length = 4;
  else
    return 0;
  if (length > left)
    return 0;
  for (i = 1; i < length; i++)
    if (value[i] < 0x80 || value[i] > 0xbf)
      return 0;
  if ((first == 0xe0 && value[1] < 0xa0) || (first == 0xed && value[1] >= 0xa0) ||
      (first == 0xf0 && value[1] < 0x90) || (first == 0xf4 && value[1] >= 0x90))
    return 0;
  return length;
}

void ICACHE_FLASH_ATTR apiJsonStringN(ApiJson *writer, const char *value, size_t length) {
  size_t offset = 0;
  apiJsonRaw(writer, "\"");
  while (offset < length) {
    unsigned char c = (unsigned char)value[offset];
    if (c >= 0x20 && c < 0x80 && c != '"' && c != '\\') {
      size_t start = offset++;
      while (offset < length && (unsigned char)value[offset] >= 0x20 &&
             (unsigned char)value[offset] < 0x80 && value[offset] != '"' && value[offset] != '\\')
        offset++;
      output(writer, value + start, offset - start);
    } else if (c >= 0x80) {
      unsigned width = utf8Width((const unsigned char *)value + offset, length - offset);
      if (width) {
        output(writer, value + offset, width);
        offset += width;
      } else {
        apiJsonRaw(writer, "\\ufffd");
        offset++;
      }
    } else {
      if (c == '"')
        apiJsonRaw(writer, "\\\"");
      else if (c == '\\')
        apiJsonRaw(writer, "\\\\");
      else if (c == '\n')
        apiJsonRaw(writer, "\\n");
      else if (c == '\r')
        apiJsonRaw(writer, "\\r");
      else if (c == '\t')
        apiJsonRaw(writer, "\\t");
      else {
        char escaped[7];
        os_sprintf(escaped, "\\u%04x", (unsigned)c);
        output(writer, escaped, 6);
      }
      offset++;
    }
  }
  apiJsonRaw(writer, "\"");
}

void ICACHE_FLASH_ATTR apiJsonString(ApiJson *writer, const char *value, size_t limit) {
  size_t length = 0;
  if (!value) {
    apiJsonRaw(writer, "null");
    return;
  }
  while (length < limit && value[length])
    length++;
  apiJsonStringN(writer, value, length);
}

void ICACHE_FLASH_ATTR apiJsonUnsigned(ApiJson *writer, uint32_t value) {
  char number[11];
  os_sprintf(number, "%u", (unsigned)value);
  apiJsonRaw(writer, number);
}

void ICACHE_FLASH_ATTR apiJsonSigned(ApiJson *writer, int32_t value) {
  char number[12];
  os_sprintf(number, "%d", (int)value);
  apiJsonRaw(writer, number);
}

void ICACHE_FLASH_ATTR apiJsonMillis(ApiJson *writer, uint32_t seconds, uint16_t millis) {
  if (millis > 999) {
    apiJsonRaw(writer, "null");
    return;
  }
  if (seconds) {
    char fraction[4];
    apiJsonUnsigned(writer, seconds);
    os_sprintf(fraction, "%03u", (unsigned)millis);
    output(writer, fraction, 3);
  } else
    apiJsonUnsigned(writer, millis);
}

void ICACHE_FLASH_ATTR apiJsonBool(ApiJson *writer, int value) {
  apiJsonRaw(writer, value ? "true" : "false");
}

void ICACHE_FLASH_ATTR apiJsonBase64(ApiJson *j, const void *data, unsigned length,
                                     uint8_t (*byte)(const void *, unsigned)) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (unsigned i = 0; i < length; i += 3) {
    unsigned n = length - i;
    uint32_t v = (uint32_t)byte(data, i) << 16;
    if (n > 1)
      v |= (uint32_t)byte(data, i + 1) << 8;
    if (n > 2)
      v |= byte(data, i + 2);
    char b[4] = {alphabet[v >> 18], alphabet[(v >> 12) & 63], n > 1 ? alphabet[(v >> 6) & 63] : '=',
                 n > 2 ? alphabet[v & 63] : '='};
    output(j, b, 4);
  }
}
