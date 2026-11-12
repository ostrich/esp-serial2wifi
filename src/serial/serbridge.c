// Copyright 2015 by Thorsten von Eicken, see LICENSE

#include "esp8266.h"

#include "uart.h"
#include "crc16.h"
#include "serbridge.h"
#include "sdk_rto.h"
#include "serled.h"
#include "config.h"
#include "operation.h"
#include "console.h"
#include "slip.h"
#ifdef SYSLOG
#include "syslog.h"
#else
#define syslog(X1...)
#endif

static struct espconn serbridgeConn1; // plain bridging port
static struct espconn serbridgeConn2; // programming port
static esp_tcp serbridgeTcp1, serbridgeTcp2;
static int8_t mcu_reset_pin, mcu_isp_pin;

uint8_t in_mcu_flashing; // for disabling slip during MCU flashing

void (*programmingCB)(char *buffer, short length) = NULL;

static sint8 sendtxbuffer(serbridgeConnData *conn);
static sint8 espbuffsend(serbridgeConnData *conn, const char *data, uint16 len);
static void releaseCopiedTxBlocks(serbridgeConnData *conn);

// Connection pool
serbridgeConnData connData[MAX_CONN];
static uint8_t listener_mask;
static bool http_programming_exclusive;
bool ICACHE_FLASH_ATTR serbridgeHTTPExclusive(void) { return http_programming_exclusive; }
void ICACHE_FLASH_ATTR serbridgeSetHTTPExclusive(bool enabled) {
  http_programming_exclusive = enabled;
}

uint8_t ICACHE_FLASH_ATTR serbridge_listener_mask(void) { return listener_mask; }
uint8_t ICACHE_FLASH_ATTR serbridge_client_count(void) {
  uint8_t count = 0;
  for (unsigned i = 0; i < MAX_CONN; i++)
    if (connData[i].conn)
      count++;
  return count;
}

// Current output backlog, including application flights. HTTP uses this only
// to yield reads; it never changes UART/TCP ownership or drops console bytes.
uint32_t ICACHE_FLASH_ATTR serbridgePendingTx(void) {
  uint32_t pending = 0;
  for (unsigned i = 0; i < MAX_CONN; i++)
    if (connData[i].conn && !connData[i].tx_failed)
      pending += connData[i].txbufferlen;
  return pending;
}

// TCP input must not busy-wait in the SDK's receive callback. Hold its
// receive window until the queued input has been fed into available UART
// FIFO space. A bounded timer callback leaves RX delivery and Wi-Fi runnable.
#define MAX_RXBUFFER (2 * 1460)
// Exported by SDK 2.0.0.p1's lwIP library but absent from its public header.
// Configure the native bounded application-send queue before pipelining.
extern sint8 espconn_tcp_set_buf_count(struct espconn *conn, uint8 count);
// Pinned SDK export: abort does not need to allocate/queue a graceful FIN.
extern sint8 espconn_abort(struct espconn *conn);
static ETSTimer uart_tx_timer;
static SerbridgeInputStats input_stats;
void ICACHE_FLASH_ATTR serbridge_get_input_stats(SerbridgeInputStats *stats) {
  *stats = input_stats;
}

//===== TCP -> UART

// Telnet protocol characters
#define IAC 255  // escape
#define DONT 254 // negotiation
#define DO 253   // negotiation
#define WILL 251 // negotiation
#define WONT 252 // negotiation
#define SB 250   // subnegotiation begin
#define SE 240   // subnegotiation end
#define BinaryOpt 0
#define EchoOpt 1
#define SgaOpt 3
#define ComPortOpt 44 // COM port options
#define SetBaud 1     // Set baud rate
#define SetDataSize 2 // Set data size
#define SetParity 3   // Set parity
#define SetStopSize 4 // Set stop-bit count
#define SetControl 5  // Set control lines
#define PurgeData 12  // Flush FIFO buffer(s)
#define PURGE_TX 2
#define BRK_REQ 4 // request current BREAK state
#define BRK_ON 5  // set BREAK (TX-line to LOW)
#define BRK_OFF 6 // reset BREAK
#define DTR_ON 8  // used here to reset microcontroller
#define DTR_OFF 9
#define RTS_ON 11 // used here to signal ISP (in-system-programming) to uC
#define RTS_OFF 12

// telnet state machine states
enum {
  TN_normal,
  TN_iac,
  TN_will,
  TN_start,
  TN_end,
  TN_comPort,
  TN_setControl,
  TN_setBaud,
  TN_setDataSize,
  TN_setParity,
  TN_purgeData,
  TN_setStopSize,
  TN_do,
  TN_dont,
  TN_wont
};
static serbridgeConnData *breakOwner;
static uint8_t tn_break;
static void ICACHE_FLASH_ATTR programmingState(serbridgeConnData *conn, bool enabled) {
  if (enabled == conn->programming_owned)
    return;
  conn->programming_owned = enabled;
  if (enabled)
    in_mcu_flashing++;
  else if (in_mcu_flashing)
    in_mcu_flashing--;
}
static void ICACHE_FLASH_ATTR breakState(serbridgeConnData *conn, bool enabled) {
  if (enabled) {
    if (breakOwner && breakOwner != conn)
      return;
    if (((READ_PERI_REG(UART_STATUS(UART0)) >> UART_TXFIFO_CNT_S) & UART_TXFIFO_CNT) != 0)
      return;
    if (flashConfig.swap_uart) {
      PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_GPIO15);
      GPIO_OUTPUT_SET(15, 0);
    } else {
      PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_GPIO1);
      GPIO_OUTPUT_SET(1, 0);
    }
    breakOwner = conn;
    tn_break = 1;
  } else if (breakOwner == conn) {
    if (flashConfig.swap_uart) {
      GPIO_OUTPUT_SET(15, 1);
      PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, 4);
    } else {
      GPIO_OUTPUT_SET(1, 1);
      PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD);
    }
    breakOwner = NULL;
    tn_break = 0;
  }
}

// Never send negotiation to an unidentified raw client. Stock telnet -8 sends
// BINARY first; its options identify the connection without changing port23 raw
// or programmer streams. Keep a split initial IAC until the next callback.
static int ICACHE_FLASH_ATTR telnetProbe(serbridgeConnData *conn, const uint8_t *data,
                                         unsigned len) {
  if (!len)
    return -1;
  if (conn->telnet_probe_iac) {
    conn->telnet_probe_iac = false;
    if (data[0] == DO || data[0] == WILL) {
      conn->telnet_state = TN_iac;
      return 1;
    }
    return 2; // raw: caller must restore the held IAC before this data
  }
  if (data[0] != IAC)
    return 0;
  if (len == 1) {
    conn->telnet_probe_iac = true;
    return -1;
  }
  return data[1] == DO || data[1] == WILL;
}
static uint8_t ICACHE_FLASH_ATTR telnetOptionBit(uint8_t option) {
  switch (option) {
  case BinaryOpt:
    return 1;
  case EchoOpt:
    return 2;
  case SgaOpt:
    return 4;
  case ComPortOpt:
    return 8;
  default:
    return 0;
  }
}
static void ICACHE_FLASH_ATTR telnetOption(serbridgeConnData *conn, uint8_t command,
                                           uint8_t option) {
  char response[3] = {IAC, command, option};
  espbuffsend(conn, response, 3);
}
static void ICACHE_FLASH_ATTR telnetConsole(serbridgeConnData *conn) {
  if (conn->telnet_console)
    return;
  conn->telnet_console = true;
  // ECHO is supplied by the target tty (including password echo suppression).
  // SGA selects immediate keystrokes instead of the client's local line editor.
  for (uint8_t option = EchoOpt; option <= SgaOpt; option += 2) {
    uint8_t bit = telnetOptionBit(option);
    if (!(conn->telnet_local & bit)) {
      conn->telnet_local |= bit;
      telnetOption(conn, WILL, option);
    }
  }
}
// Escape UART data only; option/subnegotiation replies already have wire framing.
static sint8 ICACHE_FLASH_ATTR telnetSendData(serbridgeConnData *conn, const char *data,
                                              uint16 len) {
  if (conn->conn_mode != cmTelnet)
    return espbuffsend(conn, data, len);
  uint16 start = 0;
  for (uint16 i = 0; i < len; i++) {
    uint8_t c = data[i];
    if (c == IAC || (c == '\r' && !(conn->telnet_local & 1))) {
      sint8 result = espbuffsend(conn, data + start, i - start + 1);
      if (result != ESPCONN_OK)
        return result;
      char escape = c == IAC ? IAC : 0;
      result = espbuffsend(conn, &escape, 1);
      if (result != ESPCONN_OK)
        return result;
      start = i + 1;
    }
  }
  return start < len ? espbuffsend(conn, data + start, len - start) : ESPCONN_OK;
}

// RFC2217 server replies use command+100 and escape data IAC bytes.
static void ICACHE_FLASH_ATTR telnetSettingReply(serbridgeConnData *conn, uint8_t command,
                                                 const uint8_t *value, unsigned length) {
  char response[14] = {IAC, SB, ComPortOpt, command + 100};
  unsigned used = 4;
  for (unsigned i = 0; i < length; i++) {
    response[used++] = value[i];
    if (value[i] == IAC)
      response[used++] = IAC;
  }
  response[used++] = IAC;
  response[used++] = SE;
  espbuffsend(conn, response, used);
}
static void ICACHE_FLASH_ATTR telnetApplyFormat(uint8_t bits, uint8_t parity, uint8_t stop) {
  int8_t oldBits = flashConfig.data_bits, oldParity = flashConfig.parity,
         oldStop = flashConfig.stop_bits;
  flashConfig.data_bits = bits;
  flashConfig.parity = parity;
  flashConfig.stop_bits = stop;
  if (!configSave()) {
    flashConfig.data_bits = oldBits;
    flashConfig.parity = oldParity;
    flashConfig.stop_bits = oldStop;
    return;
  }
  uart0_config(bits, parity, stop);
}
// process a buffer-full on a telnet connection
static uint16 ICACHE_FLASH_ATTR telnetUnwrap(serbridgeConnData *conn, uint8_t *inBuf, int len) {
  uint8_t state = conn->telnet_state;

  int i;
  for (i = 0; i < len; i++) {
    if (conn->tx_failed || conn->tx_recovering)
      break;
    uint8_t c = inBuf[i];
    if (state == TN_setBaud) {
      if (conn->telnet_value_iac) {
        conn->telnet_value_iac = false;
        if (c != IAC) {
          state = c == SE ? TN_normal : TN_end;
          conn->telnet_baud_count = 0;
          continue;
        }
      } else if (c == IAC) {
        conn->telnet_value_iac = true;
        continue;
      }
    }
    switch (state) {
    default:
    case TN_normal:
      if (c == IAC)
        state = TN_iac; // escape char: see what's next
      else {
        bool padding = conn->telnet_cr && c == 0 && !(conn->telnet_remote & 1);
        conn->telnet_cr = c == '\r' && !(conn->telnet_remote & 1);
        if (!padding)
          uart0_write_char(c);
      }
      break;
    case TN_iac:
      //os_printf("Telnet: IAC + %d\n", c);
      switch (c) {
      case IAC: // second escape -> write one to outbuf and go normal again
        state = TN_normal;
        conn->telnet_cr = false;
        uart0_write_char(c);
        break;
      case DO:
        state = TN_do;
        break;
      case WONT:
        state = TN_wont;
        break;
      case DONT:
        state = TN_dont;
        break;
      case WILL: // negotiation
        state = TN_will;
        break;
      case SB: // command sequence begin
        state = TN_start;
        break;
      case SE: // command sequence end
        state = TN_normal;
        break;
      default: // not sure... let's ignore
        uart0_write_char(IAC);
        uart0_write_char(c);
      }
      break;
    case TN_will:
    case TN_do: {
      uint8_t bit = telnetOptionBit(c);
      bool local = state == TN_do;
      bool accept = bit && (local || c != EchoOpt);
      uint8_t *enabled = local ? &conn->telnet_local : &conn->telnet_remote;
      if (!accept)
        telnetOption(conn, local ? WONT : DONT, c);
      else if (!(*enabled & bit)) {
        *enabled |= bit;
        telnetOption(conn, local ? WILL : DO, c);
      }
      if (accept && (c == BinaryOpt || c == EchoOpt || c == SgaOpt))
        telnetConsole(conn);
      state = TN_normal;
      break;
    }
    case TN_wont:
    case TN_dont: {
      uint8_t bit = telnetOptionBit(c);
      bool local = state == TN_dont;
      uint8_t *enabled = local ? &conn->telnet_local : &conn->telnet_remote;
      if (*enabled & bit) {
        *enabled &= ~bit;
        telnetOption(conn, local ? WONT : DONT, c);
      }
      state = TN_normal;
      break;
    }
    case TN_start: // in command seq, now comes the type of cmd
      if (c == ComPortOpt)
        state = TN_comPort;
      else
        state = TN_end; // an option we don't know, skip 'til the end seq
      break;
    case TN_end: // wait for end seq
      if (c == IAC)
        state = TN_iac; // simple wait to accept end or next escape seq
      break;
    case TN_comPort:
      switch (c) {
      case SetControl:
        state = TN_setControl;
        break;
      case SetDataSize:
        state = TN_setDataSize;
        break;
      case SetParity:
        state = TN_setParity;
        break;
      case SetStopSize:
        state = TN_setStopSize;
        break;
      case SetBaud:
        state = TN_setBaud;
        conn->telnet_baud_count = 0;
        conn->telnet_baud = 0;
        conn->telnet_value_iac = false;
        break;
      case PurgeData:
        state = TN_purgeData;
        break;
      default:
        state = TN_end;
        break;
      }
      break;
    case TN_purgeData: // purge FIFO-buffers
      switch (c) {
      case PURGE_TX:
        // TODO: flush TX buffer
        break;
      }
      state = TN_end;
      break;
    case TN_setControl: // switch control line and delay a tad
      switch (c) {
      case DTR_ON:
        if (mcu_reset_pin >= 0) {
#ifdef SERBR_DBG
          os_printf("Telnet: reset gpio%d\n", mcu_reset_pin);
#endif
          GPIO_OUTPUT_SET(mcu_reset_pin, 0);
          os_delay_us(100L);
        }
#ifdef SERBR_DBG
        else {
          os_printf("Telnet: reset: no pin\n");
        }
#endif
        break;
      case DTR_OFF:
        if (mcu_reset_pin >= 0) {
          GPIO_DIS_OUTPUT(mcu_reset_pin);
          os_delay_us(100L);
        }
        break;
      case RTS_ON:
        if (mcu_isp_pin >= 0) {
#ifdef SERBR_DBG
          os_printf("Telnet: ISP gpio%d LOW\n", mcu_isp_pin);
#endif
          GPIO_OUTPUT_SET(mcu_isp_pin, 0);
          os_delay_us(100L);
        }
#ifdef SERBR_DBG
        else {
          os_printf("Telnet: isp: no pin\n");
        }
#endif
        programmingState(conn, true);
        break;
      case RTS_OFF:
        if (mcu_isp_pin >= 0) {
#ifdef SERBR_DBG
          os_printf("Telnet: ISP gpio%d HIGH\n", mcu_isp_pin);
#endif
          GPIO_OUTPUT_SET(mcu_isp_pin, 1);
          os_delay_us(100L);
        }
        programmingState(conn, false);
        break;
      case BRK_REQ: {
        char respBuf[7] = {IAC, SB, ComPortOpt, SetControl, tn_break, IAC, SE};
        espbuffsend(conn, respBuf, 7);
#ifdef SERBR_DBG
        os_printf("Telnet: BREAK state requested: state = %d)\n", tn_break);
#endif
        break;
      }
      case BRK_ON:
        breakState(conn, true);
        break;
      case BRK_OFF:
        breakState(conn, false);
        break;
      }
      state = TN_end;
      break;
    case TN_setDataSize: {
      if (c >= 5 && c <= 8)
        telnetApplyFormat(c - 5, flashConfig.parity, flashConfig.stop_bits);
      uint8_t actual = flashConfig.data_bits + 5;
      telnetSettingReply(conn, SetDataSize, &actual, 1);
      state = TN_end;
      break;
    }
    case TN_setBaud:
      conn->telnet_baud = (conn->telnet_baud << 8) | c;
      if (++conn->telnet_baud_count == 4) {
        uint32_t requested = conn->telnet_baud;
        if (requested >= 300 && requested <= 1000000) {
          int32_t old = flashConfig.baud_rate;
          flashConfig.baud_rate = requested;
          if (configSave())
            uart0_baud(requested);
          else
            flashConfig.baud_rate = old;
        }
        uint32_t rate = flashConfig.baud_rate;
        uint8_t actual[4] = {rate >> 24, rate >> 16, rate >> 8, rate};
        telnetSettingReply(conn, SetBaud, actual, 4);
        state = TN_end;
      }
      break;
    case TN_setParity: {
      if (c >= 1 && c <= 3)
        telnetApplyFormat(flashConfig.data_bits,
                          c == 1   ? NONE_BITS
                          : c == 2 ? ODD_BITS
                                   : EVEN_BITS,
                          flashConfig.stop_bits);
      uint8_t actual = flashConfig.parity == NONE_BITS ? 1 : flashConfig.parity == ODD_BITS ? 2 : 3;
      telnetSettingReply(conn, SetParity, &actual, 1);
      state = TN_end;
      break;
    }
    case TN_setStopSize: {
      if (c >= 1 && c <= 3)
        telnetApplyFormat(flashConfig.data_bits, flashConfig.parity,
                          c == 1   ? ONE_STOP_BIT
                          : c == 2 ? TWO_STOP_BIT
                                   : ONE_HALF_STOP_BIT);
      uint8_t actual = flashConfig.stop_bits == ONE_STOP_BIT   ? 1
                       : flashConfig.stop_bits == TWO_STOP_BIT ? 2
                                                               : 3;
      telnetSettingReply(conn, SetStopSize, &actual, 1);
      state = TN_end;
      break;
    }
    }
  }
  conn->telnet_state = state;
  return i;
}

static bool ICACHE_FLASH_ATTR holdUartInput(serbridgeConnData *conn) {
  if (conn->rx_held)
    return true;
  if (espconn_recv_hold(conn->conn) != ESPCONN_OK) {
    input_stats.hold_errors++;
    espconn_disconnect(conn->conn);
    return false;
  }
  conn->rx_held = true;
  input_stats.hold_calls++;
  return true;
}

static bool ICACHE_FLASH_ATTR renewUartInputCredit(serbridgeConnData *conn) {
  if (conn->rx_held) {
    if (espconn_recv_unhold(conn->conn) != ESPCONN_OK) {
      input_stats.hold_errors++;
      espconn_disconnect(conn->conn);
      return false;
    }
    conn->rx_held = false;
  }
  // SDK 3 credits received bytes before invoking the application callback.
  // Re-arm now, before the next receive callback, after granting only drained
  // bytes. Holding inside the next application callback would be too late.
  return holdUartInput(conn);
}

static bool ICACHE_FLASH_ATTR queueUartInput(serbridgeConnData *conn, const char *data,
                                             uint16_t len) {
  if (conn->tx_failed)
    return false;
  uint16_t pending = conn->rxbufferlen - conn->rxbufferpos;
  input_stats.received += len;
  if (len > input_stats.max_callback)
    input_stats.max_callback = len;
  if ((uint32_t)pending + len > MAX_RXBUFFER) {
    input_stats.rejected++;
    espconn_disconnect(conn->conn);
    return false;
  }
  if (!holdUartInput(conn))
    return false;
  // Allocate once per connection. Reallocating on every partial TCP input
  // needs both old and new buffers at once and fragments the small SDK heap.
  if (conn->rxbuffer == NULL)
    conn->rxbuffer = os_malloc(MAX_RXBUFFER);
  if (conn->rxbuffer == NULL) {
    input_stats.alloc_errors++;
    espconn_disconnect(conn->conn);
    return false;
  }
  if (pending)
    os_memmove(conn->rxbuffer, conn->rxbuffer + conn->rxbufferpos, pending);
  os_memcpy(conn->rxbuffer + pending, data, len);
  conn->rxbufferlen = pending + len;
  if (conn->rxbufferlen > input_stats.high_water)
    input_stats.high_water = conn->rxbufferlen;
  conn->rxbufferpos = 0;
  return true;
}

static void ICACHE_FLASH_ATTR beginProgramming(serbridgeConnData *conn) {
  conn->programming_pending = false;
  if (mcu_reset_pin >= 0)
    GPIO_OUTPUT_SET(mcu_reset_pin, 0);
  os_delay_us(100L);
  if (mcu_isp_pin >= 0)
    GPIO_OUTPUT_SET(mcu_isp_pin, 0);
  os_delay_us(2000L);
  if (mcu_reset_pin >= 0)
    GPIO_DIS_OUTPUT(mcu_reset_pin);
  os_delay_us(1000L);
  conn->conn_mode = cmPGM;
  programmingState(conn, true);
  serledFlash(50);
  if (conn->rxbufferlen == conn->rxbufferpos)
    renewUartInputCredit(conn);
}

static void ICACHE_FLASH_ATTR pumpUartInput(void *arg) {
  (void)arg;
  for (int i = 0; i < MAX_CONN; i++) {
    serbridgeConnData *conn = &connData[i];
    if (conn->conn != NULL && (conn->txbufferlen || conn->tx_recovering))
      sendtxbuffer(conn); // Retry only when SDK credit checks deferred submission.
    if (conn->tx_failed || conn->tx_recovering)
      continue;
    if (conn->conn && conn->programming_pending)
      beginProgramming(conn);
    if (conn->conn == NULL || conn->rxbuffer == NULL || conn->rxbufferpos == conn->rxbufferlen)
      continue;
    uint32_t count = (READ_PERI_REG(UART_STATUS(UART0)) >> UART_TXFIFO_CNT_S) & UART_TXFIFO_CNT;
    if (count >= 100)
      return;
    uint16_t available = 100 - count;
    // A Telnet state transition can emit two bytes from one input byte.
    if (conn->conn_mode == cmTelnet)
      available /= 2;
    if (available > 96)
      available = 96;
    uint16_t remaining = conn->rxbufferlen - conn->rxbufferpos;
    uint16_t length = remaining < available ? remaining : available;
    if (length == 0)
      continue;
    char *data = conn->rxbuffer + conn->rxbufferpos;
    if (conn->conn_mode == cmTelnet)
      length = telnetUnwrap(conn, (uint8_t *)data, length);
    else
      uart0_tx_buffer(data, length);
    if (conn->tx_failed)
      continue;
    conn->rxbufferpos += length;
    if (conn->rxbufferpos == conn->rxbufferlen) {
      conn->rxbufferpos = conn->rxbufferlen = 0;
      renewUartInputCredit(conn);
    }
  }
}

// Generate a reset pulse for the attached microcontroller
void ICACHE_FLASH_ATTR serbridgeReset() {
  if (mcu_reset_pin >= 0) {
#ifdef SERBR_DBG
    os_printf("MCU reset gpio%d\n", mcu_reset_pin);
#endif
    GPIO_OUTPUT_SET(mcu_reset_pin, 0);
    os_delay_us(2000L); // esp8266 needs at least 1ms reset pulse, it seems...
    GPIO_DIS_OUTPUT(mcu_reset_pin);
  }
#ifdef SERBR_DBG
  else {
    os_printf("MCU reset: no pin\n");
  }
#endif
}

// Receive callback
static void ICACHE_FLASH_ATTR serbridgeRecvCb(void *arg, char *data, unsigned short len) {
  serbridgeConnData *conn = ((struct espconn *)arg)->reverse;
  //os_printf("Receive callback on conn %p\n", conn);
  if (conn == NULL || conn->tx_failed)
    return;

  bool startPGM = false;

  // At the start of a connection on the primary port we're in cmInit mode and we wait
  // for the first few characters to arrive in order to decide what type of connection this is..
  // The following if statements do this dispatch.
  if (conn->conn_mode == cmInit) {
    int probe = telnetProbe(conn, (const uint8_t *)data, len);
    if (probe < 0)
      return;
    if (probe == 2 && !queueUartInput(conn, "\xff", 1))
      return;

    // If the connection starts with the Arduino or ARM reset sequence we perform a RESET
    if (probe == 0 &&
        ((len == 2 && strncmp(data, "0 ", 2) == 0) || (len == 2 && strncmp(data, "?\n", 2) == 0) ||
         (len == 3 && strncmp(data, "?\r\n", 3) == 0))) {
      startPGM = true;
      conn->conn_mode = cmPGM;

      // If the connection starts with a telnet negotiation we will do telnet
    } else if (probe == 1) {
      conn->conn_mode = cmTelnet;
      // The probe retains TN_iac when the initial IAC was fragmented.
      // Negotiation bytes are consumed by telnetUnwrap
#ifdef SERBR_DBG
      os_printf("telnet mode\n");
#endif

      // Looks like a plain-vanilla connection!
    } else {
      conn->conn_mode = cmTransparent;
    }

    // if we start out in cmPGM mode due to a connection to the second port we need to do the
    // reset dance right away
  } else if (conn->conn_mode == cmPGMInit) {
    conn->conn_mode = cmPGM;
    startPGM = true;
  }

  // Defer reset pins as well as UART bytes while output is recovering.
  if (startPGM) {
    conn->programming_pending = true;
    return;
  }

  // Copy input while its SDK callback buffer is valid; transmit later.
  if (len > 0 && !queueUartInput(conn, data, len))
    return;

  serledFlash(50); // short blink on serial LED
}

//===== UART -> TCP

static void ICACHE_FLASH_ATTR releaseCopiedTxBlocks(serbridgeConnData *conn) {
  if (conn->tx_failed)
    return;
  while (conn->txhead && conn->txhead->sent == conn->txhead->length) {
    TxBlock *block = conn->txhead;
    if ((int32_t)(conn->tx_acked_sequence - block->end_sequence) < 0 &&
        (!conn->tx_copy_cursor_valid ||
         !sdk_tx_copy_complete(conn->conn, block->data, block->length, block->end_sequence)))
      break;
    conn->txhead = block->next;
    if (conn->txtail == block)
      conn->txtail = NULL;
    os_free(block);
  }
}

// A genuine SDK send error can have accepted a prefix. Fail closed instead
// of retrying it or silently continuing a truncated transparent stream.
static sint8 ICACHE_FLASH_ATTR failTxQueue(serbridgeConnData *conn, sint8 result) {
  if (!conn->tx_failed) {
    input_stats.net_fail_pending = conn->txbufferlen;
    input_stats.net_fail_flights = conn->txflightcount;
    input_stats.net_fail_age_us =
        conn->txflightcount ? system_get_time() - conn->txflight_times[conn->txflighthead] : 0;
    input_stats.net_fail_sndbuf = input_stats.net_fail_slots = 0xffffffff;
    input_stats.net_fail_snd_nxt = input_stats.net_fail_rcv_nxt = 0xffffffff;
    struct espconn_packet state;
    if (espconn_get_packet_info(conn->conn, &state) == ESPCONN_OK) {
      input_stats.net_fail_sndbuf = state.snd_buf_size;
      input_stats.net_fail_slots = state.snd_queuelen;
      input_stats.net_fail_snd_nxt = state.packseqno;
      input_stats.net_fail_rcv_nxt = state.packseq_nxt;
    }
    conn->tx_failed = true;
    conn->tx_recovering = conn->programming_pending = false;
    conn->readytosend = false;
    // Never execute more input from a session that can no longer show output.
    conn->rxbufferlen = conn->rxbufferpos = 0;
    // A graceful close can fail under the same memory pressure and resume RX
    // while espconn_disconnect still reports success. Abort releases the SDK
    // transport first; application buffers stay alive until its close callback.
    espconn_abort(conn->conn);
  }
  return result;
}

// Start recovery before finite output storage is exhausted. RX credit stays
// withheld while the UART pump is paused; TCP ACKs and retransmission still run.
// Threshold hysteresis avoids pausing input on every ordinary four-block flight.
static bool ICACHE_FLASH_ATTR recoverTxQueue(serbridgeConnData *conn) {
  if (conn->tx_failed)
    return false;
  uint32_t now = system_get_time(), age = now - conn->tx_progress_at;
  uint32_t heap = system_get_free_heap_size();
  if (conn->tx_recovering) {
    if (!conn->txbufferlen ||
        (conn->txbufferlen <= 2 * TX_BLOCK_BYTES && age < 250000U && heap >= 6144U))
      conn->tx_recovering = false;
    else if ((uint32_t)(now - conn->tx_recovery_at) >= 5000000U) {
      failTxQueue(conn, -128);
      return false;
    }
  } else if (conn->txbufferlen &&
             (conn->txbufferlen >= 6 * TX_BLOCK_BYTES || age >= 250000U || heap < 4096U)) {
    conn->tx_recovering = true;
    conn->tx_recovery_at = now;
  }
  return true;
}

static sint8 ICACHE_FLASH_ATTR sendtxbuffer(serbridgeConnData *conn) {
  releaseCopiedTxBlocks(conn);
  if (!recoverTxQueue(conn))
    return -128;
  if (!conn->readytosend)
    return ESPCONN_OK;
  while (conn->txflightcount < 4 && conn->txbufferlen > conn->txbuffersent) {
    TxBlock *block = conn->txhead;
    while (block && block->sent == block->length)
      block = block->next;
    if (!block)
      break;
    uint16_t pending = block->length - block->sent;
    if (block == conn->txtail && block->length < TX_BLOCK_BYTES &&
        system_get_time() - conn->txlast_added < 10000)
      break;
    struct espconn_packet credits;
    if (espconn_get_packet_info(conn->conn, &credits) != ESPCONN_OK || !credits.snd_buf_size ||
        credits.snd_queuelen < 2)
      break;
    uint16_t length = pending;
    if (length > credits.snd_buf_size)
      length = credits.snd_buf_size;
    uint32_t heap = system_get_free_heap_size();
    if (!input_stats.net_min_heap || heap < input_stats.net_min_heap)
      input_stats.net_min_heap = heap;
    sdk_rto_prepare_send(conn->conn);
    if (!conn->tx_sequence_initialized) {
      conn->tx_copy_cursor_valid = sdk_tx_sequence(conn->conn, &conn->tx_next_sequence);
      if (!conn->tx_copy_cursor_valid)
        conn->tx_next_sequence = 0;
      conn->tx_acked_sequence = conn->tx_next_sequence;
      conn->tx_sequence_initialized = true;
    }
    sint8 result = espconn_sent(conn->conn, (uint8_t *)block->data + block->sent, length);
    if (result != ESPCONN_OK) {
      input_stats.net_send_errors++;
      input_stats.net_last_error = result;
      return failTxQueue(conn, result);
    }
    sdk_rto_sent(conn->conn);
    uint8_t slot = (conn->txflighthead + conn->txflightcount) % 4;
    conn->txflights[slot] = length;
    conn->txflight_times[slot] = system_get_time();
    conn->txflightcount++;
    conn->txbuffersent += length;
    block->sent += length;
    conn->tx_next_sequence += length;
    block->end_sequence = conn->tx_next_sequence;
    input_stats.net_packets++;
    input_stats.net_bytes += length;
    releaseCopiedTxBlocks(conn);
  }
  conn->readytosend = conn->txflightcount < 4;
  return ESPCONN_OK;
}

static sint8 ICACHE_FLASH_ATTR espbuffsend(serbridgeConnData *conn, const char *data, uint16 len) {
  if (conn->tx_failed) {
    input_stats.net_drops += len;
    return -128;
  }
  releaseCopiedTxBlocks(conn);
  if (!conn->txbufferlen && len)
    conn->tx_progress_at = system_get_time();
  uint16_t remaining = len;
  while (remaining) {
    if (conn->txbufferlen == MAX_TXBUFFER) {
      input_stats.net_drops += remaining;
      return failTxQueue(conn, -128);
    }
    TxBlock *block = conn->txtail;
    // Never append to data already referenced by the SDK.
    if (!block || block->sent || block->length == TX_BLOCK_BYTES) {
      // Leave room for SDK bookkeeping and HTTP recovery/status requests.
      if (system_get_free_heap_size() < sizeof(TxBlock) + 2048)
        block = NULL;
      else
        block = os_malloc(sizeof(TxBlock));
      if (!block) {
        input_stats.net_alloc_errors++;
        input_stats.net_drops += remaining;
        return failTxQueue(conn, -128);
      }
      block->next = NULL;
      block->length = block->sent = 0;
      block->end_sequence = 0;
      if (conn->txtail)
        conn->txtail->next = block;
      else
        conn->txhead = block;
      conn->txtail = block;
    }
    uint16_t count = TX_BLOCK_BYTES - block->length;
    if (count > remaining)
      count = remaining;
    if (count > MAX_TXBUFFER - conn->txbufferlen)
      count = MAX_TXBUFFER - conn->txbufferlen;
    os_memcpy(block->data + block->length, data, count);
    block->length += count;
    conn->txbufferlen += count;
    if (conn->txbufferlen > input_stats.net_queue_high_water)
      input_stats.net_queue_high_water = conn->txbufferlen;
    data += count;
    remaining -= count;
    uint32_t heap = system_get_free_heap_size();
    if (!input_stats.net_min_heap || heap < input_stats.net_min_heap)
      input_stats.net_min_heap = heap;
  }
  conn->txlast_added = system_get_time();
  return sendtxbuffer(conn);
}

static void ICACHE_FLASH_ATTR serbridgeSentCb(void *arg) {
  serbridgeConnData *conn = ((struct espconn *)arg)->reverse;
  if (!conn || conn->tx_failed || !conn->txflightcount)
    return;
  sdk_rto_acked(conn->conn);
  uint8_t slot = conn->txflighthead;
  uint32_t latency = system_get_time() - conn->txflight_times[slot];
  if (latency > input_stats.net_max_ack_us)
    input_stats.net_max_ack_us = latency;
  uint16_t remaining = conn->txflights[slot];
  if (remaining > conn->txbufferlen || remaining > conn->txbuffersent) {
    input_stats.net_send_errors++;
    failTxQueue(conn, -128);
    return;
  }
  conn->tx_progress_at = system_get_time();
  conn->txbufferlen -= remaining;
  conn->txbuffersent -= remaining;
  conn->tx_acked_sequence += remaining;
  conn->txflighthead = (slot + 1) % 4;
  conn->txflightcount--;
  conn->readytosend = true;
  sendtxbuffer(conn);
}

// Only after the SDK has released its pointers in the disconnect callback.
static void ICACHE_FLASH_ATTR releaseTxQueue(serbridgeConnData *conn) {
  while (conn->txhead) {
    TxBlock *next = conn->txhead->next;
    os_free(conn->txhead);
    conn->txhead = next;
  }
  conn->txtail = NULL;
  conn->txbufferlen = conn->txbuffersent = 0;
  conn->txflightcount = conn->txflighthead = 0;
  conn->tx_sequence_initialized = conn->tx_copy_cursor_valid = false;
  conn->tx_failed = conn->tx_recovering = conn->programming_pending = false;
  conn->tx_progress_at = conn->tx_recovery_at = 0;
}

void ICACHE_FLASH_ATTR console_process(char *buf, short len) {
  // push buffer into web-console
  for (short i = 0; i < len; i++)
    console_write_char(buf[i]);
  // push the buffer into each open connection
  for (short i = 0; i < MAX_CONN; i++) {
    if (connData[i].conn) {
      telnetSendData(&connData[i], buf, len);
    }
  }
}

// callback with a buffer of characters that have arrived on the uart
void ICACHE_FLASH_ATTR serbridgeUartCb(char *buf, short length) {
  if (programmingCB) {
    programmingCB(buf, length);
  } else if (!flashConfig.slip_enable || in_mcu_flashing > 0) {
    //os_printf("SLIP: disabled got %d\n", length);
    console_process(buf, length);
  } else {
    slip_parse_buf(buf, length);
  }

  serledFlash(50); // short blink on serial LED
}

//===== Connect / disconnect

// Disconnection callback
static void ICACHE_FLASH_ATTR serbridgeDisconCb(void *arg) {
  serbridgeConnData *conn = ((struct espconn *)arg)->reverse;
  if (conn == NULL)
    return;
  ((struct espconn *)arg)->reverse = NULL;
  sdk_rto_detach((struct espconn *)arg);
  breakState(conn, false);
  programmingState(conn, false);
  // Free buffers
  releaseTxQueue(conn);
  if (conn->rxbuffer != NULL)
    os_free(conn->rxbuffer);
  conn->rxbuffer = NULL;
  conn->rxbufferlen = conn->rxbufferpos = 0;
  conn->rx_held = false;
  // Send reset to attached uC if it was in programming mode
  if (conn->conn_mode == cmPGM && mcu_reset_pin >= 0) {
    if (mcu_isp_pin >= 0)
      GPIO_OUTPUT_SET(mcu_isp_pin, 1);
    os_delay_us(100L);
    GPIO_OUTPUT_SET(mcu_reset_pin, 0);
    os_delay_us(100L);
    GPIO_DIS_OUTPUT(mcu_reset_pin);
  }
  conn->conn = NULL;
}

// Connection reset callback (note that there will be no DisconCb)
static void ICACHE_FLASH_ATTR serbridgeResetCb(void *arg, sint8 err) {
  os_printf("serbridge: connection reset err=%d\n", err);
  serbridgeDisconCb(arg);
}

// New connection callback, use one of the connection descriptors, if we have one left.
static void ICACHE_FLASH_ATTR serbridgeConnectCb(void *arg) {
  struct espconn *conn = arg;
  if (serbridgeHTTPExclusive() || deviceOperationBusy()) {
    espconn_disconnect(conn);
    return;
  }
  // Find empty conndata in pool
  int i;
  for (i = 0; i < MAX_CONN; i++)
    if (connData[i].conn == NULL)
      break;
#ifdef SERBR_DBG
  os_printf("Accept port %d, conn=%p, pool slot %d\n", conn->proto.tcp->local_port, conn, i);
#endif
  syslog(SYSLOG_FAC_USER, SYSLOG_PRIO_NOTICE, "esp-serial2wifi",
         "Accept port %d, conn=%p, pool slot %d\n", conn->proto.tcp->local_port, conn, i);
  if (i == MAX_CONN) {
#ifdef SERBR_DBG
    os_printf("Aiee, conn pool overflow!\n");
#endif
    syslog(SYSLOG_FAC_USER, SYSLOG_PRIO_WARNING, "esp-serial2wifi", "Aiee, conn pool overflow!\n");
    espconn_disconnect(conn);
    return;
  }

  os_memset(connData + i, 0, sizeof(struct serbridgeConnData));
  connData[i].conn = conn;
  conn->reverse = connData + i;
  connData[i].readytosend = true;
  connData[i].conn_mode = cmInit;
  // if it's the second port we start out in programming mode
  if (conn->proto.tcp->local_port == serbridgeConn2.proto.tcp->local_port)
    connData[i].conn_mode = cmPGMInit;

  espconn_regist_recvcb(conn, serbridgeRecvCb);
  espconn_regist_disconcb(conn, serbridgeDisconCb);
  espconn_regist_reconcb(conn, serbridgeResetCb);
  espconn_regist_sentcb(conn, serbridgeSentCb);
  if (espconn_tcp_set_buf_count(conn, 4) != ESPCONN_OK) {
    espconn_disconnect(conn);
    return;
  }

  sdk_rto_attach(conn);
  espconn_set_opt(conn, ESPCONN_REUSEADDR | ESPCONN_NODELAY);
  espconn_clear_opt(conn, ESPCONN_COPY); // Retain each source until proven copied or ACKed.
  holdUartInput(connData + i);           // withhold credit before the very first receive callback
}

//===== Initialization

void ICACHE_FLASH_ATTR serbridgeInitPins() {
  mcu_reset_pin = flashConfig.reset_pin;
  mcu_isp_pin = flashConfig.isp_pin;
#ifdef SERBR_DBG
  os_printf("Serbridge pins: reset=%d isp=%d swap=%d\n", mcu_reset_pin, mcu_isp_pin,
            flashConfig.swap_uart);
#endif

  if (flashConfig.swap_uart) {
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, 4); // RX
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, 4); // TX
    PIN_PULLUP_DIS(PERIPHS_IO_MUX_MTDO_U);
    if (flashConfig.rx_pullup)
      PIN_PULLUP_EN(PERIPHS_IO_MUX_MTCK_U);
    else
      PIN_PULLUP_DIS(PERIPHS_IO_MUX_MTCK_U);
    system_uart_swap();
  } else {
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, 0);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, 0);
    PIN_PULLUP_DIS(PERIPHS_IO_MUX_U0TXD_U);
    if (flashConfig.rx_pullup)
      PIN_PULLUP_EN(PERIPHS_IO_MUX_U0RXD_U);
    else
      PIN_PULLUP_DIS(PERIPHS_IO_MUX_U0RXD_U);
    system_uart_de_swap();
  }

  // set both pins to 1 before turning them on so we don't cause a reset
  if (mcu_isp_pin >= 0)
    GPIO_OUTPUT_SET(mcu_isp_pin, 1);
  if (mcu_reset_pin >= 0)
    GPIO_DIS_OUTPUT(mcu_reset_pin);
  // switch pin mux to make these pins GPIO pins
  if (mcu_reset_pin >= 0)
    makeGpio(mcu_reset_pin);
  if (mcu_isp_pin >= 0)
    makeGpio(mcu_isp_pin);
}

// Start transparent serial bridge TCP server on specified port (typ. 23)
void ICACHE_FLASH_ATTR serbridgeInit(int port1, int port2) {
  listener_mask = 0;
  os_timer_setfn(&uart_tx_timer, pumpUartInput, NULL);
  os_timer_arm(&uart_tx_timer, 1, 1);

  serbridgeInitPins();

  os_memset(connData, 0, sizeof(connData));
  os_memset(&serbridgeTcp1, 0, sizeof(serbridgeTcp1));
  os_memset(&serbridgeTcp2, 0, sizeof(serbridgeTcp2));

  // set-up the primary port for plain bridging
  serbridgeConn1.type = ESPCONN_TCP;
  serbridgeConn1.state = ESPCONN_NONE;
  serbridgeTcp1.local_port = port1;
  serbridgeConn1.proto.tcp = &serbridgeTcp1;

  espconn_regist_connectcb(&serbridgeConn1, serbridgeConnectCb);
  if (espconn_accept(&serbridgeConn1) == ESPCONN_OK)
    listener_mask |= 1;
  espconn_tcp_set_max_con_allow(&serbridgeConn1, MAX_CONN);
  espconn_regist_time(&serbridgeConn1, SER_BRIDGE_TIMEOUT, 0);

  // set-up the secondary port for programming
  serbridgeConn2.type = ESPCONN_TCP;
  serbridgeConn2.state = ESPCONN_NONE;
  serbridgeTcp2.local_port = port2;
  serbridgeConn2.proto.tcp = &serbridgeTcp2;

  espconn_regist_connectcb(&serbridgeConn2, serbridgeConnectCb);
  if (espconn_accept(&serbridgeConn2) == ESPCONN_OK)
    listener_mask |= 2;
  espconn_tcp_set_max_con_allow(&serbridgeConn2, MAX_CONN);
  espconn_regist_time(&serbridgeConn2, SER_BRIDGE_TIMEOUT, 0);
}

int ICACHE_FLASH_ATTR serbridgeInMCUFlashing() { return in_mcu_flashing; }
