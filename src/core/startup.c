#include <esp8266.h>
#include "flash_layout.h"

// The pinned SDK driver wrapper applies patch_array.h only for JEDEC ID
// 0x1560eb (TH25Q16, 2 MB), but keeps its 4416-byte array in DRAM on every
// device. Preserve that vendor wrapper for all 2 MB builds. Other capacities
// cannot match that ID and enter the same SDK core without the unused array.
// The layout registration also rejects a physical-capacity/map mismatch.
#if FLASH_LAYOUT_BYTES != 0x200000U
extern void call_user_start(void) __attribute__((noreturn));

// Must remain in IRAM: this is the image entry, before SDK initialization.
void __attribute__((noreturn)) call_user_start1(void) { call_user_start(); }
#endif
