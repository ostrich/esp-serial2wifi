// Copyright 2016 BeeGee; see LICENSE. Bounded ELClient socket services.
#include "esp8266.h"
#include "socket.h"
#include "netclient.h"
#define MAX_SOCKET 4
#define MAX_PEERS 4
#define RECEIVE_CHUNK 100

typedef struct {
  struct espconn *conn;
  uint16_t sent;
  bool sending;
} SocketPeer;
typedef struct {
  char *host, *data;
  struct espconn *pCon;
  ip_addr_t ip;
  uint32_t resp_cb;
  uint16_t port, length;
  uint8_t mode, index;
  bool resolving, connected, listening, failed;
  SocketPeer peers[MAX_PEERS];
} SocketClient;
static SocketClient socketClient[MAX_SOCKET];
static unsigned socketNum;
static void ICACHE_FLASH_ATTR socketNotify(SocketClient *c, unsigned kind, int16_t value) {
  uint8_t type = kind, index = c->index;
  cmdResponseStart(CMD_RESP_CB, c->resp_cb, 3);
  cmdResponseBody(&type, 1);
  cmdResponseBody(&index, 1);
  cmdResponseBody(&value, 2);
  cmdResponseEnd();
}
static SocketPeer *ICACHE_FLASH_ATTR socketPeer(SocketClient *c, struct espconn *p) {
  for (unsigned i = 0; i < MAX_PEERS; i++)
    if (c->peers[i].conn == p)
      return c->peers + i;
  return NULL;
}
static void ICACHE_FLASH_ATTR socketFinish(SocketClient *c) {
  if (!c->data)
    return;
  for (unsigned i = 0; i < MAX_PEERS; i++)
    if (c->peers[i].sending)
      return;
  os_free(c->data);
  c->data = NULL;
  socketNotify(c, c->failed ? USERCB_RECO : USERCB_SENT, c->failed ? ESPCONN_ABRT : c->length);
  if (c->mode == SOCKET_TCP_CLIENT && c->connected)
    espconn_disconnect(c->pCon);
}
static void ICACHE_FLASH_ATTR socketSendNext(SocketClient *c, SocketPeer *peer) {
  if (peer->sent == c->length) {
    peer->sending = false;
    socketFinish(c);
    return;
  }
  unsigned n = c->length - peer->sent;
  if (n > 1400)
    n = 1400;
  peer->sending = true;
  if (espconn_sent(peer->conn, (uint8_t *)c->data + peer->sent, n) != ESPCONN_OK) {
    c->failed = true;
    // A TCP error can follow partial acceptance. Keep storage until teardown.
    if (c->mode == SOCKET_UDP) {
      if (espconn_delete(c->pCon) == ESPCONN_OK) {
        c->connected = false;
        peer->sending = false;
        socketFinish(c);
      }
    } else
      espconn_disconnect(peer->conn);
    return;
  }
  peer->sent += n;
}
static void ICACHE_FLASH_ATTR socketclient_sent_cb(void *arg) {
  struct espconn *p = arg;
  SocketClient *c = p->reverse;
  if (!c || !c->data)
    return;
  SocketPeer *peer = socketPeer(c, p);
  if (peer && peer->sending)
    socketSendNext(c, peer);
}
static void ICACHE_FLASH_ATTR socketclient_recv_cb(void *arg, char *data, unsigned short length) {
  SocketClient *c = ((struct espconn *)arg)->reverse;
  if (!c)
    return;
  uint8_t type = USERCB_RECV, index = c->index;
  for (unsigned at = 0; at < length;) {
    uint16_t n = length - at;
    if (n > RECEIVE_CHUNK)
      n = RECEIVE_CHUNK;
    cmdResponseStart(CMD_RESP_CB, c->resp_cb, 4);
    cmdResponseBody(&type, 1);
    cmdResponseBody(&index, 1);
    cmdResponseBody(&n, 2);
    cmdResponseBody(data + at, n);
    cmdResponseEnd();
    at += n;
  }
  // TCP is a byte stream: a receive callback is not a complete response.
}
static void ICACHE_FLASH_ATTR socketRetire(void *arg, int16_t error) {
  struct espconn *p = arg;
  SocketClient *c = p->reverse;
  if (!c)
    return;
  SocketPeer *peer = socketPeer(c, p);
  if (error || (!peer && c->data))
    c->failed = true;
  if (peer) {
    if (peer->sending)
      c->failed = true;
    os_memset(peer, 0, sizeof(*peer));
  }
  if (c->mode != SOCKET_TCP_SERVER)
    c->connected = false;
  socketFinish(c);
  socketNotify(c, error ? USERCB_RECO : USERCB_CONN, error ? error : CONNSTAT_DIS);
}
static void ICACHE_FLASH_ATTR socketclient_discon_cb(void *arg) { socketRetire(arg, 0); }
static void ICACHE_FLASH_ATTR socketclient_recon_cb(void *arg, sint8 error) {
  socketRetire(arg, error);
}
static void ICACHE_FLASH_ATTR socketclient_connect_cb(void *arg) {
  struct espconn *p = arg;
  SocketClient *c = p->reverse;
  if (!c)
    return;
  SocketPeer *peer = NULL;
  for (unsigned i = 0; i < MAX_PEERS; i++)
    if (!c->peers[i].conn) {
      peer = c->peers + i;
      break;
    }
  if (!peer) {
    p->reverse = NULL;
    espconn_disconnect(p);
    return;
  }
  peer->conn = p;
  peer->sent = 0;
  c->connected = true;
  espconn_regist_disconcb(p, socketclient_discon_cb);
  espconn_regist_reconcb(p, socketclient_recon_cb);
  espconn_regist_sentcb(p, socketclient_sent_cb);
  espconn_regist_recvcb(p, socketclient_recv_cb);
  espconn_regist_time(p, 20, 0);
  socketNotify(c, USERCB_CONN, CONNSTAT_CON);
  if (c->mode != SOCKET_TCP_SERVER && c->data)
    socketSendNext(c, peer);
}
static void ICACHE_FLASH_ATTR socket_dns_found(const char *name, ip_addr_t *ip, void *arg) {
  (void)name;
  SocketClient *c = ((struct espconn *)arg)->reverse;
  if (!c || !c->resolving)
    return;
  c->resolving = false;
  if (!ip || !ip->addr) {
    c->failed = true;
    socketFinish(c);
    return;
  }
  os_memcpy(c->pCon->proto.tcp->remote_ip, &ip->addr, 4);
  if (espconn_connect(c->pCon) != ESPCONN_OK) {
    c->failed = true;
    socketFinish(c);
  }
}
static void ICACHE_FLASH_ATTR socketFree(SocketClient *c) {
  if (c->host)
    os_free(c->host);
  netConnectionFree(c->pCon);
  os_memset(c, 0, sizeof(*c));
}
void ICACHE_FLASH_ATTR SOCKET_Setup(CmdPacket *cmd) {
  CmdRequest req;
  cmdRequest(&req, cmd);
  SocketClient next = {0};
  int32_t result = -1;
  unsigned n = cmdArgLen(&req);
  if (cmdGetArgc(&req) != 3 || n > 128)
    goto done;
  next.host = os_zalloc(n + 1);
  if (!next.host || cmdPopArg(&req, next.host, n) || cmdPopArg(&req, &next.port, 2) ||
      cmdPopArg(&req, &next.mode, 1) || !next.port || next.mode > SOCKET_UDP ||
      (next.mode != SOCKET_TCP_SERVER && !next.host[0]))
    goto done;
  unsigned index = socketNum % MAX_SOCKET;
  SocketClient *c = socketClient + index;
  if (c->resolving || c->data || (c->connected && c->mode != SOCKET_UDP) || c->listening)
    goto done;
  next.pCon = netConnectionNew(next.mode == SOCKET_UDP ? ESPCONN_UDP : ESPCONN_TCP, next.port, c);
  if (!next.pCon)
    goto done;
  if (next.mode == SOCKET_UDP && !UTILS_StrToIP(next.host, next.pCon->proto.udp->remote_ip))
    goto done;
  if (c->connected && espconn_delete(c->pCon) != ESPCONN_OK)
    goto done;
  next.resp_cb = cmd->value;
  next.index = index;
  socketFree(c);
  *c = next;
  os_memset(&next, 0, sizeof next);
  if (c->mode == SOCKET_UDP) {
    wifi_set_broadcast_if(STATIONAP_MODE);
    c->pCon->proto.udp->local_port = c->port;
    espconn_regist_sentcb(c->pCon, socketclient_sent_cb);
    espconn_regist_recvcb(c->pCon, socketclient_recv_cb);
    if (espconn_create(c->pCon) != ESPCONN_OK) {
      socketFree(c);
      goto done;
    }
    c->connected = true;
    c->peers[0].conn = c->pCon;
  } else {
    espconn_regist_connectcb(c->pCon, socketclient_connect_cb);
    espconn_regist_reconcb(c->pCon, socketclient_recon_cb);
    if (c->mode == SOCKET_TCP_SERVER) {
      c->pCon->proto.tcp->local_port = c->port;
      if (espconn_accept(c->pCon) != ESPCONN_OK) {
        socketFree(c);
        goto done;
      }
      espconn_tcp_set_max_con_allow(c->pCon, MAX_PEERS);
      c->listening = true;
    }
  }
  socketNum = (index + 1) % MAX_SOCKET;
  result = index;
done:
  socketFree(&next);
  cmdResponseStart(CMD_RESP_V, result, 0);
  cmdResponseEnd();
}
void ICACHE_FLASH_ATTR SOCKET_Send(CmdPacket *cmd) {
  CmdRequest req;
  cmdRequest(&req, cmd);
  if (cmd->value >= MAX_SOCKET)
    return;
  SocketClient *c = socketClient + cmd->value;
  if (!c->pCon || c->data || c->resolving || (cmd->argc != 1 && cmd->argc != 2))
    return;
  unsigned n = cmdArgLen(&req);
  if (!n)
    return;
  char *data = os_malloc(n);
  if (!data) {
    socketNotify(c, USERCB_RECO, ESPCONN_MEM);
    return;
  }
  if (cmdPopArg(&req, data, n)) {
    os_free(data);
    return;
  }
  c->data = data;
  c->length = n;
  c->failed = false;
  if (c->mode == SOCKET_UDP && !c->connected) {
    if (espconn_create(c->pCon) != ESPCONN_OK) {
      c->failed = true;
      socketFinish(c);
      return;
    }
    c->connected = true;
  }
  if (c->mode == SOCKET_TCP_SERVER || c->mode == SOCKET_UDP || c->connected) {
    unsigned peers = 0;
    for (unsigned i = 0; i < MAX_PEERS; i++)
      if (c->peers[i].conn) {
        c->peers[i].sent = 0;
        c->peers[i].sending = true;
        peers++;
      }
    if (!peers) {
      c->failed = true;
      socketFinish(c);
      return;
    }
    for (unsigned i = 0; i < MAX_PEERS; i++)
      if (c->peers[i].sending)
        socketSendNext(c, c->peers + i);
  } else {
    c->pCon->state = ESPCONN_NONE;
    c->pCon->proto.tcp->local_port = espconn_port();
    if (UTILS_StrToIP(c->host, c->pCon->proto.tcp->remote_ip)) {
      if (espconn_connect(c->pCon) != ESPCONN_OK) {
        c->failed = true;
        socketFinish(c);
      }
    } else {
      c->resolving = true;
      c->ip.addr = 0;
      int r = espconn_gethostbyname(c->pCon, c->host, &c->ip, socket_dns_found);
      if (r == ESPCONN_OK)
        socket_dns_found(c->host, &c->ip, c->pCon);
      else if (r != ESPCONN_INPROGRESS)
        socket_dns_found(c->host, NULL, c->pCon);
    }
  }
}
