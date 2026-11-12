/* mqtt.c
*  Protocol: http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html
*
* Copyright (c) 2014-2015, Tuan PM <tuanpm at live dot com>
* All rights reserved.
*
* Modified by Thorsten von Eicken to make it fully callback based
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* * Redistributions of source code must retain the above copyright notice,
* this list of conditions and the following disclaimer.
* * Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
* * Neither the name of Redis nor the names of its contributors may be used
* to endorse or promote products derived from this software without
* specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*/

// TODO:
// Handle SessionPresent=0 in CONNACK and rexmit subscriptions
// Improve timeout for CONNACK, currently only has keep-alive timeout (maybe send artificial ping?)
// Allow messages that don't require ACK to be sent even when pending_buffer is != NULL
// Set dup flag in retransmissions

#include <esp8266.h>
#include "pktbuf.h"
#include "mqtt.h"
#include "netclient.h"

#ifdef MQTT_DBG
#define DBG_MQTT(format, ...) os_printf(format, ##__VA_ARGS__)
#else
#define DBG_MQTT(format, ...)                                                                      \
  do {                                                                                             \
  } while (0)
#endif

extern void dumpMem(void *buf, int len);

// max message size supported for receive
#define MQTT_MAX_RCV_MESSAGE 2048
// max message size for sending (except publish)
#define MQTT_MAX_SHORT_MESSAGE 512

#ifdef MQTT_DBG
static char *mqtt_msg_type[] = {
    "NULL",    "TYPE_CONNECT", "CONNACK",    "PUBLISH", "PUBACK",      "PUBREC",
    "PUBREL",  "PUBCOMP",      "SUBSCRIBE",  "SUBACK",  "UNSUBSCRIBE", "UNSUBACK",
    "PINGREQ", "PINGRESP",     "DISCONNECT", "RESV",
};
#endif

// forward declarations
static bool mqtt_enq_message(MQTT_Client *client, const uint8_t *data, uint16_t len);
static void mqtt_send_message(MQTT_Client *client);
static void mqtt_doAbort(MQTT_Client *client);

// Connection ownership survives settings replacement and asynchronous DNS/teardown.
typedef struct {
  struct espconn conn;
  esp_tcp tcp;
  MQTT_Client *client;
  PktBuf *flight;
  bool resolving, started;
} MqttLink;
static MqttLink *ICACHE_FLASH_ATTR mqttLink(void *arg) { return ((struct espconn *)arg)->reverse; }
static MQTT_Client *ICACHE_FLASH_ATTR mqttOwner(void *arg) {
  MqttLink *l = mqttLink(arg);
  return l ? l->client : NULL;
}
static void ICACHE_FLASH_ATTR mqttReleaseLink(MqttLink *l) {
  if (!l)
    return;
  l->conn.reverse = NULL;
  PktBuf_Release(l->flight);
  os_free(l);
}
static void ICACHE_FLASH_ATTR mqttRetry(MQTT_Client *c) {
  c->connState = TCP_RECONNECT_REQ;
  c->timeoutTick = c->reconTimeout;
  if (c->reconTimeout < 128)
    c->reconTimeout <<= 1;
}
// Deliver a publish message to the client
static void ICACHE_FLASH_ATTR deliver_publish(MQTT_Client *client, uint8_t *message,
                                              uint16_t length) {

  // parse the message into topic and data
  uint16_t topic_length = length;
  const char *topic = mqtt_get_publish_topic(message, &topic_length);
  uint16_t data_length = length;
  const char *data = mqtt_get_publish_data(message, &data_length);

  if (!topic || !data) {
    mqtt_doAbort(client);
    return;
  }
  // callback to client
  if (client->dataCb)
    client->dataCb(client, topic, topic_length, data, data_length);
  if (client->cmdDataCb)
    client->cmdDataCb(client, topic, topic_length, data, data_length);
}

/**
* @brief  Client received callback function.
* @param  arg: contain the ip link information
* @param  pdata: received data
* @param  len: the length of received data
* @retval None
*/
static void ICACHE_FLASH_ATTR mqtt_tcpclient_recv(void *arg, char *pdata, unsigned short len) {
  //os_printf("MQTT: recv CB\n");
  uint8_t msg_type;
  uint16_t msg_id;
  uint16_t msg_len;

  struct espconn *pCon = (struct espconn *)arg;
  MQTT_Client *client = mqttOwner(pCon);
  if (client == NULL)
    return; // aborted connection

  //os_printf("MQTT: Data received %d bytes\n", len);

  do {
    // append data to our buffer
    int avail = client->in_buffer_size - client->in_buffer_filled;
    if (len <= avail) {
      os_memcpy(client->in_buffer + client->in_buffer_filled, pdata, len);
      client->in_buffer_filled += len;
      len = 0;
    } else {
      os_memcpy(client->in_buffer + client->in_buffer_filled, pdata, avail);
      client->in_buffer_filled += avail;
      len -= avail;
      pdata += avail;
    }

    int total = mqtt_get_total_length(client->in_buffer, client->in_buffer_filled);
    if (total < 0 || total > client->in_buffer_size) {
      mqtt_doAbort(client);
      return;
    }
    if (!total || total > client->in_buffer_filled)
      break;
    msg_len = total;
    msg_type = mqtt_get_type(client->in_buffer);
    msg_id = mqtt_get_id(client->in_buffer, msg_len);
    unsigned flags = client->in_buffer[0] & 15;
    bool valid =
        msg_type == MQTT_MSG_TYPE_PUBLISH
            ? mqtt_get_qos(client->in_buffer) < 3
            : ((msg_type == MQTT_MSG_TYPE_PUBREL ? flags == 2 : flags == 0) &&
               ((msg_type == MQTT_MSG_TYPE_PINGRESP && msg_len == 2) ||
                (msg_type == MQTT_MSG_TYPE_SUBACK && msg_len >= 5) ||
                ((msg_type == MQTT_MSG_TYPE_CONNACK || msg_type == MQTT_MSG_TYPE_PUBACK ||
                  msg_type == MQTT_MSG_TYPE_PUBREC || msg_type == MQTT_MSG_TYPE_PUBREL ||
                  msg_type == MQTT_MSG_TYPE_PUBCOMP || msg_type == MQTT_MSG_TYPE_UNSUBACK) &&
                 msg_len == 4)));
    if (!valid) {
      mqtt_doAbort(client);
      return;
    }
    if (client->connState != MQTT_CONNECTED) {
      // why are we receiving something??
      DBG_MQTT("MQTT ERROR: recv in invalid state %d\n", client->connState);
      mqtt_doAbort(client);
      return;
    }

    // we are connected and are sending/receiving data messages
    uint8_t pending_msg_type = 0;
    uint16_t pending_msg_id = 0;
    if (client->pending_buffer != NULL) {
      pending_msg_type = mqtt_get_type(client->pending_buffer->data);
      pending_msg_id = mqtt_get_id(client->pending_buffer->data, client->pending_buffer->filled);
    }
    DBG_MQTT("MQTT: Recv type=%s id=%04X len=%d; Pend type=%s id=%02X\n", mqtt_msg_type[msg_type],
             msg_id, msg_len, mqtt_msg_type[pending_msg_type], pending_msg_id);

    switch (msg_type) {
    case MQTT_MSG_TYPE_CONNACK:
      client->connack_received = true;
      client->connack_code = msg_len == 4 && client->in_buffer[0] == 0x20 &&
                                     client->in_buffer[1] == 2 && client->in_buffer[2] <= 1 &&
                                     client->in_buffer[3] <= 5
                                 ? client->in_buffer[3]
                                 : 255;
      if (client->connack_code != 0) {
        MQTT_Reconnect(client);
        return;
      }
      //DBG_MQTT("MQTT: Connect successful\n");
      // callbacks for internal and external clients
      if (client->connectedCb)
        client->connectedCb(client);
      if (client->cmdConnectedCb)
        client->cmdConnectedCb(client);
      client->reconTimeout = 1; // reset the reconnect backoff
      break;

    case MQTT_MSG_TYPE_SUBACK:
      if (pending_msg_type == MQTT_MSG_TYPE_SUBSCRIBE && pending_msg_id == msg_id) {
        //DBG_MQTT("MQTT: Subscribe successful\n");
        client->pending_buffer = PktBuf_ShiftFree(client->pending_buffer);
      }
      break;

    case MQTT_MSG_TYPE_UNSUBACK:
      if (pending_msg_type == MQTT_MSG_TYPE_UNSUBSCRIBE && pending_msg_id == msg_id) {
        //DBG_MQTT("MQTT: Unsubscribe successful\n");
        client->pending_buffer = PktBuf_ShiftFree(client->pending_buffer);
      }
      break;

    case MQTT_MSG_TYPE_PUBACK: // ack for a publish we sent
      if (pending_msg_type == MQTT_MSG_TYPE_PUBLISH && pending_msg_id == msg_id) {
        //DBG_MQTT("MQTT: QoS1 Publish successful\n");
        client->pending_buffer = PktBuf_ShiftFree(client->pending_buffer);
      }
      break;

    case MQTT_MSG_TYPE_PUBREC: // rec for a publish we sent
      if (pending_msg_type == MQTT_MSG_TYPE_PUBLISH && pending_msg_id == msg_id) {
        //DBG_MQTT("MQTT: QoS2 publish cont\n");
        client->pending_buffer = PktBuf_ShiftFree(client->pending_buffer);
        // we need to send PUBREL
        mqtt_msg_pubrel(&client->mqtt_connection, msg_id);
        mqtt_enq_message(client, client->mqtt_connection.message.data,
                         client->mqtt_connection.message.length);
      }
      break;

    case MQTT_MSG_TYPE_PUBCOMP: // comp for a pubrel we sent (originally publish we sent)
      if (pending_msg_type == MQTT_MSG_TYPE_PUBREL && pending_msg_id == msg_id) {
        //DBG_MQTT("MQTT: QoS2 Publish successful\n");
        client->pending_buffer = PktBuf_ShiftFree(client->pending_buffer);
      }
      break;

    case MQTT_MSG_TYPE_PUBLISH: { // incoming publish
      // we may need to ACK the publish
      uint8_t msg_qos = mqtt_get_qos(client->in_buffer);
      DBG_MQTT("MQTT: Recv PUBLISH qos=%d bytes=%u\n", msg_qos, msg_len);
      if (msg_qos == 1)
        mqtt_msg_puback(&client->mqtt_connection, msg_id);
      if (msg_qos == 2)
        mqtt_msg_pubrec(&client->mqtt_connection, msg_id);
      if (msg_qos == 1 || msg_qos == 2) {
        mqtt_enq_message(client, client->mqtt_connection.message.data,
                         client->mqtt_connection.message.length);
      }
      // send the publish message to clients
      deliver_publish(client, client->in_buffer, msg_len);
    } break;

    case MQTT_MSG_TYPE_PUBREL: // rel for a rec we sent (originally publish received)
      if (pending_msg_type == MQTT_MSG_TYPE_PUBREC && pending_msg_id == msg_id) {
        //DBG_MQTT("MQTT: Cont QoS2 recv\n");
        client->pending_buffer = PktBuf_ShiftFree(client->pending_buffer);
        // we need to send PUBCOMP
        mqtt_msg_pubcomp(&client->mqtt_connection, msg_id);
        mqtt_enq_message(client, client->mqtt_connection.message.data,
                         client->mqtt_connection.message.length);
      }
      break;

    case MQTT_MSG_TYPE_PINGRESP:
      client->keepAliveAckTick = 0;
      break;
    }

    // Shift out the message and see whether we have another one
    if (msg_len < client->in_buffer_filled)
      os_memmove(client->in_buffer, client->in_buffer + msg_len,
                 client->in_buffer_filled - msg_len);
    if (mqttOwner(pCon) != client)
      return;
    client->in_buffer_filled -= msg_len;
  } while (client->in_buffer_filled > 0 || len > 0);

  // Send next packet out, if possible
  if (!client->sending && client->pending_buffer == NULL && client->msgQueue != NULL) {
    mqtt_send_message(client);
  }
}

/**
* @brief  Callback from TCP that previous send completed
* @param  arg: contain the ip link information
* @retval None
*/
static void ICACHE_FLASH_ATTR mqtt_tcpclient_sent_cb(void *arg) {
  MqttLink *link = mqttLink(arg);
  if (!link)
    return;
  PktBuf_Release(link->flight);
  link->flight = NULL;
  MQTT_Client *client = link->client;
  if (!client)
    return;
  // if the message we sent is not a "pending" one, we need to free the buffer
  if (client->sending_buffer != NULL) {
    PktBuf *buf = client->sending_buffer;
    //DBG_MQTT("PktBuf free %p l=%d\n", buf, buf->filled);
    PktBuf_Release(buf);
    client->sending_buffer = NULL;
  }
  client->sending = false;

  // send next message if one is queued and we're not expecting an ACK
  if (client->connState == MQTT_CONNECTED && client->pending_buffer == NULL &&
      client->msgQueue != NULL) {
    mqtt_send_message(client);
  }
}

/*
 * @brief: Timer function to handle timeouts
 */
static void ICACHE_FLASH_ATTR mqtt_timer(void *arg) {
  MQTT_Client *client = (MQTT_Client *)arg;
  //DBG_MQTT("MQTT: timer CB\n");

  switch (client->connState) {
  default:
    break;

  case MQTT_CONNECTED:
    // first check whether we're timing out for an ACK
    if (client->pending_buffer != NULL && --client->timeoutTick == 0) {
      // looks like we're not getting a response in time, abort the connection
      mqtt_doAbort(client);
      client->timeoutTick = 0; // trick to make reconnect happen in 1 second
      return;
    }

    // check whether our last keep-alive timed out
    if (client->keepAliveAckTick > 0 && --client->keepAliveAckTick == 0) {
      os_printf("\nMQTT ERROR: Keep-alive timed out\n");
      mqtt_doAbort(client);
      return;
    }

    // check whether we need to send a keep-alive message
    if (client->keepAliveTick > 0 && --client->keepAliveTick == 0) {
      // timeout: we need to send a ping message
      //DBG_MQTT("MQTT: Send keepalive\n");
      mqtt_msg_pingreq(&client->mqtt_connection);
      PktBuf *buf = PktBuf_New(client->mqtt_connection.message.length);
      if (!buf) {
        mqtt_doAbort(client);
        return;
      }
      os_memcpy(buf->data, client->mqtt_connection.message.data,
                client->mqtt_connection.message.length);
      buf->filled = client->mqtt_connection.message.length;
      client->msgQueue = PktBuf_Unshift(client->msgQueue, buf);
      mqtt_send_message(client);
      client->keepAliveTick = client->connect_info.keepalive;
      client->keepAliveAckTick = client->sendTimeout;
    }

    break;

  case TCP_RECONNECT_REQ:
    if (client->timeoutTick == 0 || --client->timeoutTick == 0) {
      // it's time to reconnect! start by re-enqueueing anything pending
      if (client->pending_buffer != NULL) {
        client->msgQueue = PktBuf_Unshift(client->msgQueue, client->pending_buffer);
        client->pending_buffer = NULL;
      }
      MQTT_Connect(client);
    }
  }
}

/**
 * @brief  Callback from SDK that socket is disconnected
 * @param  arg: contain the ip link information
 * @retval None
 */
static void ICACHE_FLASH_ATTR mqttEnded(void *arg) {
  MqttLink *link = mqttLink(arg);
  if (!link)
    return;
  MQTT_Client *client = link->client;
  if (client) {
    client->pCon = NULL;
    client->sending = false;
    PktBuf_Release(client->sending_buffer);
    client->sending_buffer = NULL;
    if (client->connState != MQTT_DISCONNECTED)
      mqttRetry(client);
  }
  mqttReleaseLink(link);
  if (client) {
    if (client->disconnectedCb)
      client->disconnectedCb(client);
    if (client->cmdDisconnectedCb)
      client->cmdDisconnectedCb(client);
  }
}
void ICACHE_FLASH_ATTR mqtt_tcpclient_discon_cb(void *arg) { mqttEnded(arg); }
static void ICACHE_FLASH_ATTR mqtt_tcpclient_recon_cb(void *arg, int8_t error) {
  (void)error;
  mqttEnded(arg);
}

/**
* @brief  Callback from SDK that socket is connected
* @param  arg: contain the ip link information
* @retval None
*/
static void ICACHE_FLASH_ATTR mqtt_tcpclient_connect_cb(void *arg) {
  struct espconn *pCon = (struct espconn *)arg;
  MQTT_Client *client = mqttOwner(pCon);
  if (client == NULL)
    return; // aborted connection
  client->connack_received = false;
  client->connack_code = 255;

  espconn_regist_disconcb(client->pCon, mqtt_tcpclient_discon_cb);
  espconn_regist_recvcb(client->pCon, mqtt_tcpclient_recv);
  espconn_regist_sentcb(client->pCon, mqtt_tcpclient_sent_cb);
  os_printf("MQTT: TCP connected to %s:%d\n", client->host, client->port);

  client->connState = MQTT_CONNECTED; // CONNACK status is tracked separately.
  // send MQTT connect message to broker
  mqtt_msg_connect(&client->mqtt_connection, &client->connect_info);
  if (!client->mqtt_connection.message.length) {
    mqtt_doAbort(client);
    return;
  }
  PktBuf *buf = PktBuf_New(client->mqtt_connection.message.length);
  if (!buf) {
    mqtt_doAbort(client);
    return;
  }
  os_memcpy(buf->data, client->mqtt_connection.message.data,
            client->mqtt_connection.message.length);
  buf->filled = client->mqtt_connection.message.length;
  client->msgQueue = PktBuf_Unshift(client->msgQueue, buf); // prepend to send (rexmit) queue
  mqtt_send_message(client);
  // MQTT state was established before the initial submission.
}

/**
 * @brief  Allocate and enqueue mqtt message, kick sending, if appropriate
 */
static bool ICACHE_FLASH_ATTR mqtt_enq_message(MQTT_Client *client, const uint8_t *data,
                                               uint16_t len) {
  if (!len)
    return false;
  PktBuf *buf = PktBuf_New(len);
  if (!buf)
    return false;
  os_memcpy(buf->data, data, len);
  buf->filled = len;
  client->msgQueue = PktBuf_Push(client->msgQueue, buf);

  if (client->connState == MQTT_CONNECTED && !client->sending && client->pending_buffer == NULL) {
    mqtt_send_message(client);
  }
  return true;
}

/**
 * @brief  Send out top message in queue onto socket
 */
static void ICACHE_FLASH_ATTR mqtt_send_message(MQTT_Client *client) {
  //DBG_MQTT("MQTT: Send_message\n");
  PktBuf *buf = client->msgQueue;
  if (buf == NULL || client->sending)
    return; // ahem...
  client->msgQueue = PktBuf_Shift(client->msgQueue);

  // get some details about the message
  uint16_t msg_type = mqtt_get_type(buf->data);
#ifdef MQTT_DBG
  uint8_t msg_id = mqtt_get_id(buf->data, buf->filled);
  os_printf("MQTT: Send type=%s id=%04X len=%d\n", mqtt_msg_type[msg_type], msg_id, buf->filled);
#if 0
  for (int i=0; i<buf->filled; i++) {
    if (buf->data[i] >= ' ' && buf->data[i] <= '~') os_printf("%c", buf->data[i]);
    else os_printf("\\x%02X", buf->data[i]);
  }
  os_printf("\n");
#endif
#endif

  MqttLink *link = mqttLink(client->pCon);
  if (!link || link->flight) {
    client->msgQueue = PktBuf_Unshift(client->msgQueue, buf);
    return;
  }
  client->sending = true;
  link->flight = buf;
  PktBuf_Retain(buf);
  // depending on whether it needs an ack we need to hold on to the message
  bool needsAck = (msg_type == MQTT_MSG_TYPE_PUBLISH && mqtt_get_qos(buf->data) > 0) ||
                  msg_type == MQTT_MSG_TYPE_PUBREL || msg_type == MQTT_MSG_TYPE_PUBREC ||
                  msg_type == MQTT_MSG_TYPE_SUBSCRIBE || msg_type == MQTT_MSG_TYPE_UNSUBSCRIBE ||
                  msg_type == MQTT_MSG_TYPE_PINGREQ;
  if (msg_type == MQTT_MSG_TYPE_PINGREQ) {
    client->pending_buffer = NULL; // we don't need to rexmit this one
    client->sending_buffer = buf;
  } else if (needsAck) {
    client->pending_buffer = buf; // remeber for rexmit on disconnect/reconnect
    client->sending_buffer = NULL;
    client->timeoutTick = client->sendTimeout + 1; // +1 to ensure full sendTireout seconds
  } else {
    client->pending_buffer = NULL;
    client->sending_buffer = buf;
    client->timeoutTick = 0;
  }
  client->keepAliveTick =
      client->connect_info.keepalive > 0 ? client->connect_info.keepalive + 1 : 0;
  if (espconn_sent(client->pCon, buf->data, buf->filled) != ESPCONN_OK)
    mqtt_doAbort(client);
}

/**
* @brief  DNS lookup for broker hostname completed, move to next phase
*/
static void ICACHE_FLASH_ATTR mqtt_dns_found(const char *name, ip_addr_t *ip, void *arg) {
  (void)name;
  MqttLink *link = mqttLink(arg);
  if (!link || !link->resolving)
    return;
  link->resolving = false;
  MQTT_Client *client = link->client;
  if (!client) {
    mqttReleaseLink(link);
    return;
  }
  if (!ip || !ip->addr) {
    client->pCon = NULL;
    mqttReleaseLink(link);
    mqttRetry(client);
    return;
  }
  os_memcpy(link->tcp.remote_ip, &ip->addr, 4);
  link->started = true;
  if (espconn_connect(&link->conn) != ESPCONN_OK) {
    client->pCon = NULL;
    mqttReleaseLink(link);
    mqttRetry(client);
  }
}

//===== publish / subscribe

static void ICACHE_FLASH_ATTR msg_conn_init(mqtt_connection_t *new_msg, mqtt_connection_t *old_msg,
                                            uint8_t *buf, uint16_t buflen) {
  new_msg->message_id = old_msg->message_id;
  new_msg->buffer = buf;
  new_msg->buffer_length = buflen;
}

/**
* @brief  MQTT publish function.
* @param  client: MQTT_Client reference
* @param  topic:  string topic will publish to
* @param  data:   buffer data send point to
* @param  data_length: length of data
* @param  qos:    qos
* @param  retain: retain
* @retval TRUE if success queue
*/
bool ICACHE_FLASH_ATTR MQTT_Publish(MQTT_Client *client, const char *topic, const char *data,
                                    uint16_t data_length, uint8_t qos, uint8_t retain) {
  // estimate the packet size to allocate a buffer
  if (!client->in_buffer || !client->mqtt_connection.buffer || !topic || qos > 2)
    return false;
  uint16_t topic_length = os_strlen(topic);
  // estimate: fixed hdr, pkt-id, topic length, topic, data, fudge
  uint32_t required = 3U + 2 + 2 + topic_length + data_length + 16;
  if (required > 65535)
    return false;
  uint16_t buf_len = required;
  PktBuf *buf = PktBuf_New(buf_len);
  if (buf == NULL) {
    os_printf("MQTT ERROR: Cannot allocate buffer for %d byte publish\n", buf_len);
    return FALSE;
  }
  // use a temporary mqtt_message_t pointing to our buffer, this is a bit of a mess because we
  // need to keep track of the message_id that is embedded in it
  mqtt_connection_t msg;
  msg_conn_init(&msg, &client->mqtt_connection, buf->data, buf_len);
  uint16_t msg_id;
  if (!mqtt_msg_publish(&msg, topic, data, data_length, qos, retain, &msg_id)->length) {
    os_printf("MQTT ERROR: Queuing Publish failed\n");
    PktBuf_Release(buf);
    return FALSE;
  }
  client->mqtt_connection.message_id = msg.message_id;
  if (msg.message.data != buf->data)
    os_memmove(buf->data, msg.message.data, msg.message.length);
  buf->filled = msg.message.length;

  DBG_MQTT("MQTT: Publish, topic: \"%s\", length: %d\n", topic, msg.message.length);
  //dumpMem(buf, buf_len);
  client->msgQueue = PktBuf_Push(client->msgQueue, buf);

  if (client->connState == MQTT_CONNECTED && !client->sending && client->pending_buffer == NULL) {
    mqtt_send_message(client);
  }
  return TRUE;
}

/**
* @brief  MQTT subscribe function.
* @param  client: MQTT_Client reference
* @param  topic:  string topic will subscribe
* @param  qos:    qos
* @retval TRUE if success queue
*/
bool ICACHE_FLASH_ATTR MQTT_Subscribe(MQTT_Client *client, char *topic, uint8_t qos) {
  if (!client->mqtt_connection.buffer || qos > 2)
    return false;
  uint16_t msg_id;
  if (!mqtt_msg_subscribe(&client->mqtt_connection, topic, qos, &msg_id)->length) {
    os_printf("MQTT ERROR: Queuing Subscribe failed (too long)\n");
    return FALSE;
  }
  DBG_MQTT("MQTT: Subscribe, topic: \"%s\"\n", topic);
  return mqtt_enq_message(client, client->mqtt_connection.message.data,
                          client->mqtt_connection.message.length);
}

//===== Initialization and connect/disconnect

/**
* @brief  MQTT initialization mqtt client function
* @param  client:        MQTT_Client reference
* @param  host:   Domain or IP string
* @param  port:   Port to connect
* @param  security:    1 for ssl, 0 for none
* @param  clientid:      MQTT client id
* @param  client_user:   MQTT client user
* @param  client_pass:   MQTT client password
* @param  keepAliveTime: MQTT keep alive timer, in second
* @param  cleanSession:  On connection, a client sets the "clean session" flag, which is sometimes also known as the "clean start" flag.
*                        If clean session is set to false, then the connection is treated as durable. This means that when the client
*                        disconnects, any subscriptions it has will remain and any subsequent QoS 1 or 2 messages will be stored until
*                        it connects again in the future. If clean session is true, then all subscriptions will be removed for the client
*                        when it disconnects.
* @retval None
*/
bool ICACHE_FLASH_ATTR MQTT_Init(MQTT_Client *client, char *host, uint32 port, uint8_t security,
                                 uint8_t timeout, char *id, char *user, char *password,
                                 uint16_t keepalive) {
  MQTT_Client next;
  os_memset(&next, 0, sizeof next);
  next.host = netString(host, os_strlen(host));
  next.connect_info.client_id = netString(id, os_strlen(id));
  next.connect_info.username = netString(user, os_strlen(user));
  next.connect_info.password = netString(password, os_strlen(password));
  next.in_buffer = os_zalloc(MQTT_MAX_RCV_MESSAGE);
  uint8_t *buffer = os_zalloc(MQTT_MAX_SHORT_MESSAGE);
  mqtt_msg_init(&next.mqtt_connection, buffer, MQTT_MAX_SHORT_MESSAGE);
  if (security || !next.host || !next.connect_info.client_id || !next.connect_info.username ||
      !next.connect_info.password || !next.in_buffer || !buffer) {
    MQTT_Free(&next);
    return false;
  }
  next.port = port;
  next.sendTimeout = timeout ? timeout : 1;
  next.reconTimeout = 1;
  next.connect_info.keepalive = keepalive;
  next.connect_info.clean_session = 1;
  next.in_buffer_size = MQTT_MAX_RCV_MESSAGE;
  *client = next;
  return true;
}

/**
 * @brief  MQTT Set Last Will Topic, must be called before MQTT_Connect
 */
bool ICACHE_FLASH_ATTR MQTT_InitLWT(MQTT_Client *client, char *will_topic, char *will_msg,
                                    uint8_t will_qos, uint8_t will_retain) {

  char *topic = netString(will_topic, os_strlen(will_topic)),
       *message = netString(will_msg, os_strlen(will_msg));
  if (!topic || !message) {
    if (topic)
      os_free(topic);
    if (message)
      os_free(message);
    return false;
  }
  if (client->connect_info.will_topic)
    os_free(client->connect_info.will_topic);
  if (client->connect_info.will_message)
    os_free(client->connect_info.will_message);
  client->connect_info.will_topic = topic;
  client->connect_info.will_message = message;
  client->connect_info.will_qos = will_qos;
  client->connect_info.will_retain = will_retain;

  return true;
}

/**
* @brief  Begin connect to MQTT broker
* @param  client: MQTT_Client reference
* @retval None
*/
void ICACHE_FLASH_ATTR MQTT_Connect(MQTT_Client *client) {
  if (client->pCon || !client->in_buffer || !client->mqtt_connection.buffer || !client->host ||
      !*client->host)
    return;
  MqttLink *link = os_zalloc(sizeof(*link));
  if (!link) {
    mqttRetry(client);
    goto timer;
  }
  link->conn.type = ESPCONN_TCP;
  link->conn.proto.tcp = &link->tcp;
  link->conn.reverse = link;
  link->client = client;
  link->tcp.local_port = espconn_port();
  link->tcp.remote_port = client->port;
  client->pCon = &link->conn;
  espconn_regist_connectcb(client->pCon, mqtt_tcpclient_connect_cb);
  espconn_regist_reconcb(client->pCon, mqtt_tcpclient_recon_cb);
  client->connState = TCP_CONNECTING;
  client->timeoutTick = 20;
  client->sending = false;
  client->in_buffer_filled = 0;
  if (UTILS_StrToIP(client->host, link->tcp.remote_ip)) {
    link->started = true;
    if (espconn_connect(client->pCon) != ESPCONN_OK) {
      client->pCon = NULL;
      mqttReleaseLink(link);
      mqttRetry(client);
    }
  } else {
    link->resolving = true;
    client->ip.addr = 0;
    int result = espconn_gethostbyname(client->pCon, client->host, &client->ip, mqtt_dns_found);
    if (result == ESPCONN_OK)
      mqtt_dns_found(client->host, &client->ip, client->pCon);
    else if (result != ESPCONN_INPROGRESS)
      mqtt_dns_found(client->host, NULL, client->pCon);
  }
timer:
  os_timer_disarm(&client->mqttTimer);
  os_timer_setfn(&client->mqttTimer, mqtt_timer, client);
  os_timer_arm(&client->mqttTimer, 1000, 1);
}
static void ICACHE_FLASH_ATTR mqtt_doAbort(MQTT_Client *client) {
  MqttLink *link = client->pCon ? mqttLink(client->pCon) : NULL;
  client->pCon = NULL;
  client->sending = false;
  mqttRetry(client);
  PktBuf_Release(client->sending_buffer);
  client->sending_buffer = NULL;
  if (link) {
    link->client = NULL;
    if (link->started)
      espconn_disconnect(&link->conn);
    else if (!link->resolving)
      mqttReleaseLink(link);
  }
  if (client->disconnectedCb)
    client->disconnectedCb(client);
  if (client->cmdDisconnectedCb)
    client->cmdDisconnectedCb(client);
}

void ICACHE_FLASH_ATTR MQTT_Reconnect(MQTT_Client *client) {
  DBG_MQTT("MQTT: Reconnect requested\n");
  if (client->connState == MQTT_DISCONNECTED)
    MQTT_Connect(client);
  else if (client->connState == MQTT_CONNECTED)
    mqtt_doAbort(client);
  // in other cases we're already in the reconnecting process
}

void ICACHE_FLASH_ATTR MQTT_Disconnect(MQTT_Client *client) {
  DBG_MQTT("MQTT: Disconnect requested\n");
  os_timer_disarm(&client->mqttTimer);
  if (client->connState == MQTT_DISCONNECTED)
    return;
  if (client->connState == TCP_RECONNECT_REQ) {
    client->connState = MQTT_DISCONNECTED;
    return;
  }
  mqtt_doAbort(client);
  //void *out_buffer = client->mqtt_connection.buffer;
  //if (out_buffer != NULL) os_free(out_buffer);
  client->connState = MQTT_DISCONNECTED; // ensure we don't automatically reconnect
}

void ICACHE_FLASH_ATTR MQTT_Free(MQTT_Client *client) {
  DBG_MQTT("MQTT: Free requested\n");
  MQTT_Disconnect(client);

  while (client->msgQueue)
    client->msgQueue = PktBuf_ShiftFree(client->msgQueue);
  PktBuf_Release(client->pending_buffer);
  client->pending_buffer = NULL;
  PktBuf_Release(client->sending_buffer);
  client->sending_buffer = NULL;
  if (client->user_data)
    os_free(client->user_data);
  client->user_data = NULL;
  if (client->connect_info.will_topic)
    os_free(client->connect_info.will_topic);
  if (client->connect_info.will_message)
    os_free(client->connect_info.will_message);
  if (client->host)
    os_free(client->host);
  client->host = NULL;

  if (client->connect_info.client_id)
    os_free(client->connect_info.client_id);
  if (client->connect_info.username)
    os_free(client->connect_info.username);
  if (client->connect_info.password)
    os_free(client->connect_info.password);
  os_memset(&client->connect_info, 0, sizeof(mqtt_connect_info_t));

  if (client->in_buffer)
    os_free(client->in_buffer);
  client->in_buffer = NULL;

  if (client->mqtt_connection.buffer)
    os_free(client->mqtt_connection.buffer);
  os_memset(&client->mqtt_connection, 0, sizeof(client->mqtt_connection));
}

void ICACHE_FLASH_ATTR MQTT_OnConnected(MQTT_Client *client, MqttCallback connectedCb) {
  client->connectedCb = connectedCb;
}

void ICACHE_FLASH_ATTR MQTT_OnDisconnected(MQTT_Client *client, MqttCallback disconnectedCb) {
  client->disconnectedCb = disconnectedCb;
}

void ICACHE_FLASH_ATTR MQTT_OnData(MQTT_Client *client, MqttDataCallback dataCb) {
  client->dataCb = dataCb;
}

void ICACHE_FLASH_ATTR MQTT_OnPublished(MQTT_Client *client, MqttCallback publishedCb) {
  client->publishedCb = publishedCb;
}
