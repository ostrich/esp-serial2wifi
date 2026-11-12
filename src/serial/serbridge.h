#ifndef __SER_BRIDGE_H__
#define __SER_BRIDGE_H__

#include <ip_addr.h>
#include <c_types.h>
#include <espconn.h>

#define MAX_CONN 4
#define SER_BRIDGE_TIMEOUT 300 // 300 seconds = 5 minutes

// Payload budget, allocated on demand in immutable TCP-sized blocks.
// Heap pressure can reject a slow client before reaching this upper bound.
#define TX_BLOCK_BYTES 536
#define MAX_TXBUFFER (24 * TX_BLOCK_BYTES)

enum connModes {
  cmInit = 0,    // initialization mode: nothing received yet
  cmPGMInit,     // initialization mode for programming
  cmTransparent, // transparent mode
  cmPGM,         // Arduino/AVR/ARM programming mode
  cmTelnet,      // use telnet escape sequences for programming mode
};

typedef struct TxBlock {
  struct TxBlock *next;
  uint16_t length, sent;
  uint32_t end_sequence;
  char data[TX_BLOCK_BYTES];
} TxBlock;

typedef struct serbridgeConnData {
  struct espconn *conn;
  enum connModes conn_mode; // connection mode
  uint8_t telnet_state, telnet_baud_count;
  bool telnet_value_iac, programming_owned;
  uint8_t telnet_local, telnet_remote;
  bool telnet_console, telnet_cr, telnet_probe_iac;
  uint32_t telnet_baud;
  uint16 txbufferlen;
  uint16_t txbuffersent; // submitted bytes awaiting completion
  uint32_t tx_next_sequence, tx_acked_sequence;
  bool tx_sequence_initialized, tx_copy_cursor_valid;
  TxBlock *txhead, *txtail;
  bool tx_failed, tx_recovering, programming_pending;
  uint32_t tx_progress_at, tx_recovery_at;
  char *rxbuffer; // TCP input waiting for UART transmit space
  uint16_t rxbufferlen, rxbufferpos;
  bool rx_held;
  uint16_t txflights[4];
  uint32_t txflight_times[4];
  uint8_t txflighthead, txflightcount;
  uint32_t txlast_added; // latest UART byte, for bounded TCP coalescing
  bool readytosend;      // true, if txbuffer can be sent by espconn_sent
} serbridgeConnData;

typedef struct {
  uint32_t received, high_water, rejected, hold_errors, alloc_errors, max_callback;
  uint32_t hold_calls;
  uint32_t net_send_errors, net_drops, net_alloc_errors;
  uint32_t net_packets, net_bytes, net_max_ack_us, net_min_heap;
  uint32_t net_queue_high_water; // maximum queued payload for any one client, including flights
  int32_t net_last_error;
  uint32_t net_fail_pending, net_fail_flights, net_fail_age_us;
  uint32_t net_fail_sndbuf, net_fail_slots, net_fail_snd_nxt, net_fail_rcv_nxt;
} SerbridgeInputStats;
void serbridge_get_input_stats(SerbridgeInputStats *stats);
uint8_t serbridge_listener_mask(void);
uint8_t serbridge_client_count(void);
uint32_t serbridgePendingTx(void);
bool serbridgeHTTPExclusive(void);
void serbridgeSetHTTPExclusive(bool enabled);

// port1 is transparent&programming, second port is programming only
void ICACHE_FLASH_ATTR serbridgeInit(int port1, int port2);
void ICACHE_FLASH_ATTR serbridgeInitPins(void);
void ICACHE_FLASH_ATTR serbridgeUartCb(char *buf, short len);
void ICACHE_FLASH_ATTR serbridgeReset();

int ICACHE_FLASH_ATTR serbridgeInMCUFlashing();

// callback when receiving UART chars when in programming mode
extern void (*programmingCB)(char *buffer, short length);

#endif /* __SER_BRIDGE_H__ */
