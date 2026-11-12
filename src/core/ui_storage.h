#ifndef UI_STORAGE_H
#define UI_STORAGE_H
#include "flash_layout.h"
#define UI_BANK_HEADER_BYTES 4096U
#define UI_PACKAGE_HEADER_BYTES 1024U
#define UI_PACKAGE_LIMIT (UI_PACKAGE_HEADER_BYTES + FLASH_APP1_SIZE + 1024U + FLASH_UI1_SIZE)
bool uiStorageMount(void);
bool uiStorageValidate(uint8_t slot, uint32_t length, const uint8_t digest[32]);
bool uiStorageCommit(uint8_t slot, uint32_t length, const uint8_t digest[32]);
bool uiStorageHeader(const uint8_t *header, uint32_t total, uint8_t slot, uint32_t *app,
                     uint32_t *ui);
#endif
