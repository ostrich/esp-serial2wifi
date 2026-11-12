#ifndef API_TEST_TYPES
#define API_TEST_TYPES
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t uint8;
typedef int8_t sint8;
typedef uint16_t uint16;
typedef int16_t sint16;
typedef uint32_t uint32;
typedef int32_t sint32;
typedef int STATUS;
#define ICACHE_FLASH_ATTR
#define BIT(n) (1U << (n))
#define BIT2 BIT(2)
#define BIT3 BIT(3)
#define BIT4 BIT(4)
#define BIT5 BIT(5)
#endif
