// Copyright 2015 by Thorsten von Eicken, see LICENSE

#include <esp8266.h>
#include "uart.h"
#include "cgi.h"
#include "uart.h"
#include "serbridge.h"
#include "serled.h"
#include "config.h"
#include "console.h"

// Microcontroller console capturing the last 1024 characters received on the uart so
// they can be shown on a web page

// Buffer to hold concole contents.
// Invariants:
// - console_rd==console_wr <=> buffer empty
// - *console_rd == next char to read
// - *console_wr == next char to write
// - 0 <= console_xx < BUF_MAX
// - (console_wr+1)%BUF_MAX) == console_rd <=> buffer full
#define BUF_MAX CONSOLE_BUFFER_BYTES
static char console_buf[BUF_MAX];
static int console_wr, console_rd;
static uint32_t console_boot, console_epoch;
static uint32_t console_pos; // offset since reset of buffer

// Capture is a lossy history window. Live terminal output instead has an
// independent FIFO: an ordinary delayed TCP ACK must not overwrite unread data.
#define STREAM_BLOCK_BYTES 512
#define STREAM_BLOCK_LIMIT 16
#define STREAM_HEAP_RESERVE 6144
struct StreamBlock {
  struct StreamBlock *next;
  unsigned length;
  uint8_t data[STREAM_BLOCK_BYTES];
};
static struct StreamBlock *stream_head, *stream_tail;
static unsigned stream_offset, stream_bytes, stream_blocks;
static uint32_t stream_boot, stream_epoch, stream_start, stream_end;
static uint32_t stream_high_water, stream_lost, stream_total_lost;
static bool stream_active, stream_failed;
static void ICACHE_FLASH_ATTR streamFree(void) {
  while(stream_head){struct StreamBlock *next=stream_head->next;os_free(stream_head);stream_head=next;}
  stream_tail=NULL;stream_offset=stream_bytes=stream_blocks=0;
}
bool ICACHE_FLASH_ATTR consoleStreamOpen(void) {
  if(stream_active)return false;
  stream_active=true;stream_failed=false;stream_high_water=stream_lost=0;
  stream_boot=console_boot;stream_epoch=console_epoch;
  stream_start=stream_end=console_pos+(console_wr+BUF_MAX-console_rd)%BUF_MAX;
  return true;
}
void ICACHE_FLASH_ATTR consoleStreamClose(void){streamFree();stream_active=false;}
unsigned ICACHE_FLASH_ATTR consoleStreamPending(void){return stream_bytes;}
uint32_t ICACHE_FLASH_ATTR consoleStreamHighWater(void){return stream_high_water;}
uint32_t ICACHE_FLASH_ATTR consoleStreamLost(void){return stream_lost;}
uint32_t ICACHE_FLASH_ATTR consoleStreamTotalLost(void){return stream_total_lost;}
uint32_t ICACHE_FLASH_ATTR consoleBootId(void){return console_boot;}
static void ICACHE_FLASH_ATTR streamWrite(uint8_t byte) {
  if(!stream_active||stream_boot!=console_boot||stream_epoch!=console_epoch)return;
  stream_end++;
  if(stream_failed){stream_total_lost++;stream_lost++;stream_start=stream_end;return;}
  if(!stream_tail||stream_tail->length==STREAM_BLOCK_BYTES){
    struct StreamBlock *block=NULL;
    if(stream_blocks<STREAM_BLOCK_LIMIT&&system_get_free_heap_size()>=sizeof(*block)+STREAM_HEAP_RESERVE)block=os_malloc(sizeof(*block));
    if(!block){stream_lost=stream_bytes+1;stream_total_lost+=stream_lost;stream_failed=true;streamFree();stream_start=stream_end;return;}
    block->next=NULL;block->length=0;
    if(stream_tail)stream_tail->next=block;else stream_head=block;
    stream_tail=block;stream_blocks++;
  }
  stream_tail->data[stream_tail->length++]=byte;stream_bytes++;
  if(stream_bytes>stream_high_water)stream_high_water=stream_bytes;
}
void ICACHE_FLASH_ATTR consoleStreamCapture(ConsoleCapture *out) {
  os_memset(out,0,sizeof(*out));out->boot=console_boot;out->epoch=console_epoch;
  out->start=stream_start;out->end=stream_end;
  if(stream_boot!=console_boot)out->reason="restart";
  else if(stream_epoch!=console_epoch)out->reason="cleared";
  else if(stream_failed){out->reason="gap";out->lost=stream_lost;}
  if(out->reason){out->start=out->end;return;}
  out->length=stream_bytes>1023?1023:stream_bytes;out->end=out->start+out->length;
  out->stream=stream_head;out->offset=stream_offset;
}
void ICACHE_FLASH_ATTR consoleStreamConsume(unsigned length) {
  if(length>stream_bytes)return;
  stream_start+=length;stream_bytes-=length;
  while(length){unsigned available=stream_head->length-stream_offset;
    if(length<available){stream_offset+=length;break;}
    length-=available;struct StreamBlock *next=stream_head->next;os_free(stream_head);stream_head=next;stream_offset=0;stream_blocks--;
    if(!stream_head)stream_tail=NULL;
  }
}

static void ICACHE_FLASH_ATTR
console_write(char c) {
  console_buf[console_wr] = c;
  console_wr = (console_wr+1) % BUF_MAX;
  if (console_wr == console_rd) {
    // full, we write anyway and loose the oldest char
    console_rd = (console_rd+1) % BUF_MAX; // full, eat first char
    console_pos++;
  }
}

#if 0
// return previous character in console, 0 if at start
static char ICACHE_FLASH_ATTR
console_prev(void) {
  if (console_wr == console_rd) return 0;
  return console_buf[(console_wr-1+BUF_MAX)%BUF_MAX];
}
#endif

void ICACHE_FLASH_ATTR
console_write_char(char c) {
  //if (c == '\n' && console_prev() != '\r') console_write('\r'); // does more harm than good
  console_write(c);
  streamWrite((uint8_t)c);
}

/* Called from cooperative UART/HTTP tasks, never from the UART ISR. A response
 * is counted and emitted synchronously, so the ring cannot change between passes. */
void ICACHE_FLASH_ATTR consoleCapture(ConsoleCapture *out, bool supplied,
    uint32_t boot, uint32_t epoch, uint32_t cursor) {
  os_memset(out,0,sizeof(*out));
  out->boot=console_boot;out->epoch=console_epoch;
  out->length=(console_wr+BUF_MAX-console_rd)%BUF_MAX;
  out->start=console_pos;out->end=console_pos+out->length;out->offset=console_rd;
  if(!supplied)return;
  if(boot!=console_boot)out->reason="restart";
  else if(epoch!=console_epoch)out->reason="cleared";
  else {
    uint32_t distance=out->end-cursor;
    if(distance<=out->length) {
      unsigned skip=out->length-distance;
      out->offset=(out->offset+skip)%BUF_MAX;out->start=cursor;out->length=distance;
    } else if(distance<=0x7fffffffU) {
      out->reason="gap";out->lost=distance-out->length;
    } else out->reason="cursor_unavailable";
  }
}
uint8_t ICACHE_FLASH_ATTR consoleCaptureByte(const ConsoleCapture *window,unsigned index) {
  if(window->stream){
    const struct StreamBlock *block=window->stream;unsigned offset=window->offset+index;
    while(block&&offset>=block->length){offset-=block->length;block=block->next;}
    return block?block->data[offset]:0;
  }
  return (uint8_t)console_buf[(window->offset+index)%BUF_MAX];
}
void ICACHE_FLASH_ATTR consoleCaptureClear(void) {
  console_rd=console_wr=console_pos=0;console_epoch++;
}

int ICACHE_FLASH_ATTR
ajaxConsoleReset(HttpdConnData *connData) {
  if (connData->conn==NULL) return HTTPD_CGI_DONE; // Connection aborted. Clean up.
  jsonHeader(connData, 200);
  consoleCaptureClear();
  serbridgeReset();
  return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR
ajaxConsoleClear(HttpdConnData *connData) {
  if (connData->conn==NULL) return HTTPD_CGI_DONE; // Connection aborted. Clean up.
  jsonHeader(connData, 200);
  //reset buffer
  consoleCaptureClear();
  return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR
ajaxConsoleBaud(HttpdConnData *connData) {
  if (connData->conn==NULL) return HTTPD_CGI_DONE; // Connection aborted. Clean up.
  char buff[512];
  int len, status = 400;
  len = httpdFindArg(connData->getArgs, "rate", buff, sizeof(buff));
  if (len > 0) {
    int rate = atoi(buff);
    if (rate >= 300 && rate <= 1000000) {
      uart0_baud(rate);
      flashConfig.baud_rate = rate;
      status = configSave() ? 200 : 400;
    }
  } else if (connData->requestType == HTTPD_METHOD_GET) {
    status = 200;
  }

  jsonHeader(connData, status);
  os_sprintf(buff, "{\"rate\": %d}", flashConfig.baud_rate);
  httpdSend(connData, buff, -1);
  return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR
ajaxConsoleFormat(HttpdConnData *connData) {
  if (connData->conn==NULL) return HTTPD_CGI_DONE; // Connection aborted. Clean up.
  char buff[16];
  int len, status = 400;

  len = httpdFindArg(connData->getArgs, "fmt", buff, sizeof(buff));
  if (len == 3 && buff[0]>='5' && buff[0]<='8' &&
      (buff[1]=='N'||buff[1]=='E'||buff[1]=='O') && (buff[2]=='1'||buff[2]=='2')) {
    int c = buff[0];
    if (c >= '5' && c <= '8') flashConfig.data_bits = c - '5' + FIVE_BITS;
    if (buff[1] == 'N') flashConfig.parity = NONE_BITS;
    if (buff[1] == 'E') flashConfig.parity = EVEN_BITS;
    if (buff[1] == 'O') flashConfig.parity = ODD_BITS;
    if (buff[2] == '1') flashConfig.stop_bits = ONE_STOP_BIT;
    if (buff[2] == '2') flashConfig.stop_bits = TWO_STOP_BIT;
    uart0_config(flashConfig.data_bits, flashConfig.parity, flashConfig.stop_bits);
    status = configSave() ? 200 : 400;
  } else if (connData->requestType == HTTPD_METHOD_GET) {
    status = 200;
  }

  jsonHeader(connData, status);
  os_sprintf(buff, "{\"fmt\": \"%c%c%c\"}", flashConfig.data_bits + '5',
      flashConfig.parity == NONE_BITS ? 'N' : flashConfig.parity == ODD_BITS ? 'O' : 'E',
      flashConfig.stop_bits == TWO_STOP_BIT ? '2': '1');
  httpdSend(connData, buff, -1);
  return HTTPD_CGI_DONE;
}


int ICACHE_FLASH_ATTR
ajaxConsoleSend(HttpdConnData *connData) {
  if (connData->conn==NULL) return HTTPD_CGI_DONE; // Connection aborted. Clean up.
  char buff[2048];
  int len, status = 400;

  // figure out where to start in buffer based on URI param
  len = httpdFindArg(connData->getArgs, "text", buff, sizeof(buff));
  if (len > 0) {
    serledFlash(50); // short blink on serial LED
    uart0_tx_buffer(buff, len);
    status = 200;
  }

  jsonHeader(connData, status);
  return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR
ajaxConsole(HttpdConnData *connData) {
  if (connData->conn==NULL) return HTTPD_CGI_DONE; // Connection aborted. Clean up.
  char buff[2048];
  int len; // length of text in buff
  int console_len = (console_wr+BUF_MAX-console_rd) % BUF_MAX; // num chars in console_buf
  int start = 0; // offset onto console_wr to start sending out chars

  jsonHeader(connData, 200);

  // figure out where to start in buffer based on URI param
  len = httpdFindArg(connData->getArgs, "start", buff, sizeof(buff));
  if (len > 0) {
    start = atoi(buff);
    if (start < console_pos) {
      start = 0;
    } else if (start >= console_pos+console_len) {
      start = console_len;
    } else {
      start = start - console_pos;
    }
  }

  // start outputting
  len = os_sprintf(buff, "{\"len\":%d, \"start\":%d, \"text\": \"",
      console_len-start, console_pos+start);

  int rd = (console_rd+start) % BUF_MAX;
  while (len < 2040 && rd != console_wr) {
    uint8_t c = console_buf[rd];
    if (c == '\\' || c == '"') {
      buff[len++] = '\\';
      buff[len++] = c;
    } else if (c == '\r') {
      // this is crummy, but browsers display a newline for \r\n sequences
    } else if (c < ' ') {
      len += os_sprintf(buff+len, "\\u%04x", c);
    } else {
      buff[len++] = c;
    }
    rd = (rd + 1) % BUF_MAX;
  }
  os_strcpy(buff+len, "\"}"); len+=2;
  httpdSend(connData, buff, len);
  return HTTPD_CGI_DONE;
}

void ICACHE_FLASH_ATTR consoleInit() {
  console_wr = 0;
  console_rd = 0;console_pos=0;console_epoch=0;
  console_boot=os_random();if(!console_boot)console_boot=1;
}



// Read-only diagnostics; counters reset only when the ESP restarts.
int ICACHE_FLASH_ATTR
ajaxConsoleStats(HttpdConnData *connData) {
  if (connData->conn == NULL) return HTTPD_CGI_DONE;
  UartRxStats stats;
  uart_get_rx_stats(&stats);
  SerbridgeInputStats input;
  serbridge_get_input_stats(&input);
  char buff[1024];
  os_sprintf(buff,
    "{\"received\":%u,\"delivered\":%u,\"polled\":%u,"
    "\"ring_drops\":%u,\"fifo_overruns\":%u,\"framing_errors\":%u,"
    "\"framing_drops\":%u,\"task_post_failures\":%u,"
    "\"high_water\":%u,\"pending\":%u,\"uptime_us\":%u,"
    "\"tcp_received\":%u,\"tcp_high_water\":%u,\"tcp_rejected\":%u,"
    "\"tcp_hold_errors\":%u,\"tcp_alloc_errors\":%u,\"tcp_max_callback\":%u,\"tcp_hold_calls\":%u,"
    "\"net_send_errors\":%u,\"net_drops\":%u,\"net_alloc_errors\":%u,"
    "\"net_packets\":%u,\"net_bytes\":%u,\"net_max_ack_us\":%u,"
    "\"net_min_heap\":%u,\"net_last_error\":%d,\"net_queue_high_water\":%u,"
    "\"net_fail_pending\":%u,\"net_fail_flights\":%u,\"net_fail_age_us\":%u,"
    "\"net_fail_sndbuf\":%u,\"net_fail_slots\":%u,\"net_fail_snd_nxt\":%u,\"net_fail_rcv_nxt\":%u,"
    "\"heap\":%u,\"sleep_type\":%u}",
    stats.bytes_received, stats.bytes_delivered, stats.bytes_polled,
    stats.ring_drops, stats.fifo_overruns, stats.framing_errors,
    stats.framing_drops, stats.task_post_failures, stats.high_water,
    stats.pending, system_get_time(), input.received, input.high_water,
    input.rejected, input.hold_errors, input.alloc_errors, input.max_callback, input.hold_calls,
    input.net_send_errors, input.net_drops, input.net_alloc_errors,
    input.net_packets, input.net_bytes, input.net_max_ack_us,
    input.net_min_heap, input.net_last_error, input.net_queue_high_water,
    input.net_fail_pending, input.net_fail_flights, input.net_fail_age_us,
    input.net_fail_sndbuf, input.net_fail_slots, input.net_fail_snd_nxt, input.net_fail_rcv_nxt,
    system_get_free_heap_size(), wifi_get_sleep_type());
  jsonHeader(connData, 200);
  httpdSend(connData, buff, -1);
  return HTTPD_CGI_DONE;
}
