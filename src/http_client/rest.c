// Copyright 2015 Thorsten von Eicken; see LICENSE.
// Bounded HTTP/1.x client for the attached MCU. Callback payload remains <=100 bytes.
#include "esp8266.h"
#include "rest.h"
#include "cmd.h"
#include "netclient.h"
#define MAX_REST 4
#define REST_BODY 100

typedef struct {
  char line[128], body[REST_BODY];
  unsigned lineUsed, bodyUsed, chunkState;
  uint32_t remaining, chunk;
  int16_t status;
  bool headers, length, chunked, delivered, head, overflow;
} RestReply;
typedef struct {
  char *host, *header, *content_type, *user_agent, *data;
  uint16_t port, data_len, data_sent;
  ip_addr_t ip;
  struct espconn *pCon;
  uint32_t resp_cb;
  bool busy, resolving;
  RestReply *reply;
} RestClient;
static RestClient restClient[MAX_REST];
static unsigned restNum;
static void ICACHE_FLASH_ATTR restResult(RestClient *c, int code) {
  RestReply *r = c->reply;
  if (r && r->delivered)
    return;
  if (r)
    r->delivered = true;
  int16_t status = code;
  unsigned n = r ? r->bodyUsed : 0;
  cmdResponseStart(CMD_RESP_CB, c->resp_cb, n ? 2 : 1);
  cmdResponseBody(&status, 2);
  if (n)
    cmdResponseBody(r->body, n);
  cmdResponseEnd();
}
static void ICACHE_FLASH_ATTR restDone(RestClient *c) {
  if (c->data)
    os_free(c->data);
  c->data = NULL;
  if (c->reply)
    os_free(c->reply);
  c->reply = NULL;
  c->busy = c->resolving = false;
}
static void ICACHE_FLASH_ATTR restClose(RestClient *c, int status) {
  restResult(c, status);
  espconn_disconnect(c->pCon);
}
static bool ICACHE_FLASH_ATTR headerName(const char *p, const char *name) {
  while (*name) {
    unsigned a = (unsigned char)*p++, b = (unsigned char)*name++;
    if (a >= 'A' && a <= 'Z')
      a += 'a' - 'A';
    if (a != b)
      return false;
  }
  return true;
}
// Parse complete lines only. Ignore long unrelated headers without truncating framing fields.
static bool ICACHE_FLASH_ATTR restLine(RestReply *r) {
  char *p = r->line;
  p[r->lineUsed] = 0;
  if (!r->status) {
    if (r->lineUsed < 12 || r->overflow || os_strncmp(p, "HTTP/1.", 7) || p[7] < '0' ||
        p[7] > '9' || p[8] != ' ' || p[9] < '1' || p[9] > '5' || p[10] < '0' || p[10] > '9' ||
        p[11] < '0' || p[11] > '9' || (p[12] && p[12] != ' '))
      return false;
    r->status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + p[11] - '0';
  } else if (!*p) {
    if (r->status < 200) {
      r->status = 0;
      r->length = r->chunked = false;
      r->remaining = 0;
      return true;
    }
    r->headers = true;
    if (r->head || r->status == 204 || r->status == 304) {
      r->length = true;
      r->remaining = 0;
      r->chunked = false;
    }
  } else if (headerName(p, "content-length:")) {
    if (r->overflow || r->length)
      return false;
    p += 15;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p < '0' || *p > '9')
      return false;
    uint32_t n = 0;
    while (*p >= '0' && *p <= '9') {
      unsigned d = *p++ - '0';
      if (n > (0xffffffffU - d) / 10)
        return false;
      n = n * 10 + d;
    }
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p)
      return false;
    r->length = true;
    r->remaining = n;
  } else if (headerName(p, "transfer-encoding:")) {
    p += 18;
    while (*p == ' ' || *p == '\t')
      p++;
    if (r->overflow || !headerName(p, "chunked") || p[7])
      return false;
    r->chunked = true;
  }
  return true;
}
static void ICACHE_FLASH_ATTR restBody(RestReply *r, char byte) {
  if (r->bodyUsed < REST_BODY)
    r->body[r->bodyUsed++] = byte;
}
// Returns -1 invalid, 0 incomplete, 1 complete. Never assumes TCP chunk boundaries.
static int ICACHE_FLASH_ATTR restFeed(RestReply *r, const char *p, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    unsigned char ch = p[i];
    if (!r->headers) {
      if (ch == '\n') {
        if (!restLine(r))
          return -1;
        r->lineUsed = 0;
        r->overflow = false;
        if (r->headers && !r->chunked && r->length && !r->remaining)
          return 1;
      } else if (ch != '\r') {
        if (r->lineUsed + 1 < sizeof r->line)
          r->line[r->lineUsed++] = ch;
        else
          r->overflow = true;
      }
    } else if (r->chunked) {
      if (r->chunkState == 0) {
        if (r->overflow && ch != '\n')
          continue;
        unsigned v = ch >= '0' && ch <= '9'   ? ch - '0'
                     : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                     : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                              : 16;
        if (v < 16) {
          if (r->overflow || r->chunk > 0x0fffffffU)
            return -1;
          r->chunk = r->chunk * 16 + v;
          r->lineUsed++;
        } else if (ch == ';' && r->lineUsed)
          r->overflow = true;
        else if (ch == '\n' && r->lineUsed) {
          r->remaining = r->chunk;
          r->chunk = 0;
          r->lineUsed = 0;
          r->overflow = false;
          r->chunkState = r->remaining ? 1 : 4;
        } else if (ch != '\r' && !r->overflow)
          return -1;
      } else if (r->chunkState == 1) {
        restBody(r, ch);
        if (!--r->remaining)
          r->chunkState = 2;
      } else if (r->chunkState == 2) {
        if (ch != '\r')
          return -1;
        r->chunkState = 3;
      } else if (r->chunkState == 3) {
        if (ch != '\n')
          return -1;
        r->chunkState = 0;
      } else if (ch == '\n') {
        if (!r->lineUsed)
          return 1;
        r->lineUsed = 0;
      } else if (ch != '\r')
        r->lineUsed = 1; // consume trailers through their final empty line
    } else {
      restBody(r, ch);
      if (r->length && !--r->remaining)
        return 1;
    }
  }
  return 0;
}
static void ICACHE_FLASH_ATTR tcpclient_recv(void *arg, char *p, unsigned short n) {
  RestClient *c = ((struct espconn *)arg)->reverse;
  if (!c || !c->busy || !c->reply)
    return;
  int result = restFeed(c->reply, p, n);
  if (result)
    restClose(c, result < 0 ? 502 : c->reply->status);
}
static void ICACHE_FLASH_ATTR tcpclient_discon_cb(void *arg) {
  RestClient *c = ((struct espconn *)arg)->reverse;
  if (!c)
    return;
  if (c->reply && !c->reply->delivered)
    restResult(c, c->reply->headers && !c->reply->chunked && !c->reply->length ? c->reply->status
                                                                               : 502);
  restDone(c);
}
static void ICACHE_FLASH_ATTR tcpclient_recon_cb(void *arg, sint8 error) {
  (void)error;
  RestClient *c = ((struct espconn *)arg)->reverse;
  if (!c)
    return;
  restResult(c, 502);
  restDone(c);
}
static void ICACHE_FLASH_ATTR tcpclient_sent_cb(void *arg) {
  RestClient *c = ((struct espconn *)arg)->reverse;
  if (!c || !c->busy)
    return;
  if (c->data_sent < c->data_len) {
    unsigned n = c->data_len - c->data_sent;
    if (n > 1400)
      n = 1400;
    if (espconn_sent(c->pCon, (uint8_t *)c->data + c->data_sent, n) != ESPCONN_OK) {
      restClose(c, 502);
      return;
    }
    c->data_sent += n;
  } else {
    os_free(c->data);
    c->data = NULL;
  }
}
static void ICACHE_FLASH_ATTR tcpclient_connect_cb(void *arg) {
  struct espconn *p = arg;
  RestClient *c = p->reverse;
  if (!c || !c->busy)
    return;
  espconn_regist_disconcb(p, tcpclient_discon_cb);
  espconn_regist_recvcb(p, tcpclient_recv);
  espconn_regist_sentcb(p, tcpclient_sent_cb);
  espconn_regist_time(p, 20, 0);
  c->data_sent = 0;
  tcpclient_sent_cb(p);
}
static void ICACHE_FLASH_ATTR rest_dns_found(const char *name, ip_addr_t *ip, void *arg) {
  (void)name;
  RestClient *c = ((struct espconn *)arg)->reverse;
  if (!c || !c->resolving)
    return;
  c->resolving = false;
  if (!ip || !ip->addr) {
    restResult(c, 502);
    restDone(c);
    return;
  }
  os_memcpy(c->pCon->proto.tcp->remote_ip, &ip->addr, 4);
  if (espconn_connect(c->pCon) != ESPCONN_OK) {
    restResult(c, 502);
    restDone(c);
  }
}
static void ICACHE_FLASH_ATTR restFree(RestClient *c) {
  // Only called for an idle slot: no DNS, connection or borrowed payload remains.
  if (c->host)
    os_free(c->host);
  if (c->header)
    os_free(c->header);
  if (c->content_type)
    os_free(c->content_type);
  if (c->user_agent)
    os_free(c->user_agent);
  netConnectionFree(c->pCon);
  os_memset(c, 0, sizeof(*c));
}
void ICACHE_FLASH_ATTR REST_Setup(CmdPacket *cmd) {
  CmdRequest req;
  cmdRequest(&req, cmd);
  int32_t result = -1;
  RestClient next = {0};
  if (cmdGetArgc(&req) != 3 || cmdArgLen(&req) > 128)
    goto done;
  unsigned n = cmdArgLen(&req);
  next.host = os_zalloc(n + 1);
  if (!next.host || cmdPopArg(&req, next.host, n))
    goto done;
  uint8_t security;
  if (cmdPopArg(&req, &next.port, 2) || cmdPopArg(&req, &security, 1) || security || !next.port)
    goto done;
  unsigned index = restNum % MAX_REST;
  RestClient *c = restClient + index;
  if (c->busy)
    goto done;
  next.header = netString("", 0);
  next.content_type = netString("application/x-www-form-urlencoded", 33);
  next.user_agent = netString("esp-serial2wifi", sizeof("esp-serial2wifi") - 1);
  next.pCon = netConnectionNew(ESPCONN_TCP, next.port, c);
  if (!next.header || !next.content_type || !next.user_agent || !next.pCon)
    goto done;
  next.resp_cb = cmd->value;
  restFree(c);
  *c = next;
  os_memset(&next, 0, sizeof next);
  restNum = (index + 1) % MAX_REST;
  result = index;
done:
  restFree(&next);
  cmdResponseStart(CMD_RESP_V, result, 0);
  cmdResponseEnd();
}
void ICACHE_FLASH_ATTR REST_SetHeader(CmdPacket *cmd) {
  CmdRequest req;
  cmdRequest(&req, cmd);
  RestClient *c = restClient + (cmd->value % MAX_REST);
  uint8_t selector;
  if (cmd->value >= MAX_REST || !c->pCon || c->busy || cmdGetArgc(&req) != 2 ||
      cmdPopArg(&req, &selector, 1))
    return;
  unsigned n = cmdArgLen(&req);
  if (n > 256 || selector > 2)
    return;
  char *value = os_zalloc(n + 3);
  if (!value)
    return;
  if (cmdPopArg(&req, value, n)) {
    os_free(value);
    return;
  }
  if (selector == 0) {
    value[n] = '\r';
    value[n + 1] = '\n';
  } else
    for (unsigned i = 0; i < n; i++)
      if ((unsigned char)value[i] < 32) {
        os_free(value);
        return;
      }
  char **target = selector == 0 ? &c->header : selector == 1 ? &c->content_type : &c->user_agent;
  os_free(*target);
  *target = value;
}
void ICACHE_FLASH_ATTR REST_Request(CmdPacket *cmd) {
  CmdRequest req;
  cmdRequest(&req, cmd);
  RestClient *c = restClient + (cmd->value % MAX_REST);
  if (cmd->value >= MAX_REST || !c->pCon || c->busy || (cmd->argc != 2 && cmd->argc != 3))
    return;
  unsigned n = cmdArgLen(&req);
  char method[16], path[1024];
  if (!n || n >= sizeof method || cmdPopArg(&req, method, n))
    return;
  method[n] = 0;
  n = cmdArgLen(&req);
  if (!n || n >= sizeof path || cmdPopArg(&req, path, n))
    return;
  path[n] = 0;
  for (const char *p = method; *p; p++)
    if (*p < 'A' || *p > 'Z')
      return;
  for (const char *p = path; *p; p++)
    if ((unsigned char)*p <= 32)
      return;
  unsigned body = cmd->argc == 3 ? cmdArgLen(&req) : 0;
  if (body > 2048)
    return;
  unsigned capacity = 160 + os_strlen(c->host) + os_strlen(c->header) + os_strlen(c->content_type) +
                      os_strlen(c->user_agent) + os_strlen(method) + os_strlen(path) + body;
  c->data = os_malloc(capacity);
  c->reply = os_zalloc(sizeof(*c->reply));
  if (!c->data || !c->reply) {
    restResult(c, 503);
    restDone(c);
    return;
  }
  c->reply->head = !os_strcmp(method, "HEAD");
  c->data_len = os_sprintf(
      c->data,
      "%s %s HTTP/1.0\r\nHost: %s\r\n%sContent-Length: %u\r\nConnection: close\r\nContent-Type: %s\r\nUser-Agent: %s\r\n\r\n",
      method, path, c->host, c->header, body, c->content_type, c->user_agent);
  if (body && cmdPopArg(&req, c->data + c->data_len, body)) {
    restDone(c);
    return;
  }
  c->data_len += body;
  c->busy = true;
  c->pCon->state = ESPCONN_NONE;
  c->pCon->proto.tcp->local_port = espconn_port();
  espconn_regist_connectcb(c->pCon, tcpclient_connect_cb);
  espconn_regist_reconcb(c->pCon, tcpclient_recon_cb);
  if (UTILS_StrToIP(c->host, c->pCon->proto.tcp->remote_ip)) {
    if (espconn_connect(c->pCon) != ESPCONN_OK) {
      restResult(c, 502);
      restDone(c);
    }
  } else {
    c->resolving = true;
    c->ip.addr = 0;
    int result = espconn_gethostbyname(c->pCon, c->host, &c->ip, rest_dns_found);
    if (result == ESPCONN_OK)
      rest_dns_found(c->host, &c->ip, c->pCon);
    else if (result != ESPCONN_INPROGRESS) {
      c->resolving = false;
      restResult(c, 502);
      restDone(c);
    }
  }
}
