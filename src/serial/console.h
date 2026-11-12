#ifndef CONSOLE_H
#define CONSOLE_H

#include "httpd.h"
#define CONSOLE_BUFFER_BYTES 1024

typedef struct {
  uint32_t boot, epoch, start, end, lost;
  unsigned offset, length;
  const char *reason;
  const void *stream; // live FIFO snapshot; NULL for the rolling capture
} ConsoleCapture;
void consoleCapture(ConsoleCapture *, bool supplied, uint32_t boot, uint32_t epoch,
                    uint32_t cursor);
uint8_t consoleCaptureByte(const ConsoleCapture *, unsigned index);
void consoleCaptureClear(void);
// One live consumer, starting at the current UART tail. Payload blocks are
// allocated only while needed and freed as they are copied into HTTP frames.
bool consoleStreamOpen(void);
void consoleStreamClose(void);
void consoleStreamCapture(ConsoleCapture *);
void consoleStreamConsume(unsigned length);
unsigned consoleStreamPending(void);
uint32_t consoleStreamHighWater(void);
uint32_t consoleStreamLost(void);
uint32_t consoleStreamTotalLost(void);
uint32_t consoleBootId(void);
void consoleInit(void);
void ICACHE_FLASH_ATTR console_write_char(char c);
int ajaxConsole(HttpdConnData *connData);
int ajaxConsoleReset(HttpdConnData *connData);
int ajaxConsoleClear(HttpdConnData *connData);
int ajaxConsoleBaud(HttpdConnData *connData);
int ajaxConsoleFormat(HttpdConnData *connData);
int ajaxConsoleSend(HttpdConnData *connData);
int tplConsole(HttpdConnData *connData, char *token, void **arg);

int ajaxConsoleStats(HttpdConnData *connData);

#endif
