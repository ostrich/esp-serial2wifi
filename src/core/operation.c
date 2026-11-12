#include <esp8266.h>
#include "operation.h"
static const void *operationOwner;
bool ICACHE_FLASH_ATTR deviceOperationBegin(const void *owner) {
  if (!owner || operationOwner)
    return false;
  operationOwner = owner;
  return true;
}
bool ICACHE_FLASH_ATTR deviceOperationOwned(const void *owner) {
  return owner && operationOwner == owner;
}
bool ICACHE_FLASH_ATTR deviceOperationBusy(void) { return operationOwner != NULL; }
void ICACHE_FLASH_ATTR deviceOperationEnd(const void *owner) {
  if (deviceOperationOwned(owner))
    operationOwner = NULL;
}
