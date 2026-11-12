#ifndef DEVICE_OPERATION_H
#define DEVICE_OPERATION_H
#include <stdbool.h>
/* Cooperative callbacks: claim before scheduling or touching serial/flash state. */
bool deviceOperationBegin(const void *owner);
bool deviceOperationOwned(const void *owner);
bool deviceOperationBusy(void);
void deviceOperationEnd(const void *owner);
#endif
