#include "web-server.h"

#include <espconn.h>

#include "espfs.h"
#include "config.h"
#include "cgi.h"
#include "api_json.h"
#include "api_request.h"
#include "operation.h"
#include "serial_api.h"
#include "cmd.h"
#include "serbridge.h"

// the file is responsible for handling user defined web-pages
// - collects HTML files from user image, shows them on the left frame
// - handles JSON data coming from the browser
// - handles SLIP messages coming from MCU

#define MAX_ARGUMENT_BUFFER_SIZE 128
#define HEADER_SIZE 32

uint32_t web_server_cb = 0;

struct ArgumentBuffer {
  char argBuffer[MAX_ARGUMENT_BUFFER_SIZE];
  int argBufferPtr;
  int numberOfArgs;
};

static char *web_server_reasons[] = {
    "load",    // readable name for RequestReason::LOAD
    "refresh", // readable name for RequestReason::REFRESH
    "button",  // readable name for RequestReason::BUTTON
    "submit"   // readable name for RequestReason::SUBMIT
};

// this variable contains the names of the user defined pages
// this information appears at the left frame below of the built in URL-s
// format:    ,"UserPage1", "/UserPage1.html", "UserPage2", "/UserPage2.html",
char *webServerPages = NULL;

char *ICACHE_FLASH_ATTR WEB_UserPages() { return webServerPages ? webServerPages : ""; }

typedef struct {
  char *data;
  unsigned used;
} MenuBuffer;
static int ICACHE_FLASH_ATTR menuEmit(void *context, const char *data, size_t length) {
  MenuBuffer *b = context;
  if (length > 1023 - b->used)
    return 0;
  os_memcpy(b->data + b->used, data, length);
  b->used += length;
  return 1;
}
static void ICACHE_FLASH_ATTR menuPair(ApiJson *j, const char *name, unsigned length) {
  unsigned first = length - 5;
  while (first && name[first - 1] != '/')
    first--;
  char path[262];
  os_memcpy(path, "/user/", 6);
  os_memcpy(path + 6, name, length);
  path[length + 6] = 0;
  apiJsonLiteral(j, ",");
  apiJsonStringN(j, name + first, length - 5 - first);
  apiJsonLiteral(j, ",");
  apiJsonStringN(j, path, length + 6);
}
// Build bounded, escaped custom-page menu entries.
void ICACHE_FLASH_ATTR WEB_BrowseFiles() {
  char buffer[1024];
  MenuBuffer out = {buffer, 0};
  if (espFsIsValid(userPageCtx)) {
    EspFsIterator it;
    espFsIteratorInit(userPageCtx, &it);
    while (espFsIteratorNext(&it)) {
      unsigned length = os_strlen(it.name);
      if (length < 6 || length > 255 || os_strcmp(it.name + length - 5, ".html"))
        continue;
      ApiJson size;
      apiJsonInit(&size, NULL, NULL);
      menuPair(&size, it.name, length);
      if (!size.okay || size.length >= sizeof(buffer) - out.used)
        break;
      ApiJson json;
      apiJsonInit(&json, menuEmit, &out);
      menuPair(&json, it.name, length);
    }
  }
  buffer[out.used] = 0;
  char *next = os_malloc(out.used + 1);
  if (!next)
    return; // preserve a previous valid menu on allocation failure
  os_memcpy(next, buffer, out.used + 1);
  if (webServerPages)
    os_free(webServerPages);
  webServerPages = next;
}

// initializer
void ICACHE_FLASH_ATTR WEB_Init() {
  espFsInit(userPageCtx, (void *)getUserPageSectionStart(), ESPFS_FLASH);
  if (espFsIsValid(userPageCtx))
    os_printf("Valid user file system found!\n");
  else
    os_printf("No user file system found!\n");
  WEB_BrowseFiles(); // collect user defined HTML files
}

// initializes the argument buffer
static void ICACHE_FLASH_ATTR WEB_argInit(struct ArgumentBuffer *argBuffer) {
  argBuffer->numberOfArgs = 0;
  argBuffer->argBufferPtr = 0;
}

// adds an argument to the argument buffer (returns 0 if successful)
static int ICACHE_FLASH_ATTR WEB_addArg(struct ArgumentBuffer *argBuffer, char *arg, int argLen) {
  if (argBuffer->argBufferPtr + argLen + sizeof(int) >= MAX_ARGUMENT_BUFFER_SIZE)
    return -1; // buffer overflow

  os_memcpy(argBuffer->argBuffer + argBuffer->argBufferPtr, &argLen, sizeof(int));

  if (argLen != 0) {
    os_memcpy(argBuffer->argBuffer + argBuffer->argBufferPtr + sizeof(int), arg, argLen);
    argBuffer->numberOfArgs++;
  }

  argBuffer->argBufferPtr += argLen + sizeof(int);
  return 0;
}

// creates and sends a SLIP message from the argument buffer
static void ICACHE_FLASH_ATTR WEB_sendArgBuffer(struct ArgumentBuffer *argBuffer,
                                                HttpdConnData *connData, RequestReason reason) {
  cmdResponseStart(CMD_RESP_CB, web_server_cb, 4 + argBuffer->numberOfArgs);
  uint16_t r = (uint16_t)reason;
  cmdResponseBody(&r, sizeof(uint16_t));                     // 1st argument: reason
  cmdResponseBody(&connData->conn->proto.tcp->remote_ip, 4); // 2nd argument: IP
  cmdResponseBody(&connData->conn->proto.tcp->remote_port, sizeof(uint16_t)); // 3rd argument: port
  cmdResponseBody(connData->url, os_strlen(connData->url));                   // 4th argument: URL

  int p = 0;
  for (int j = 0; j < argBuffer->numberOfArgs; j++) {
    int argLen;
    os_memcpy(&argLen, argBuffer->argBuffer + p, sizeof(int));

    char *arg = argBuffer->argBuffer + p + sizeof(int);
    cmdResponseBody(arg, argLen);
    p += argLen + sizeof(int);
  }

  cmdResponseEnd();
}

// this method processes SLIP data from MCU and converts to JSON
// this method receives JSON from the browser, sends SLIP data to MCU
static int ICACHE_FLASH_ATTR WEB_handleJSONRequest(HttpdConnData *connData) {
  if (deviceOperationBusy() || programmingCB || serialInputPending())
    return apiError(connData, 409, "DEVICE_BUSY", "Wait for the current device operation.");
  if (!flashConfig.slip_enable) {
    errorResponse(connData, 400, "Slip processing is disabled!");
    return HTTPD_CGI_DONE;
  }

  if (web_server_cb == 0) {
    errorResponse(connData, 500, "No MCU callback is registered!");
    return HTTPD_CGI_DONE;
  }
  if (serbridgeInMCUFlashing()) {
    errorResponse(connData, 500, "Slip disabled at uploading program onto the MCU!");
    return HTTPD_CGI_DONE;
  }

  char reasonBuf[16] = {0};
  int i;
  int len = httpdFindArg(connData->getArgs, "reason", reasonBuf, sizeof(reasonBuf));
  if (len <= 0 || len >= (int)sizeof(reasonBuf)) {
    errorResponse(connData, 400, "No reason specified!");
    return HTTPD_CGI_DONE;
  }

  RequestReason reason = INVALID;
  for (i = 0; i < sizeof(web_server_reasons) / sizeof(char *); i++) {
    if (os_strcmp(web_server_reasons[i], reasonBuf) == 0)
      reason = (RequestReason)i;
  }

  if (reason == INVALID) {
    errorResponse(connData, 400, "Invalid reason!");
    return HTTPD_CGI_DONE;
  }

  struct ArgumentBuffer argBuffer;
  WEB_argInit(&argBuffer);

  switch (reason) {
  case BUTTON: {
    char id_buf[40];

    int id_len = httpdFindArg(connData->getArgs, "id", id_buf, sizeof(id_buf));
    if (id_len <= 0) {
      errorResponse(connData, 400, "No button ID specified!");
      return HTTPD_CGI_DONE;
    }
    if (WEB_addArg(&argBuffer, id_buf, id_len)) {
      errorResponse(connData, 400, "Post too large!");
      return HTTPD_CGI_DONE;
    }
  } break;
  case SUBMIT: {
    if (connData->post->received < connData->post->len) {
      errorResponse(connData, 400, "Post too large!");
      return HTTPD_CGI_DONE;
    }

    int bptr = 0;
    int sent_args = 0;
    int max_buf_size = MAX_ARGUMENT_BUFFER_SIZE - HEADER_SIZE - os_strlen(connData->url);

    while (bptr < connData->post->len) {
      char *line = connData->post->buff + bptr;

      char *eo = os_strchr(line, '&');
      if (eo != NULL) {
        *eo = 0;
        bptr = eo - connData->post->buff + 1;
      } else {
        eo = line + os_strlen(line);
        bptr = connData->post->len;
      }

      int len = os_strlen(line);
      while (len >= 1 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
        len--;
      line[len] = 0;

      char *val = os_strchr(line, '=');
      if (val != NULL) {
        *val = 0;
        char *name = line;
        int vblen = os_strlen(val + 1) + 1;
        char value[vblen];
        httpdUrlDecode(val + 1, strlen(val + 1), value, vblen);

        int namLen = os_strlen(name);
        int valLen = os_strlen(value);

        char arg[namLen + valLen + 3];
        int argPtr = 0;
        arg[argPtr++] = (char)WEB_STRING;
        os_strcpy(arg + argPtr, name);
        argPtr += namLen;
        arg[argPtr++] = 0;
        os_strcpy(arg + argPtr, value);
        argPtr += valLen;

        if (sent_args != 0) {
          if (argBuffer.argBufferPtr + argPtr >= max_buf_size) {
            WEB_addArg(&argBuffer, NULL,
                       0); // there's enough room in the buffer for termination block
            WEB_sendArgBuffer(&argBuffer, connData, reason);
            WEB_argInit(&argBuffer);
            sent_args = 0;
          }
        }

        if (WEB_addArg(&argBuffer, arg, argPtr)) {
          errorResponse(connData, 400, "Post too large!");
          return HTTPD_CGI_DONE;
        }
        sent_args++;
      }
    }
  } break;
  case LOAD:
  case REFRESH:
  default:
    break;
  }

  if (WEB_addArg(&argBuffer, NULL, 0)) {
    errorResponse(connData, 400, "Post too large!");
    return HTTPD_CGI_DONE;
  }

  os_printf("Web callback to MCU: %s\n", reasonBuf);

  WEB_sendArgBuffer(&argBuffer, connData, reason);

  if (reason == SUBMIT) {
    httpdStartResponse(connData, 204);
    httpdEndHeaders(connData);
    return HTTPD_CGI_DONE;
  }

  return HTTPD_CGI_MORE;
}

// Serialize directly into the bounded HTTP response; a first pass validates/counts.
static void ICACHE_FLASH_ATTR webResponse(ApiJson *j, const void *data) {
  CmdRequest req = *(const CmdRequest *)data;
  bool first = true;
  apiJsonLiteral(j, "{");
  while (req.arg_num < cmdGetArgc(&req)) {
    unsigned n = cmdArgLen(&req);
    const char *p = (const char *)req.arg_ptr + 2;
    if (!n) {
      cmdSkipArg(&req);
      break;
    }
    if (n > 1024 || !req.end || p + n > (const char *)req.end) {
      j->okay = 0;
      return;
    }
    unsigned name = 1;
    while (name < n && p[name])
      name++;
    if (name == n) {
      j->okay = 0;
      return;
    }
    const char *value = p + name + 1;
    unsigned count = n - name - 1;
    if (!first)
      apiJsonLiteral(j, ",");
    first = false;
    apiJsonStringN(j, p + 1, name - 1);
    apiJsonLiteral(j, ":");
    switch ((unsigned char)p[0]) {
    case WEB_NULL:
      if (count)
        goto invalid;
      apiJsonLiteral(j, "null");
      break;
    case WEB_STRING:
      apiJsonStringN(j, value, count);
      break;
    case WEB_BOOLEAN:
      if (count != 1)
        goto invalid;
      apiJsonBool(j, *value != 0);
      break;
    case WEB_INTEGER: {
      int32_t v;
      if (count != 4)
        goto invalid;
      os_memcpy(&v, value, 4);
      apiJsonSigned(j, v);
      break;
    }
    case WEB_FLOAT: {
      float f;
      if (count != 4)
        goto invalid;
      os_memcpy(&f, value, 4);
      if (!(f >= -2147483000.0f && f <= 2147483000.0f)) {
        apiJsonLiteral(j, "null");
        break;
      }
      int32_t whole = f;
      int fraction = (f - whole) * 1000;
      if (fraction < 0)
        fraction = -fraction;
      if (f < 0 && whole == 0)
        apiJsonLiteral(j, "-");
      apiJsonSigned(j, whole);
      char tail[5];
      os_sprintf(tail, ".%03d", fraction);
      apiJsonRaw(j, tail);
      break;
    }
    // ELClient's explicit raw-JSON API supplies arrays/objects for lists/tables.
    case WEB_JSON: {
      char raw[1025];
      if (!count)
        goto invalid;
      os_memcpy(raw, value, count);
      raw[count] = 0;
      apiJsonRaw(j, raw);
      break;
    }
    default:
      goto invalid;
    }
    cmdSkipArg(&req);
  }
  apiJsonLiteral(j, "}");
  return;
invalid:
  j->okay = 0;
}
static int ICACHE_FLASH_ATTR WEB_handleMCUResponse(HttpdConnData *connData, CmdRequest *response) {
  return apiResponse(connData, 200, webResponse, response);
}

// this method is responsible for the MCU <==JSON==> Browser communication
int ICACHE_FLASH_ATTR WEB_CgiJsonHook(HttpdConnData *connData) {
  if (connData->conn == NULL)
    return HTTPD_CGI_DONE; // Connection aborted. Clean up.

  void *cgiData = connData->cgiData;

  if (cgiData == NULL) {
    connData->cgiData = (void *)1; // indicate, that request was processed
    return WEB_handleJSONRequest(connData);
  }

  if (connData->cgiResponse != NULL) // data from MCU
    return WEB_handleMCUResponse(connData, (CmdRequest *)(connData->cgiResponse));

  return HTTPD_CGI_MORE;
}

// configuring the callback
void ICACHE_FLASH_ATTR WEB_Setup(CmdPacket *cmd) {
  CmdRequest req;
  cmdRequest(&req, cmd);

  if (cmdGetArgc(&req) < 1)
    return;

  cmdPopArg(&req, &web_server_cb, 4); // pop the callback

  os_printf("Web-server connected, cb=0x%x\n", web_server_cb);
}

// this method is called when MCU transmits WEB_DATA command
void ICACHE_FLASH_ATTR WEB_Data(CmdPacket *cmd) {
  CmdRequest req;
  cmdRequest(&req, cmd);

  if (cmdGetArgc(&req) < 2)
    return;

  uint8_t ip[4];
  cmdPopArg(&req, ip, 4); // pop the IP address

  uint16_t port;
  cmdPopArg(&req, &port, 2); // pop the HTTP port

  HttpdConnData *conn = httpdLookUpConn(ip, port); // look up connection based on IP/port
  if (conn != NULL &&
      conn->cgi == WEB_CgiJsonHook) // make sure that the right CGI handler is configured
    httpdSetCGIResponse(conn, &req);
  else
    os_printf("WEB_DATA ignored as no valid HTTP connection found!\n");
}
