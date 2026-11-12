#include <esp8266.h>
#include "api_request.h"
#include "cgi.h"

static int ICACHE_FLASH_ATTR emit(void *context, const char *data, size_t length) {
  return httpdSend(context, data, length);
}
static void ICACHE_FLASH_ATTR headers(HttpdConnData *conn, int code) {
  noCacheHeaders(conn,code); httpdHeader(conn,"Content-Type","application/json");
  httpdHeader(conn,"X-Content-Type-Options","nosniff"); httpdEndHeaders(conn);
}
int ICACHE_FLASH_ATTR apiErrorFlash(HttpdConnData *conn,int code,const void *name,size_t nameLength,const void *message,size_t messageLength) {
  char text[160];ApiJson json;
  if(nameLength>48||messageLength>sizeof(text)){espconn_disconnect(conn->conn);return HTTPD_CGI_DONE;}
  headers(conn,code);apiJsonInit(&json,emit,conn);
  apiJsonLiteral(&json,"{\"error\":{\"code\":");apiFlashCopy(text,name,nameLength);apiJsonStringN(&json,text,nameLength);
  apiJsonLiteral(&json,",\"message\":");apiFlashCopy(text,message,messageLength);apiJsonStringN(&json,text,messageLength);
  apiJsonLiteral(&json,",\"retryable\":false}}");
  if(!json.okay)espconn_disconnect(conn->conn);
  return HTTPD_CGI_DONE;
}
int ICACHE_FLASH_ATTR apiResponse(HttpdConnData *conn,int code,ApiFormat format,const void *data) {
  ApiJson json; apiJsonInit(&json,NULL,NULL); format(&json,data);
  if (!json.okay || json.length>2304) return apiError(conn,503,"RESPONSE_TOO_LARGE","Response exceeds the available budget.");
  headers(conn,code); apiJsonInit(&json,emit,conn); format(&json,data);
  if (!json.okay) espconn_disconnect(conn->conn);
  return HTTPD_CGI_DONE;
}
bool ICACHE_FLASH_ATTR apiRead(HttpdConnData *conn) {
  if (!conn->conn) return false;
  if (conn->requestType!=HTTPD_METHOD_GET || (conn->getArgs && *conn->getArgs)) {
    apiError(conn,405,"METHOD_NOT_ALLOWED","Use GET without query parameters."); return false;
  }
  return true;
}
bool ICACHE_FLASH_ATTR apiWrite(HttpdConnData *conn) {
  if(!conn->conn)return false;
  if(conn->requestType!=HTTPD_METHOD_POST||(conn->getArgs&&*conn->getArgs)) {
    apiError(conn,405,"METHOD_NOT_ALLOWED","Use POST without query parameters.");return false;
  }
  char guard[8];httpdGetHeader(conn,"X-Serial2WiFi-Request",guard,sizeof guard);
  if(os_strcmp(guard,"1")){apiError(conn,415,"REQUEST_FORMAT","Use the supported request header.");return false;}
  return true;
}
static int ICACHE_FLASH_ATTR hex(unsigned char c) {
  if (c>='0'&&c<='9')return c-'0';
  if (c>='a'&&c<='f')return c-'a'+10;
  if (c>='A'&&c<='F')return c-'A'+10;
  return -1;
}
static bool ICACHE_FLASH_ATTR decode(const char *start,const char *end,char *out,unsigned capacity) {
  unsigned n=0;
  while(start<end) {
    unsigned char c=*start++;
    if(c=='+')c=' ';
    else if(c=='%') {
      if(end-start<2)return false;
      int a=hex(start[0]),b=hex(start[1]); if(a<0||b<0)return false;
      c=(a<<4)|b; start+=2;
    }
    if(c<32||c==127||n+1>=capacity)return false;
    out[n++]=c;
  }
  out[n]=0; return true;
}
bool ICACHE_FLASH_ATTR apiFormFields(HttpdConnData *conn,ApiField *fields,unsigned count,unsigned required) {
  char type[80];if(!apiWrite(conn))return false;
  httpdGetHeader(conn,"Content-Type",type,sizeof(type));
  if(os_strcmp(type,"application/x-www-form-urlencoded")&&os_strcmp(type,"application/x-www-form-urlencoded;charset=UTF-8")) {
    apiError(conn,415,"REQUEST_FORMAT","Use the supported form body.");return false;
  }
  if(!conn->post || conn->post->len<0 || conn->post->len>1024 ||
      conn->post->received!=conn->post->len || conn->post->buffLen!=conn->post->len || !conn->post->buff) {
    apiError(conn,413,"BODY_LIMIT","Send one complete body of at most 1024 bytes."); return false;
  }
  for(unsigned i=0;i<count;i++){fields[i].present=false;fields[i].value[0]=0;}
  const char *p=conn->post->buff,*end=p+conn->post->len;
  while(p<end) {
    const char *amp=p,*equals=p;char key[40];
    while(amp<end && *amp!='&')amp++;
    while(equals<amp && *equals!='=')equals++;
    if(equals==amp || !decode(p,equals,key,sizeof(key)))goto invalid;
    unsigned i=0;while(i<count && os_strcmp(key,fields[i].name))i++;
    if(i==count || fields[i].present || !decode(equals+1,amp,fields[i].value,fields[i].capacity))goto invalid;
    fields[i].present=true;p=amp;
    if(p<end && ++p==end)goto invalid;
  }
  for(unsigned i=0;i<required;i++)if(!fields[i].present)goto invalid;
  return true;
invalid:
  apiError(conn,422,"INVALID_FIELDS","Check the required fields, encoding and length limits."); return false;
}

bool ICACHE_FLASH_ATTR apiForm(HttpdConnData *conn,ApiField *fields,unsigned count) {
  return apiFormFields(conn,fields,count,count);
}
