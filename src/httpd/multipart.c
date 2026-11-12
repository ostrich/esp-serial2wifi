#include <esp8266.h>
#include "multipart.h"
#include "cgi.h"

// Fixed memory irrespective of total body size. Retain a delimiter across HTTP
// chunks; only completed files and a terminal boundary can commit an upload.
#define MP_BUFFER 512
#define MP_BOUNDARY 128
enum { MP_START, MP_HEADERS, MP_DATA, MP_DONE, MP_ERROR };
struct _MultipartCtx {
  MultipartCallback callBack;
  char buffer[MP_BUFFER], delimiter[MP_BOUNDARY + 2];
  unsigned used, delimiterLength, received, files;
  int position, state;
  bool started, file;
};
MultipartCtx *ICACHE_FLASH_ATTR multipartCreateContext(MultipartCallback callback) {
  MultipartCtx *c = os_malloc(sizeof(*c));
  if (!c)
    return NULL;
  os_memset(c, 0, sizeof(*c));
  c->callBack = callback;
  return c;
}
void ICACHE_FLASH_ATTR multipartDestroyContext(MultipartCtx *c) { os_free(c); }
static void ICACHE_FLASH_ATTR consume(MultipartCtx *c, unsigned n) {
  c->used -= n;
  if (c->used)
    os_memmove(c->buffer, c->buffer + n, c->used);
}
static int ICACHE_FLASH_ATTR body(MultipartCtx *c, unsigned n) {
  if (c->file && n) {
    if (c->callBack(FILE_DATA, c->buffer, n, c->position))
      return -1;
    c->position += n;
  }
  consume(c, n);
  return 1;
}
static int ICACHE_FLASH_ATTR process(MultipartCtx *c) {
  if (c->state == MP_DONE) {
    c->used = 0;
    return 0;
  }
  if (c->state == MP_START) {
    unsigned n = c->delimiterLength - 2;
    if (c->used < n + 2)
      return 0;
    if (os_memcmp(c->buffer, c->delimiter + 2, n) || os_memcmp(c->buffer + n, "\r\n", 2))
      return -1;
    consume(c, n + 2);
    c->state = MP_HEADERS;
    return 1;
  }
  if (c->state == MP_HEADERS) {
    unsigned n = 0;
    while (n + 1 < c->used && (c->buffer[n] != '\r' || c->buffer[n + 1] != '\n'))
      n++;
    if (n + 1 >= c->used)
      return 0;
    if (!n) {
      consume(c, 2);
      c->position = 0;
      c->state = MP_DATA;
      return 1;
    }
    // Browsers send quoted filenames in Content-Disposition.
    // Other part/header lines are ignored, including ordinary form fields.
    if (n >= 20 && !os_strncmp(c->buffer, "Content-Disposition:", 20)) {
      for (unsigned i = 20; i + 10 < n; i++)
        if (!os_strncmp(c->buffer + i, "filename=\"", 10)) {
          unsigned first = i + 10, last = first;
          while (last < n && c->buffer[last] != '"')
            last++;
          if (last == n || last == first || last - first > 255)
            return -1;
          for (unsigned k = first; k < last; k++)
            if ((unsigned char)c->buffer[k] < 32 || c->buffer[k] == 127)
              return -1;
          char saved = c->buffer[last];
          c->buffer[last] = 0;
          int result = c->callBack(FILE_START, c->buffer + first, last - first, 0);
          c->buffer[last] = saved;
          if (result)
            return -1;
          c->file = true;
          c->files++;
          break;
        }
    }
    consume(c, n + 2);
    return 1;
  }
  if (c->state == MP_DATA) {
    unsigned keep = c->delimiterLength + 2;
    for (unsigned at = 0; at + c->delimiterLength <= c->used; at++) {
      if (os_memcmp(c->buffer + at, c->delimiter, c->delimiterLength))
        continue;
      if (at + c->delimiterLength + 2 > c->used)
        return at ? body(c, at) : 0;
      const char *suffix = c->buffer + at + c->delimiterLength;
      bool final = suffix[0] == '-' && suffix[1] == '-';
      if (!final && !(suffix[0] == '\r' && suffix[1] == '\n'))
        continue; // payload lookalike
      if (at && body(c, at) < 0)
        return -1;
      if (c->file && c->callBack(FILE_DONE, NULL, 0, c->position))
        return -1;
      c->file = false;
      consume(c, c->delimiterLength + 2);
      c->state = final ? MP_DONE : MP_HEADERS;
      return 1;
    }
    return c->used > keep ? body(c, c->used - keep) : 0;
  }
  return -1;
}
int ICACHE_FLASH_ATTR multipartProcess(MultipartCtx *c, HttpdConnData *conn) {
  if (!conn->conn)
    return HTTPD_CGI_DONE;
  HttpdPostData *post = conn->post;
  if (conn->requestType != HTTPD_METHOD_POST || !post || !post->multipartBoundary ||
      post->len <= 0) {
    errorResponse(conn, 400, "Use a fixed-length multipart POST");
    return HTTPD_CGI_DONE;
  }
  if (!c->started) {
    const char *boundary = post->multipartBoundary;
    unsigned n = os_strlen(boundary);
    if (n >= 4 && boundary[0] == '-' && boundary[1] == '-' && boundary[2] == '"' &&
        boundary[n - 1] == '"') {
      boundary += 3;
      n -= 4;
      c->delimiter[2] = '-';
      c->delimiter[3] = '-';
      if (n + 2 > MP_BOUNDARY)
        goto invalid;
      os_memcpy(c->delimiter + 4, boundary, n);
      n += 2;
    } else {
      if (n < 3 || n > MP_BOUNDARY || boundary[0] != '-' || boundary[1] != '-')
        goto invalid;
      os_memcpy(c->delimiter + 2, boundary, n);
    }
    c->delimiter[0] = '\r';
    c->delimiter[1] = '\n';
    c->delimiterLength = n + 2;
    c->started = true;
    if (c->callBack(FILE_UPLOAD_START, NULL, 0, 0))
      goto invalid;
  }
  if (post->buffLen < 0 || (unsigned)post->buffLen > (unsigned)post->len - c->received)
    goto invalid;
  unsigned offset = 0;
  while (offset < (unsigned)post->buffLen) {
    unsigned n = post->buffLen - offset;
    if (n > MP_BUFFER - c->used)
      n = MP_BUFFER - c->used;
    if (!n)
      goto invalid;
    os_memcpy(c->buffer + c->used, post->buff + offset, n);
    c->used += n;
    offset += n;
    int result;
    while ((result = process(c)) > 0) {
    }
    if (result < 0)
      goto invalid;
  }
  c->received += post->buffLen;
  if (c->received < (unsigned)post->len)
    return HTTPD_CGI_MORE;
  if (c->state != MP_DONE || !c->files || c->callBack(FILE_UPLOAD_DONE, NULL, 0, 0))
    goto invalid;
  httpdStartResponse(conn, 204);
  httpdEndHeaders(conn);
  return HTTPD_CGI_DONE;
invalid:
  c->state = MP_ERROR;
  errorResponse(conn, 400, "Incomplete or invalid custom-page upload");
  return HTTPD_CGI_DONE;
}
