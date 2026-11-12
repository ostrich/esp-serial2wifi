/*
Esp8266 http server - core routines
*/

/*
* ----------------------------------------------------------------------------
* "THE BEER-WARE LICENSE" (Revision 42):
* Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
* this notice you can do whatever you want with this stuff. If we meet some day,
* and you think this stuff is worth it, you can buy me a beer in return.
* ----------------------------------------------------------------------------
* Modified and enhanced by Thorsten von Eicken in 2015
* ----------------------------------------------------------------------------
*/

#include <esp8266.h>
#include "httpd.h"

//#define HTTPD_DBG
#ifdef HTTPD_DBG
#define DBG(format, ...)                                                                           \
  do {                                                                                             \
    os_printf(format, ##__VA_ARGS__);                                                              \
  } while (0)
#else
#define DBG(format, ...)                                                                           \
  do {                                                                                             \
  } while (0)
#endif

//Max length of request head
#define MAX_HEAD_LEN 1024
//Max amount of connections
#define MAX_CONN 6
//Max post buffer len
#define MAX_POST 1024
//Max send buffer len
#define MAX_SENDBUFF_LEN 2600

//This gets set at init time.
static HttpdBuiltInUrl *builtInUrls;

//Private data for http connection
struct HttpdPriv {
  char *head;              // allocated only while this connection is active
  char from[24];           // source ip&port
  char *sendBuff;          // output buffer
  bool sendFailed;         // ambiguous send: retain source until disconnect
  bool readDeferred;       // scheduled without retaining a callback stack buffer
  uint32 readNotBefore;    // timed CGI wakeup; zero means ready now
  char *pendingSend;       // SDK-borrowed bytes, owned until ACK/disconnect
  short headPos;           // offset into header
  short sendBuffLen;       // offset into output buffer
  short sendBuffMax;       // size of output buffer
  short code;              // http response code (only for logging)
  unsigned short cgiIndex; // preserve route fallback across a deferred first call
};

//Connection pool
static HttpdPriv connPrivData[MAX_CONN];
static HttpdConnData connData[MAX_CONN];
static HttpdPostData connPostData[MAX_CONN];

//Listening connection data
static struct espconn httpdConn;
static esp_tcp httpdTcp;

//Struct to keep extension->mime data in
typedef struct {
  const char *ext;
  const char *mimetype;
} MimeMap;

//The mappings from file extensions to mime types. If you need an extra mime type,
//add it here.
static const MimeMap mimeTypes[] = {
    {"htm", "text/htm"},     {"html", "text/html; charset=UTF-8"},
    {"css", "text/css"},     {"js", "text/javascript"},
    {"txt", "text/plain"},   {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},  {"png", "image/png"},
    {"ico", "image/x-icon"}, {"tpl", "text/html; charset=UTF-8"},
    {NULL, "text/html"}, //default value
};

//Returns a static char* to a mime type for a given url to a file.
const char ICACHE_FLASH_ATTR *httpdGetMimetype(char *url) {
  int i = 0;
  //Go find the extension
  char *ext = url + (strlen(url) - 1);
  while (ext != url && *ext != '.')
    ext--;
  if (*ext == '.')
    ext++;

  //ToDo: os_strcmp is case sensitive; we may want to do case-intensive matching here...
  while (mimeTypes[i].ext != NULL && os_strcmp(ext, mimeTypes[i].ext) != 0)
    i++;
  return mimeTypes[i].mimetype;
}

// debug string to identify connection (ip address & port)
// a static string works because callbacks don't get interrupted...
static char connStr[24];

static void debugConn(void *arg, char *what) {
#if 0
  struct espconn *espconn = arg;
  esp_tcp *tcp = espconn->proto.tcp;
  os_sprintf(connStr, "%d.%d.%d.%d:%d ",
    tcp->remote_ip[0], tcp->remote_ip[1], tcp->remote_ip[2], tcp->remote_ip[3],
    tcp->remote_port);
  DBG("%s %s\n", connStr, what);
#else
  connStr[0] = 0;
#endif
}

// Retires a connection for re-use
static void ICACHE_FLASH_ATTR httpdReleaseSend(HttpdConnData *conn) {
  if (conn->priv->pendingSend) {
    os_free(conn->priv->pendingSend);
    conn->priv->pendingSend = NULL;
  }
}

// Streaming files no longer need request headers after opening; completed CGIs
// no longer need them either. Preserve only the URL for logging/cleanup.
void ICACHE_FLASH_ATTR httpdReleaseHeaders(HttpdConnData *conn) {
  if (!conn->priv->head || conn->priv->headPos < 0 || !conn->url)
    return;
  size_t length = os_strlen(conn->url) + 1;
  if (length + 16 >= MAX_HEAD_LEN)
    return;
  char *path = os_malloc(length);
  if (!path)
    return;
  os_memcpy(path, conn->url, length);
  os_free(conn->priv->head);
  conn->priv->head = path;
  conn->priv->headPos = -1;
  conn->url = path;
  conn->getArgs = NULL;
}

static void ICACHE_FLASH_ATTR httpdRetireConn(HttpdConnData *conn) {
  httpdReleaseSend(conn);
  conn->priv->readDeferred = false;
  conn->priv->readNotBefore = 0;
  if (conn->conn && conn->conn->reverse == conn)
    conn->conn->reverse = NULL; // break reverse link

  // log information about the request we handled
  uint32 dt = conn->startTime;
  if (dt > 0)
    dt = (system_get_time() - dt) / 1000;
  if (conn->conn && conn->url)
#if 0
    DBG("HTTP %s %s from %s -> %d in %ums, heap=%ld\n",
      conn->requestType == HTTPD_METHOD_GET ? "GET" : "POST", conn->url, conn->priv->from,
      conn->priv->code, dt, (unsigned long)system_get_free_heap_size());
#else
    DBG("HTTP %s %s: %d, %ums, h=%ld\n", conn->requestType == HTTPD_METHOD_GET ? "GET" : "POST",
        conn->url, conn->priv->code, dt, (unsigned long)system_get_free_heap_size());
#endif

  conn->conn = NULL; // don't try to send anything, the SDK crashes...
  if (conn->cgi != NULL)
    conn->cgi(conn); // free cgi data
  if (conn->post->buff != NULL)
    os_free(conn->post->buff);
  conn->cgi = NULL;
  conn->cgiPrivData = NULL;
  conn->post->buff = NULL;
  conn->post->multipartBoundary = NULL;
  os_free(conn->priv->head);
  conn->priv->head = NULL;
  conn->url = NULL;
  conn->getArgs = NULL;
}

static bool ICACHE_FLASH_ATTR httpdAppendHeader(HttpdConnData *conn, char byte) {
  if (conn->priv->headPos < 0 || conn->priv->headPos >= MAX_HEAD_LEN)
    return false;
  conn->priv->head[conn->priv->headPos++] = byte;
  conn->priv->head[conn->priv->headPos] = 0;
  return true;
}

//Stupid li'l helper function that returns the value of a hex char.
static int httpdHexVal(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return 0;
}

//Decode a percent-encoded value.
//Takes the valLen bytes stored in val, and converts it into at most retLen bytes that
//are stored in the ret buffer. Returns the actual amount of bytes used in ret. Also
//zero-terminates the ret buffer.
int httpdUrlDecode(char *val, int valLen, char *ret, int retLen) {
  int s = 0, d = 0;
  int esced = 0, escVal = 0;
  while (s < valLen && d < retLen) {
    if (esced == 1) {
      escVal = httpdHexVal(val[s]) << 4;
      esced = 2;
    } else if (esced == 2) {
      escVal += httpdHexVal(val[s]);
      ret[d++] = escVal;
      esced = 0;
    } else if (val[s] == '%') {
      esced = 1;
    } else if (val[s] == '+') {
      ret[d++] = ' ';
    } else {
      ret[d++] = val[s];
    }
    s++;
  }
  if (d < retLen)
    ret[d] = 0;
  return d;
}

//Find a specific arg in a string of get- or post-data.
//Line is the string of post/get-data, arg is the name of the value to find. The
//zero-terminated result is written in buff, with at most buffLen bytes used. The
//function returns the length of the result, or -1 if the value wasn't found. The
//returned string will be urldecoded already.
int ICACHE_FLASH_ATTR httpdFindArg(char *line, char *arg, char *buff, int buffLen) {
  char *p, *e;
  if (line == NULL)
    return 0;
  p = line;
  while (p != NULL && *p != '\n' && *p != '\r' && *p != 0) {
    //os_printf("findArg: %s\n", p);
    if (os_strncmp(p, arg, os_strlen(arg)) == 0 && p[strlen(arg)] == '=') {
      p += os_strlen(arg) + 1; //move p to start of value
      e = (char *)os_strstr(p, "&");
      if (e == NULL)
        e = p + os_strlen(p);
      //os_printf("findArg: val %s len %d\n", p, (e-p));
      return httpdUrlDecode(p, (e - p), buff, buffLen);
    }
    p = (char *)os_strstr(p, "&");
    if (p != NULL)
      p += 1;
  }
  //os_printf("Finding %s in %s: Not found :/\n", arg, line);
  return -1; //not found
}

//Get the value of a certain header in the HTTP client head
static bool ICACHE_FLASH_ATTR httpdHeaderName(const char *line, const char *name) {
  while (*name) {
    unsigned char a = *line++, b = *name++;
    if (a >= 'A' && a <= 'Z')
      a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z')
      b += 'a' - 'A';
    if (a != b)
      return false;
  }
  return *line == ':';
}

int ICACHE_FLASH_ATTR httpdGetHeader(HttpdConnData *conn, char *header, char *ret, int retLen) {
  if (retLen <= 0)
    return 0;
  ret[0] = 0;
  if (conn->priv->headPos < 0)
    return 0;
  char *p = conn->priv->head;
  p = p + strlen(p) + 1; //skip GET/POST part
  p = p + strlen(p) + 1; //skip HTTP part
  while (p < (conn->priv->head + conn->priv->headPos)) {
    while (*p <= 32 && *p != 0)
      p++; //skip crap at start
    //See if this is the header
    if (httpdHeaderName(p, header)) {
      //Skip 'key:' bit of header line
      p = p + strlen(header) + 1;
      //Skip past spaces after the colon
      while (*p == ' ' || *p == '\t')
        p++;
      //Copy from p to end
      while (*p != 0 && *p != '\r' && *p != '\n' && retLen > 1) {
        *ret++ = *p++;
        retLen--;
      }
      //Zero-terminate string
      *ret = 0;
      //All done :)
      return 1;
    }
    p += strlen(p) + 1; //Skip past end of string and \0 terminator
  }
  return 0;
}

//Setup an output buffer
void ICACHE_FLASH_ATTR httpdSetOutputBuffer(HttpdConnData *conn, char *buff, short max) {
  conn->priv->sendBuff = buff;
  conn->priv->sendBuffLen = 0;
  conn->priv->sendBuffMax = max;
}

//Start the response headers.
void ICACHE_FLASH_ATTR httpdStartResponse(HttpdConnData *conn, int code) {
  char buff[128];
  int l;
  conn->priv->code = code;
  char *status = code < 400 ? "OK" : "ERROR";
  l = os_sprintf(buff, "HTTP/1.0 %d %s\r\nServer: esp-serial2wifi\r\nConnection: close\r\n", code,
                 status);
  httpdSend(conn, buff, l);
}

//Send a http header.
void ICACHE_FLASH_ATTR httpdHeader(HttpdConnData *conn, const char *field, const char *val) {
  char buff[256];
  int l;

  l = os_sprintf(buff, "%s: %s\r\n", field, val);
  httpdSend(conn, buff, l);
}

//Finish the headers.
void ICACHE_FLASH_ATTR httpdEndHeaders(HttpdConnData *conn) { httpdSend(conn, "\r\n", -1); }

//ToDo: sprintf->snprintf everywhere... esp doesn't have snprintf tho' :/
//Redirect to the given URL.
void ICACHE_FLASH_ATTR httpdRedirect(HttpdConnData *conn, char *newUrl) {
  char buff[1024];
  int l;
  conn->priv->code = 302;
  l = os_sprintf(buff,
                 "HTTP/1.0 302 Found\r\nServer: esp-serial2wifi\r\nConnection: close\r\n"
                 "Location: %s\r\n\r\nRedirecting to %s\r\n",
                 newUrl, newUrl);
  httpdSend(conn, buff, l);
}

//Use this as a cgi function to redirect one url to another.
int ICACHE_FLASH_ATTR cgiRedirect(HttpdConnData *connData) {
  if (connData->conn == NULL) {
    //Connection aborted. Clean up.
    return HTTPD_CGI_DONE;
  }
  httpdRedirect(connData, (char *)connData->cgiArg);
  return HTTPD_CGI_DONE;
}

//Add data to the send buffer. len is the length of the data. If len is -1
//the data is seen as a C-string.
//Returns 1 for success, 0 for out-of-memory.
int ICACHE_FLASH_ATTR httpdSend(HttpdConnData *conn, const char *data, int len) {
  if (len < 0)
    len = strlen(data);
  if (conn->priv->sendBuffLen + len > conn->priv->sendBuffMax) {
    DBG("%sERROR! httpdSend full (%d of %d)\n", connStr, conn->priv->sendBuffLen,
        conn->priv->sendBuffMax);
    return 0;
  }
  os_memcpy(conn->priv->sendBuff + conn->priv->sendBuffLen, data, len);
  conn->priv->sendBuffLen += len;
  return 1;
}

//Helper function to send any data in conn->priv->sendBuff
void ICACHE_FLASH_ATTR httpdFlush(HttpdConnData *conn) {
  if (conn->priv->sendBuffLen != 0) {
    if (conn->priv->sendFailed) {
      conn->priv->sendBuffLen = 0;
      return;
    }
    // The SDK may retain even COPY-mode input while waiting for TCP credit.
    // Never lend callback stack storage or overwrite an outstanding response.
    if (conn->priv->pendingSend) {
      conn->priv->sendFailed = true;
      espconn_disconnect(conn->conn);
      conn->priv->sendBuffLen = 0;
      return;
    }
    conn->priv->pendingSend = os_malloc(conn->priv->sendBuffLen);
    if (!conn->priv->pendingSend) {
      conn->priv->sendFailed = true;
      espconn_disconnect(conn->conn);
      conn->priv->sendBuffLen = 0;
      return;
    }
    os_memcpy(conn->priv->pendingSend, conn->priv->sendBuff, conn->priv->sendBuffLen);
    sint8 status =
        espconn_sent(conn->conn, (uint8_t *)conn->priv->pendingSend, conn->priv->sendBuffLen);
    if (status != 0) {
      conn->priv->sendFailed = true;
      DBG("%sERROR! espconn_sent returned %d, trying to send %d to %s\n", connStr, status,
          conn->priv->sendBuffLen, conn->url);
      // Errors can follow partial acceptance; release only on disconnect.
      espconn_disconnect(conn->conn);
    }
    conn->priv->sendBuffLen = 0;
  }
}

// Cooperative HTTP read scheduling. The SDK retains response copies until ACK;
// independent asset connections are outside the browser API request limit.
// Let static-file opening release its large request header, then admit only one
// HTTP response at a time while serial output is active. Pause new read work
// before constructing a response if backlog/heap pressure needs TCP service.
extern uint32_t serbridgePendingTx(void);
extern unsigned consoleStreamPending(void);
int apiSerialLive(HttpdConnData *);
extern uint8_t serbridge_client_count(void);
int cgiEspFsHook(HttpdConnData *);
int cgiUserFsHook(HttpdConnData *);
static ETSTimer httpdReadTimer;
static bool httpdReadArmed, httpdReadGranted;
static unsigned httpdReadCursor;
static void httpdProcessRequest(HttpdConnData *);
static void httpdReadTick(void *);
void ICACHE_FLASH_ATTR httpdWait(HttpdConnData *conn, unsigned milliseconds) {
  conn->priv->readDeferred = true;
  conn->priv->readNotBefore = system_get_time() + milliseconds * 1000U;
  if (!httpdReadArmed) {
    httpdReadArmed = true;
    os_timer_setfn(&httpdReadTimer, httpdReadTick, NULL);
    os_timer_arm(&httpdReadTimer, 10, 0);
  }
}
static bool ICACHE_FLASH_ATTR httpdReadDue(HttpdConnData *conn) {
  return !conn->priv->readNotBefore ||
         (int32_t)(system_get_time() - conn->priv->readNotBefore) >= 0;
}
static bool ICACHE_FLASH_ATTR httpdDeferRead(HttpdConnData *conn) {
  if (conn->priv->readDeferred && !httpdReadDue(conn))
    return true;
  if (conn->cgiResponse)
    return false; // MCU response borrows its current command frame.
  if (conn->requestType != HTTPD_METHOD_GET)
    return false; // streamed POST buffers are callback-owned
  // UART streaming must drain even when background GETs are waiting. Its
  // output is copied into the normal ACK-owned HTTP buffer, never lent from
  // the live FIFO. The stream's own pendingSend is still checked by callers.
  if (conn->cgi == apiSerialLive)
    return false;
  uint32_t pending = serbridgePendingTx(), live = consoleStreamPending();
  if (!pending && !serbridge_client_count() && !live)
    return false;
  // Opening a static file checks Accept-Encoding and releases a 1KB header.
  // Its first response contains headers only, not a file payload.
  if ((conn->cgi == cgiEspFsHook || conn->cgi == cgiUserFsHook) && !conn->cgiData)
    return false;
  bool blocked = pending > 6U * 536U || live > 1024U || system_get_free_heap_size() < 8192;
  for (unsigned i = 0; i < MAX_CONN; i++)
    if (connData[i].conn) {
      if (connData[i].priv->pendingSend)
        blocked = true;
      if (!httpdReadGranted && conn != &connData[i] && connData[i].priv->readDeferred &&
          httpdReadDue(&connData[i]))
        blocked = true;
    }
  if (!blocked)
    return false;
  conn->priv->readDeferred = true;
  if (!httpdReadArmed) {
    httpdReadArmed = true;
    os_timer_setfn(&httpdReadTimer, httpdReadTick, NULL);
    os_timer_arm(&httpdReadTimer, 10, 0);
  }
  return true;
}

//Callback called when the data on a socket has been successfully sent.
static void ICACHE_FLASH_ATTR httpdSentCb(void *arg) {
  debugConn(arg, "httpdSentCb");
  struct espconn *pCon = (struct espconn *)arg;
  HttpdConnData *conn = (HttpdConnData *)pCon->reverse;
  if (conn == NULL)
    return; // aborted connection

  // A partial-error prefix can still produce a callback before disconnect.
  if (conn->priv->sendFailed)
    return;
  httpdReleaseSend(conn);
  char sendBuff[MAX_SENDBUFF_LEN];
  httpdSetOutputBuffer(conn, sendBuff, sizeof(sendBuff));

  if (conn->cgi == NULL) { //Marked for destruction?
    //os_printf("Closing 0x%p/0x%p->0x%p\n", arg, conn->conn, conn);
    espconn_disconnect(conn->conn); // we will get a disconnect callback
    return;                         //No need to call httpdFlush.
  }

  if (httpdDeferRead(conn))
    return;
  int r = conn->cgi(conn); //Execute cgi fn.
  if (r == HTTPD_CGI_DONE) {
    conn->cgi = NULL; //mark for destruction.
    httpdReleaseHeaders(conn);
  }
  if (r == HTTPD_CGI_NOTFOUND || r == HTTPD_CGI_AUTHENTICATED) {
    DBG("%sERROR! Bad CGI code %d\n", connStr, r);
    conn->cgi = NULL; //mark for destruction.
  }
  httpdFlush(conn);
}

static const char *httpNotFoundHeader =
    "HTTP/1.0 404 Not Found\r\nConnection: close\r\n"
    "Content-Type: text/plain\r\nContent-Length: 12\r\n\r\nNot Found.\r\n";

//This is called when the headers have been received and the connection is ready to send
//the result headers and data.
//We need to find the CGI function to call, call it, and dependent on what it returns either
//find the next cgi function, wait till the cgi data is sent or close up the connection.
static void ICACHE_FLASH_ATTR httpdProcessRequest(HttpdConnData *conn) {
  int r;
  int i = conn->cgi ? conn->priv->cgiIndex : 0;
  if (conn->url == NULL) {
    DBG("%sWtF? url = NULL\n", connStr);
    return; //Shouldn't happen
  }
  //See if we can find a CGI that's happy to handle the request.
  while (1) {
    //Look up URL in the built-in URL table.
    if (conn->cgi == NULL) {
      while (builtInUrls[i].url != NULL) {
        int match = 0;
        int urlLen = os_strlen(builtInUrls[i].url);
        //See if there's a literal match
        if (os_strcmp(builtInUrls[i].url, conn->url) == 0)
          match = 1;
        //See if there's a wildcard match
        if (builtInUrls[i].url[urlLen - 1] == '*' &&
            os_strncmp(builtInUrls[i].url, conn->url, urlLen - 1) == 0)
          match = 1;
        else if (builtInUrls[i].url[0] == '*' && (strlen(conn->url) >= urlLen - 1) &&
                 os_strncmp(builtInUrls[i].url + 1, conn->url + strlen(conn->url) - urlLen + 1,
                            urlLen - 1) == 0)
          match = 1;
        if (match) {
          //os_printf("Is url index %d\n", i);
          conn->cgiData = NULL;
          conn->cgiResponse = NULL;
          conn->cgi = builtInUrls[i].cgiCb;
          conn->priv->cgiIndex = i;
          conn->cgiArg = builtInUrls[i].cgiArg;
          break;
        }
        i++;
      }
      if (builtInUrls[i].url == NULL) {
        //Drat, we're at the end of the URL table. This usually shouldn't happen. Well, just
        //generate a built-in 404 to handle this.
        DBG("%s%s not found. 404!\n", connStr, conn->url);
        httpdSend(conn, httpNotFoundHeader, -1);
        httpdFlush(conn);
        conn->cgi = NULL; //mark for destruction.
        if (conn->post)
          conn->post->len = 0; // skip any remaining receives
        return;
      }
    }

    //Okay, we have a CGI function that matches the URL. See if it wants to handle the
    //particular URL we're supposed to handle.
    if (httpdDeferRead(conn))
      return;
    r = conn->cgi(conn);
    if (r == HTTPD_CGI_MORE) {
      //Yep, it's happy to do so and has more data to send.
      httpdFlush(conn);
      return;
    } else if (r == HTTPD_CGI_DONE) {
      //Yep, it's happy to do so and already is done sending data.
      httpdReleaseHeaders(conn);
      httpdFlush(conn);
      conn->cgi = NULL; //mark for destruction.
      if (conn->post)
        conn->post->len = 0; // skip any remaining receives
      return;
    } else {
      if (!(r == HTTPD_CGI_NOTFOUND || r == HTTPD_CGI_AUTHENTICATED)) {
        os_printf("%shandler for %s returned invalid result %d\n", connStr, conn->url, r);
      }
      //URL doesn't want to handle the request: either the data isn't found or there's no
      //need to generate a login screen.
      conn->cgi = NULL; // force lookup again
      i++;              //look at next url the next iteration of the loop.
    }
  }
}

static void ICACHE_FLASH_ATTR httpdReadTick(void *unused) {
  (void)unused;
  httpdReadArmed = false;
  // Round-robin: a file cannot keep taking its next chunk ahead of JSON reads.
  for (unsigned n = 0; n < MAX_CONN; n++) {
    unsigned i = (httpdReadCursor + n) % MAX_CONN;
    HttpdConnData *conn = &connData[i];
    if (!conn->conn || !conn->priv->readDeferred || conn->priv->sendFailed ||
        conn->priv->pendingSend || !httpdReadDue(conn))
      continue;
    conn->priv->readDeferred = false;
    conn->priv->readNotBefore = 0;
    char buffer[MAX_SENDBUFF_LEN];
    httpdSetOutputBuffer(conn, buffer, sizeof buffer);
    httpdReadGranted = true;
    httpdProcessRequest(conn);
    httpdReadGranted = false;
    if (!conn->priv->readDeferred) {
      httpdReadCursor = (i + 1) % MAX_CONN;
      break;
    }
  }
  // A just-served read may have occupied the sole response allowance. Retain
  // the rest without heap copies and resume after ACK/bridge progress.
  for (unsigned i = 0; i < MAX_CONN; i++)
    if (connData[i].conn && connData[i].priv->readDeferred && !httpdReadArmed) {
      httpdReadArmed = true;
      os_timer_setfn(&httpdReadTimer, httpdReadTick, NULL);
      os_timer_arm(&httpdReadTimer, 10, 0);
      break;
    }
}

//Parse a line of header data and modify the connection data accordingly.
static void ICACHE_FLASH_ATTR httpdRejectHeader(HttpdConnData *conn) {
  conn->post->len = -2;
  espconn_disconnect(conn->conn);
}

static void ICACHE_FLASH_ATTR httpdParseHeader(char *h, HttpdConnData *conn) {
  int i;
  char first_line = false;

  if (os_strncmp(h, "GET ", 4) == 0) {
    conn->requestType = HTTPD_METHOD_GET;
    first_line = true;
  } else if (os_strncmp(h, "POST ", 5) == 0) {
    conn->requestType = HTTPD_METHOD_POST;
    first_line = true;
  }

  if (first_line) {
    char *e;

    //Skip past the space after POST/GET
    i = 0;
    while (h[i] != ' ')
      i++;
    conn->url = h + i + 1;

    //Figure out end of url.
    e = (char *)os_strstr(conn->url, " ");
    if (e == NULL)
      return; //wtf?
    *e = 0;   //terminate url part

    // Count number of open connections
    //esp_tcp *tcp = conn->conn->proto.tcp;
    //DBG("%sHTTP %s %s from %s\n", connStr,
    //  conn->requestType == HTTPD_METHOD_GET ? "GET" : "POST", conn->url, conn->priv->from);
    //Parse out the URL part before the GET parameters.
    conn->getArgs = (char *)os_strstr(conn->url, "?");
    if (conn->getArgs != 0) {
      *conn->getArgs = 0;
      conn->getArgs++;
      //DBG("%sargs = %s\n", connStr, conn->getArgs);
    } else {
      conn->getArgs = NULL;
    }

  } else if (httpdHeaderName(h, "Content-Length")) {
    const char *p = h + 15;
    uint32_t length = 0;
    if (conn->post->buff) {
      httpdRejectHeader(conn);
      return;
    }
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p < '0' || *p > '9') {
      httpdRejectHeader(conn);
      return;
    }
    while (*p >= '0' && *p <= '9') {
      unsigned digit = *p++ - '0';
      if (length > (0x7fffffffU - digit) / 10) {
        httpdRejectHeader(conn);
        return;
      }
      length = length * 10 + digit;
    }
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p) {
      httpdRejectHeader(conn);
      return;
    }
    conn->post->len = length;

    // Allocate the buffer
    if (conn->post->len > MAX_POST) {
      // we'll stream this in in chunks
      conn->post->buffSize = MAX_POST;
    } else {
      conn->post->buffSize = conn->post->len;
    }
    //DBG("Mallocced buffer for %d + 1 bytes of post data.\n", conn->post->buffSize);
    conn->post->buff = (char *)os_malloc(conn->post->buffSize + 1);
    if (!conn->post->buff) {
      httpdRejectHeader(conn);
      return;
    }
    conn->post->buff[0] = 0;
    conn->post->buffLen = 0;
  } else if (httpdHeaderName(h, "Transfer-Encoding")) {
    // This server supports fixed-length streaming, not chunked HTTP framing.
    httpdRejectHeader(conn);
  } else if (httpdHeaderName(h, "Content-Type")) {
    if (os_strstr(h, "multipart/form-data")) {
      // It's multipart form data so let's pull out the boundary for future use
      char *b;
      if ((b = os_strstr(h, "boundary=")) != NULL) {
        conn->post->multipartBoundary =
            b + 7; // move the pointer 2 chars before boundary then fill them with dashes
        conn->post->multipartBoundary[0] = '-';
        conn->post->multipartBoundary[1] = '-';
        //DBG("boundary = %s\n", conn->post->multipartBoundary);
      }
    }
  }
}

//Callback called when there's data available on a socket.
static void ICACHE_FLASH_ATTR httpdRecvCb(void *arg, char *data, unsigned short len) {
  debugConn(arg, "httpdRecvCb");
  struct espconn *pCon = (struct espconn *)arg;
  HttpdConnData *conn = (HttpdConnData *)pCon->reverse;
  if (conn == NULL || conn->post->len == -2)
    return; // aborted/rejected request

  char sendBuff[MAX_SENDBUFF_LEN];
  httpdSetOutputBuffer(conn, sendBuff, sizeof(sendBuff));

  //This is slightly evil/dirty: we abuse conn->post->len as a state variable for where in the http communications we are:
  //==-2: Invalid framing; ignore further bytes until disconnect
  //==-1: Post len unknown because we're still receiving headers
  //==0: No post data
  //>0: Need to receive post data
  //ToDo: See if we can use something more elegant for this.

  for (int x = 0; x < len; x++) {
    if (conn->post->len < 0) {
      //This byte is a header byte.
      if (!httpdAppendHeader(conn, data[x])) {
        httpdRejectHeader(conn);
        return;
      }
      //Scan for /r/n/r/n. Receiving this indicate the headers end.
      if (data[x] == '\n' && (char *)os_strstr(conn->priv->head, "\r\n\r\n") != NULL) {
        //Indicate we're done with the headers.
        conn->post->len = 0;
        conn->post->multipartBoundary = NULL;
        //Reset url data
        conn->url = NULL;
        //Iterate over all received headers and parse them.
        char *p = conn->priv->head;
        while (p < (&conn->priv->head[conn->priv->headPos - 4])) {
          char *e = (char *)os_strstr(p, "\r\n"); //Find end of header line
          if (e == NULL)
            break;                   //Shouldn't happen.
          e[0] = 0;                  //Zero-terminate header
          httpdParseHeader(p, conn); //and parse it.
          if (conn->post->len == -2)
            return;
          p = e + 2; //Skip /r/n (now /0/n)
        }
        //If we don't need to receive post data, we can send the response now.
        if (conn->post->len == 0) {
          httpdProcessRequest(conn);
        }
      }
    } else if (conn->post->len > 0 && conn->post->received < conn->post->len) {
      //This byte is a POST byte.
      conn->post->buff[conn->post->buffLen++] = data[x];
      conn->post->received++;
      if (conn->post->buffLen >= conn->post->buffSize || conn->post->received == conn->post->len) {
        //Received a chunk of post data
        conn->post->buff[conn->post->buffLen] =
            0; //zero-terminate, in case the cgi handler knows it can use strings
        //Send the response.
        httpdProcessRequest(conn);
        conn->post->buffLen = 0;
      }
    }
  }
}

static void ICACHE_FLASH_ATTR httpdDisconCb(void *arg) {
  debugConn(arg, "httpdDisconCb");
  struct espconn *pCon = (struct espconn *)arg;
  HttpdConnData *conn = (HttpdConnData *)pCon->reverse;
  if (conn == NULL)
    return; // aborted connection
  httpdRetireConn(conn);
}

// Callback indicating a failure in the connection. "Recon" is probably intended in the sense
// of "you need to reconnect". Sigh... Note that there is no DisconCb after ReconCb
static void ICACHE_FLASH_ATTR httpdReconCb(void *arg, sint8 err) {
  debugConn(arg, "httpdReconCb");
  struct espconn *pCon = (struct espconn *)arg;
  HttpdConnData *conn = (HttpdConnData *)pCon->reverse;
  if (conn == NULL)
    return; // aborted connection
  DBG("%s***** reset, err=%d\n", connStr, err);
  httpdRetireConn(conn);
}

static void ICACHE_FLASH_ATTR httpdConnectCb(void *arg) {
  debugConn(arg, "httpdConnectCb");
  struct espconn *conn = arg;

  // Find empty conndata in pool
  int i;
  for (i = 0; i < MAX_CONN; i++)
    if (connData[i].conn == NULL)
      break;
  //DBG("Con req, conn=%p, pool slot %d\n", conn, i);
  if (i == MAX_CONN) {
    os_printf("%sHTTP: conn pool overflow!\n", connStr);
    espconn_disconnect(conn);
    return;
  }

#if 0
  int num = 0;
  for (int j = 0; j<MAX_CONN; j++) if (connData[j].conn != NULL) num++;
  DBG("%sConnect (%d open)\n", connStr, num + 1);
#endif

  // Preserve all pool slots without reserving six idle 1 KiB headers.
  // URL, arguments and header values point here until CGI cleanup finishes.
  char *head = os_malloc(MAX_HEAD_LEN + 1);
  if (!head) {
    conn->reverse = NULL;
    espconn_disconnect(conn);
    return;
  }
  head[0] = 0;
  connData[i].priv = &connPrivData[i];
  connData[i].priv->head = head;
  connData[i].conn = conn;
  conn->reverse = connData + i;
  connData[i].priv->headPos = 0;
  connData[i].priv->pendingSend = NULL;
  connData[i].priv->sendFailed = false;
  connData[i].priv->readDeferred = false;
  connData[i].priv->readNotBefore = 0;
  connData[i].priv->cgiIndex = 0;

  esp_tcp *tcp = conn->proto.tcp;
  os_sprintf(connData[i].priv->from, "%d.%d.%d.%d:%d", tcp->remote_ip[0], tcp->remote_ip[1],
             tcp->remote_ip[2], tcp->remote_ip[3], tcp->remote_port);
  connData[i].post = &connPostData[i];
  connData[i].post->buff = NULL;
  connData[i].post->buffLen = 0;
  connData[i].post->received = 0;
  connData[i].post->len = -1;
  connData[i].startTime = system_get_time();
  connData[i].cgiPrivData = NULL;

  espconn_regist_recvcb(conn, httpdRecvCb);
  espconn_regist_reconcb(conn, httpdReconCb);
  espconn_regist_disconcb(conn, httpdDisconCb);
  espconn_regist_sentcb(conn, httpdSentCb);

  espconn_set_opt(conn, ESPCONN_REUSEADDR | ESPCONN_NODELAY);
}

//Httpd initialization routine. Call this to kick off webserver functionality.
void ICACHE_FLASH_ATTR httpdInit(HttpdBuiltInUrl *fixedUrls, int port) {
  int i;

  for (i = 0; i < MAX_CONN; i++) {
    connData[i].conn = NULL;
  }
  httpdConn.type = ESPCONN_TCP;
  httpdConn.state = ESPCONN_NONE;
  httpdTcp.local_port = port;
  httpdConn.proto.tcp = &httpdTcp;
  builtInUrls = fixedUrls;
  DBG("Httpd init, conn=%p\n", &httpdConn);
  espconn_regist_connectcb(&httpdConn, httpdConnectCb);
  espconn_accept(&httpdConn);
  espconn_tcp_set_max_con_allow(&httpdConn, MAX_CONN);
}

// looks up connection handle based on ip / port
HttpdConnData *ICACHE_FLASH_ATTR httpdLookUpConn(uint8_t *ip, int port) {
  int i;

  for (i = 0; i < MAX_CONN; i++) {
    HttpdConnData *conn = connData + i;

    if (conn->conn == NULL)
      continue;
    if (conn->cgi == NULL)
      continue;
    if (conn->conn->proto.tcp->remote_port != port)
      continue;
    if (os_memcmp(conn->conn->proto.tcp->remote_ip, ip, 4) != 0)
      continue;

    return conn;
  }
  return NULL;
}

// this method is used for setting the response of a CGI handler outside of the HTTP callback
// this method useful at the following scenario:
//   Browser -> CGI handler -> MCU request
//   MCU response -> CGI handler -> browser
// when MCU response arrives, the handler looks up connection based on ip/port and call httpdSetCGIResponse with the data to transmit

int ICACHE_FLASH_ATTR httpdSetCGIResponse(HttpdConnData *conn, void *response) {
  char sendBuff[MAX_SENDBUFF_LEN];
  conn->priv->sendBuff = sendBuff;
  conn->priv->sendBuffLen = 0;

  conn->cgiResponse = response;
  httpdProcessRequest(conn);
  conn->cgiResponse = NULL;

  return HTTPD_CGI_DONE;
}
