#ifndef SERIAL2WIFI_API_REQUEST_H
#define SERIAL2WIFI_API_REQUEST_H
#include "httpd.h"
#include "api_json.h"
typedef void (*ApiFormat)(ApiJson *, const void *);
typedef struct { const char *name; char *value; unsigned capacity; bool present; } ApiField;
int apiResponse(HttpdConnData *, int, ApiFormat, const void *);
int apiErrorFlash(HttpdConnData *,int,const void *,size_t,const void *,size_t);
#define apiError(conn,status,code,message) ({ API_FLASH_STORAGE(_code,code); API_FLASH_STORAGE(_message,message); apiErrorFlash(conn,status,_code,sizeof(code)-1,_message,sizeof(message)-1); })
bool apiFormFields(HttpdConnData *, ApiField *, unsigned count, unsigned required);
bool apiForm(HttpdConnData *, ApiField *, unsigned);
bool apiWrite(HttpdConnData *);
bool apiRead(HttpdConnData *);
#endif
