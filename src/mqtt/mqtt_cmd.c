//
// MQTT Commands coming in from the attache microcontrollver over the serial port
//

#include <esp8266.h>
#include "mqtt.h"
#include "mqtt_client.h"
#include "mqtt_cmd.h"

#ifdef MQTTCMD_DBG
#define DBG(format, ...) do { os_printf(format, ## __VA_ARGS__); } while(0)
#else
#define DBG(format, ...) do { } while(0)
#endif

static bool blocked; // flag to prevent MQTT from sending on serial while trying to PGM uC

void ICACHE_FLASH_ATTR
mqtt_block() { blocked = true; }
void ICACHE_FLASH_ATTR
mqtt_unblock() { blocked = false; }

void ICACHE_FLASH_ATTR
cmdMqttConnectedCb(MQTT_Client* client) {
  if (blocked||!client->user_data) return;
  MqttCmdCb* cb = (MqttCmdCb*)client->user_data;
  DBG("MQTT: Connected Cb=%p\n", (void*)cb->connectedCb);
  cmdResponseStart(CMD_RESP_CB, cb->connectedCb, 0);
  cmdResponseEnd();
}

void ICACHE_FLASH_ATTR
cmdMqttDisconnectedCb(MQTT_Client* client) {
  if (blocked||!client->user_data) return;
  MqttCmdCb* cb = (MqttCmdCb*)client->user_data;
  DBG("MQTT: Disconnected cb=%p\n", (void*)cb->disconnectedCb);
  cmdResponseStart(CMD_RESP_CB, cb->disconnectedCb, 0);
  cmdResponseEnd();
}

void ICACHE_FLASH_ATTR
cmdMqttPublishedCb(MQTT_Client* client) {
  if (blocked||!client->user_data) return;
  MqttCmdCb* cb = (MqttCmdCb*)client->user_data;
  DBG("MQTT: Published cb=%p\n", (void*)cb->publishedCb);
  cmdResponseStart(CMD_RESP_CB, cb->publishedCb, 0);
  cmdResponseEnd();
}

void ICACHE_FLASH_ATTR
cmdMqttDataCb(MQTT_Client* client, const char* topic, uint32_t topic_len,
    const char* data, uint32_t data_len)
{
  if (blocked||!client->user_data) return;
  MqttCmdCb* cb = (MqttCmdCb*)client->user_data;
  DBG("MQTT: Data cb=%p topic=%s len=%u\n", (void*)cb->dataCb, topic, data_len);

  cmdResponseStart(CMD_RESP_CB, cb->dataCb, 2);
  cmdResponseBody(topic, topic_len);
  cmdResponseBody(data, data_len);
  cmdResponseEnd();
}

void ICACHE_FLASH_ATTR MQTTCMD_Lwt(CmdPacket *cmd) {
  CmdRequest req;cmdRequest(&req,cmd);if(cmdGetArgc(&req)!=4)return;
  char topic[129],message[129];unsigned n=cmdArgLen(&req);
  if(n>128||cmdPopArg(&req,topic,n))return;topic[n]=0;n=cmdArgLen(&req);
  if(n>128||cmdPopArg(&req,message,n))return;message[n]=0;uint8_t qos,retain;
  if(cmdPopArg(&req,&qos,1)||cmdPopArg(&req,&retain,1)||qos>2||retain>1)return;
  if(MQTT_InitLWT(&mqttClient,topic,message,qos,retain))MQTT_Reconnect(&mqttClient);
}
void ICACHE_FLASH_ATTR MQTTCMD_Publish(CmdPacket *cmd) {
  CmdRequest req;cmdRequest(&req,cmd);if(cmdGetArgc(&req)!=5)return;
  unsigned n=cmdArgLen(&req);char topic[129];if(n>128||cmdPopArg(&req,topic,n))return;topic[n]=0;
  n=cmdArgLen(&req);const char *data=(const char*)req.arg_ptr+2;cmdSkipArg(&req);
  uint16_t length;uint8_t qos,retain;
  if(cmdPopArg(&req,&length,2)||cmdPopArg(&req,&qos,1)||cmdPopArg(&req,&retain,1)||length>n||qos>2||retain>1)return;
  MQTT_Publish(&mqttClient,topic,data,length,qos,retain);
}
void ICACHE_FLASH_ATTR MQTTCMD_Subscribe(CmdPacket *cmd) {
  CmdRequest req;cmdRequest(&req,cmd);if(cmdGetArgc(&req)!=2)return;
  unsigned n=cmdArgLen(&req);char topic[129];uint32_t qos;
  if(n>128||cmdPopArg(&req,topic,n)||cmdPopArg(&req,&qos,4)||qos>2)return;topic[n]=0;
  MQTT_Subscribe(&mqttClient,topic,qos);
}
void ICACHE_FLASH_ATTR MQTTCMD_Setup(CmdPacket *cmd) {
  CmdRequest req;cmdRequest(&req,cmd);if(cmdGetArgc(&req)!=4)return;
  MqttCmdCb next;
  if(cmdPopArg(&req,&next.connectedCb,4)||cmdPopArg(&req,&next.disconnectedCb,4)||cmdPopArg(&req,&next.publishedCb,4)||cmdPopArg(&req,&next.dataCb,4))return;
  MqttCmdCb *cb=os_malloc(sizeof(*cb));if(!cb)return;*cb=next;
  if(mqttClient.user_data)os_free(mqttClient.user_data);mqttClient.user_data=cb;
  mqttClient.cmdConnectedCb=cmdMqttConnectedCb;mqttClient.cmdDisconnectedCb=cmdMqttDisconnectedCb;
  mqttClient.cmdPublishedCb=cmdMqttPublishedCb;mqttClient.cmdDataCb=cmdMqttDataCb;
  if(mqttClient.connState==MQTT_CONNECTED){if(cb->connectedCb)cmdMqttConnectedCb(&mqttClient);}
  else if(cb->disconnectedCb)cmdMqttDisconnectedCb(&mqttClient);
}
