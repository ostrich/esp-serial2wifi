#include <esp8266.h>
#include "serial_api.h"
#include "api_request.h"
#include "operation.h"
#include "config.h"
#include "console.h"
#include "uart.h"
#include "serbridge.h"

static char *sendBuffer;
static uint16_t sendLength,sendPosition;
static uint32_t sendCancelled;
static ETSTimer sendTimer;
bool ICACHE_FLASH_ATTR serialInputPending(void){return sendBuffer!=NULL;}
static void ICACHE_FLASH_ATTR pump(void *unused) {
  (void)unused;
  if(!sendBuffer)return;
  if(deviceOperationBusy()||programmingCB||serbridgeInMCUFlashing()) {
    sendCancelled+=sendLength-sendPosition;sendPosition=sendLength;
  } else {
    unsigned n=uart0_tx_available(),remaining=sendLength-sendPosition;
    if(n>remaining)n=remaining;
    if(n) {uart0_tx_buffer(sendBuffer+sendPosition,n);sendPosition+=n;}
  }
  if(sendPosition==sendLength) {
    os_free(sendBuffer);sendBuffer=NULL;sendLength=sendPosition=0;return;
  }
  os_timer_arm(&sendTimer,5,0);
}
static bool ICACHE_FLASH_ATTR number(const char **input,uint32_t *out) {
  const char *p=*input;uint32_t n=0;
  if(*p<'0'||*p>'9')return false;
  do {unsigned digit=*p++-'0';if(n>(0xffffffffU-digit)/10)return false;n=n*10+digit;}while(*p>='0'&&*p<='9');
  *out=n;*input=p;return true;
}
static uint8_t ICACHE_FLASH_ATTR captureByte(const void *data,unsigned index){return consoleCaptureByte(data,index);}
static void ICACHE_FLASH_ATTR captureFormat(ApiJson *j,const void *data) {
  const ConsoleCapture *c=data;
  apiJsonLiteral(j,"{\"schema\":1,\"boot\":");apiJsonUnsigned(j,c->boot);
  apiJsonLiteral(j,",\"epoch\":");apiJsonUnsigned(j,c->epoch);
  apiJsonLiteral(j,",\"start\":");apiJsonUnsigned(j,c->start);
  apiJsonLiteral(j,",\"end\":");apiJsonUnsigned(j,c->end);
  apiJsonLiteral(j,",\"reason\":");apiJsonString(j,c->reason,24);
  apiJsonLiteral(j,",\"lost\":");if(!c->reason||!os_strcmp(c->reason,"gap"))apiJsonUnsigned(j,c->lost);else apiJsonLiteral(j,"null");
  apiJsonLiteral(j,",\"encoding\":\"base64\",\"data\":\"");
  apiJsonBase64(j,c,c->length,captureByte);
  apiJsonLiteral(j,"\"}");
}
int ICACHE_FLASH_ATTR apiSerialCapture(HttpdConnData *conn) {
  if(!apiRead(conn))return HTTPD_CGI_DONE;
  uint32_t boot=0,epoch=0,cursor=0;bool supplied=false;
  const char *base="/api/v1/serial/capture",*p=conn->url;
  size_t n=os_strlen(base);
  if(!p||os_strncmp(p,base,n))return apiError(conn,404,"CAPTURE_PATH","Unknown capture path.");
  p+=n;
  if(*p) {
    supplied=true;
    if(*p++!='/'||!number(&p,&boot)||*p++!='/'||!number(&p,&epoch)||*p++!='/'||!number(&p,&cursor)||*p)
      return apiError(conn,422,"INVALID_CURSOR","Use the capture cursor returned by this device.");
  }
  ConsoleCapture window;consoleCapture(&window,supplied,boot,epoch,cursor);
  return apiResponse(conn,200,captureFormat,&window);
}
// One live browser stream owns a FIFO independent of lossy capture readers.
// HTTP/1.0 close-delimited SSE needs no chunked framing or response-sized ring copy.
typedef struct {uint32_t heartbeat;} LiveCapture;
static HttpdConnData *liveOwner;
static int ICACHE_FLASH_ATTR liveEmit(void *context,const char *data,size_t length) {
  return httpdSend(context,data,length);
}
int ICACHE_FLASH_ATTR apiSerialLive(HttpdConnData *conn) {
  LiveCapture *live=conn->cgiData;
  if(!conn->conn) {
    if(live)os_free(live);conn->cgiData=NULL;if(liveOwner==conn){consoleStreamClose();liveOwner=NULL;}
    return HTTPD_CGI_DONE;
  }
  if(!live) {
    if(!apiRead(conn))return HTTPD_CGI_DONE;
    if(liveOwner||programmingCB||serbridgeInMCUFlashing()||serbridge_client_count())
      return apiError(conn,409,"TERMINAL_BUSY","Close the other live terminal or finish programming first.");
    live=os_zalloc(sizeof(*live));if(!live)return apiError(conn,503,"TERMINAL_MEMORY","Not enough memory for a live terminal.");
    if(!consoleStreamOpen()){os_free(live);return apiError(conn,409,"TERMINAL_BUSY","Another live terminal owns the output stream.");}
    // UART/HTTP tasks are cooperative: snapshot after opening the FIFO, without
    // yielding, so its first byte starts exactly where this history ends.
    ConsoleCapture window;consoleCapture(&window,false,0,0,0);
    live->heartbeat=system_get_time();
    conn->cgiData=live;liveOwner=conn;
    httpdStartResponse(conn,200);httpdHeader(conn,"Content-Type","text/event-stream");
    httpdHeader(conn,"Cache-Control","no-store");httpdEndHeaders(conn);httpdReleaseHeaders(conn);
    // History is output-only. The browser must finish parsing it with replies muted.
    int sent=httpdSend(conn,"event: ready\ndata: ",-1);ApiJson json;apiJsonInit(&json,liveEmit,conn);captureFormat(&json,&window);if(!httpdSend(conn,"\n\n",2))sent=0;
    if(!sent||!json.okay){consoleStreamClose();os_free(live);conn->cgiData=NULL;liveOwner=NULL;espconn_disconnect(conn->conn);return HTTPD_CGI_DONE;}
    return HTTPD_CGI_MORE;
  }
  if(programmingCB||serbridgeInMCUFlashing()||serbridge_client_count()) {
    httpdSend(conn,"event: stopped\ndata: ",-1);
    httpdSend(conn,serbridge_client_count()?"TCP serial client connected":"Programming started",-1);httpdSend(conn,"\n\n",2);
    consoleStreamClose();os_free(live);conn->cgiData=NULL;liveOwner=NULL;return HTTPD_CGI_DONE;
  }
  ConsoleCapture window;consoleStreamCapture(&window);
  if(window.length||window.reason) {
    httpdSend(conn,"event: output\ndata: ",-1);ApiJson json;apiJsonInit(&json,liveEmit,conn);captureFormat(&json,&window);httpdSend(conn,"\n\n",2);
    if(!json.okay){consoleStreamClose();os_free(live);conn->cgiData=NULL;liveOwner=NULL;espconn_disconnect(conn->conn);return HTTPD_CGI_DONE;}
    consoleStreamConsume(window.length);
    live->heartbeat=system_get_time();
    if(window.reason){consoleStreamClose();os_free(live);conn->cgiData=NULL;liveOwner=NULL;return HTTPD_CGI_DONE;}
  } else if((uint32_t)(system_get_time()-live->heartbeat)>=1000000U) {
    httpdSend(conn,": alive\n\n",-1);live->heartbeat=system_get_time();
  } else httpdWait(conn,30);
  return HTTPD_CGI_MORE;
}
static void ICACHE_FLASH_ATTR serialFormat(ApiJson *j,const void *unused) {
  (void)unused;
  apiJsonLiteral(j,"{\"schema\":1,\"baud\":");apiJsonUnsigned(j,flashConfig.baud_rate);
  apiJsonLiteral(j,",\"data_bits\":");apiJsonUnsigned(j,flashConfig.data_bits+5);
  apiJsonLiteral(j,",\"parity\":");apiJsonString(j,flashConfig.parity==NONE_BITS?"N":flashConfig.parity==ODD_BITS?"O":"E",1);
  apiJsonLiteral(j,",\"stop_bits\":");apiJsonString(j,flashConfig.stop_bits==ONE_STOP_BIT?"1":flashConfig.stop_bits==TWO_STOP_BIT?"2":"1.5",3);
  apiJsonLiteral(j,",\"clients\":");apiJsonUnsigned(j,serbridge_client_count());
  apiJsonLiteral(j,",\"ports\":[");unsigned mask=serbridge_listener_mask();
  if(mask&1)apiJsonLiteral(j,"23");if((mask&3)==3)apiJsonLiteral(j,",");if(mask&2)apiJsonLiteral(j,"2323");
  apiJsonLiteral(j,"],\"programming\":");apiJsonBool(j,programmingCB||serbridgeInMCUFlashing());
  apiJsonLiteral(j,",\"queued_bytes\":");apiJsonUnsigned(j,sendLength-sendPosition);
  apiJsonLiteral(j,",\"cancelled_bytes\":");apiJsonUnsigned(j,sendCancelled);
  apiJsonLiteral(j,",\"reset_pin\":");if(flashConfig.reset_pin<0)apiJsonLiteral(j,"null");else apiJsonUnsigned(j,flashConfig.reset_pin);
  apiJsonLiteral(j,",\"stream_queued_bytes\":");apiJsonUnsigned(j,consoleStreamPending());
  apiJsonLiteral(j,",\"stream_high_water\":");apiJsonUnsigned(j,consoleStreamHighWater());
  apiJsonLiteral(j,",\"stream_lost_bytes\":");apiJsonUnsigned(j,consoleStreamLost());
  apiJsonLiteral(j,",\"stream_total_lost_bytes\":");apiJsonUnsigned(j,consoleStreamTotalLost());
  apiJsonLiteral(j,",\"capture_bytes\":");apiJsonUnsigned(j,CONSOLE_BUFFER_BYTES-1);
  apiJsonLiteral(j,"}");
}
int ICACHE_FLASH_ATTR apiSerial(HttpdConnData *conn) {
  if(!apiRead(conn))return HTTPD_CGI_DONE;
  return apiResponse(conn,200,serialFormat,NULL);
}
static bool ICACHE_FLASH_ATTR idle(HttpdConnData *conn) {
  if(deviceOperationBusy()||programmingCB||serbridgeInMCUFlashing()) {
    apiError(conn,409,"PROGRAMMING_BUSY","Finish attached-MCU programming before using serial controls.");return false;
  }
  if(sendBuffer) {apiError(conn,409,"SERIAL_BUSY","A prior command is still draining to UART; wait before changing serial state.");return false;}
  return true;
}
int ICACHE_FLASH_ATTR apiSerialUART(HttpdConnData *conn) {
  char baud[12],bits[3],parity[2],stop[4];
  ApiField fields[]={{"baud",baud,sizeof baud,0},{"data_bits",bits,sizeof bits,0},{"parity",parity,sizeof parity,0},{"stop_bits",stop,sizeof stop,0}};
  if(!apiForm(conn,fields,4)||!idle(conn))return HTTPD_CGI_DONE;
  uint32_t rate;const char *p=baud;
  if(!number(&p,&rate)||*p||rate<300||rate>1000000||bits[0]<'5'||bits[0]>'8'||bits[1]||
      !parity[0]||parity[1]||(parity[0]!='N'&&parity[0]!='O'&&parity[0]!='E')||
      (os_strcmp(stop,"1")&&os_strcmp(stop,"1.5")&&os_strcmp(stop,"2")))
    return apiError(conn,422,"INVALID_UART","Check baud, data bits, parity and stop bits.");
  int32_t oldRate=flashConfig.baud_rate;int8_t oldBits=flashConfig.data_bits,oldParity=flashConfig.parity,oldStop=flashConfig.stop_bits;
  flashConfig.baud_rate=rate;flashConfig.data_bits=bits[0]-'5';
  flashConfig.parity=parity[0]=='N'?NONE_BITS:parity[0]=='O'?ODD_BITS:EVEN_BITS;
  flashConfig.stop_bits=!os_strcmp(stop,"1")?ONE_STOP_BIT:!os_strcmp(stop,"2")?TWO_STOP_BIT:ONE_HALF_STOP_BIT;
  if(!configSave()) {
    flashConfig.baud_rate=oldRate;flashConfig.data_bits=oldBits;flashConfig.parity=oldParity;flashConfig.stop_bits=oldStop;
    return apiError(conn,503,"SAVE_FAILED","UART settings could not be saved; reload before retrying.");
  }
  uart0_config(flashConfig.data_bits,flashConfig.parity,flashConfig.stop_bits);uart0_baud(rate);
  return apiResponse(conn,200,serialFormat,NULL);
}
static void ICACHE_FLASH_ATTR accepted(ApiJson *j,const void *data) {
  apiJsonLiteral(j,"{\"schema\":1,\"accepted_bytes\":");apiJsonUnsigned(j,*(const uint32_t*)data);apiJsonLiteral(j,"}");
}
int ICACHE_FLASH_ATTR apiSerialSend(HttpdConnData *conn) {
  char hex[513],bytes[256];ApiField field={"hex",hex,sizeof hex,0};
  if(!apiForm(conn,&field,1)||!idle(conn))return HTTPD_CGI_DONE;
  unsigned length=os_strlen(hex);if(!length||(length&1))goto invalid;
  for(unsigned i=0;i<length;i++) {
    int c=(unsigned char)hex[i];int v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
    if(v<0)goto invalid;
    if(!(i&1))bytes[i/2]=v<<4;else bytes[i/2]|=v;
  }
  uint32_t count=length/2;sendBuffer=os_malloc(count);
  if(!sendBuffer)return apiError(conn,503,"SERIAL_MEMORY","Not enough memory to queue this command.");
  os_memcpy(sendBuffer,bytes,count);sendLength=count;sendPosition=0;
  os_timer_disarm(&sendTimer);os_timer_setfn(&sendTimer,pump,NULL);os_timer_arm(&sendTimer,1,0);
  return apiResponse(conn,200,accepted,&count);
invalid:
  return apiError(conn,422,"INVALID_SERIAL_DATA","Send 1 to 256 bytes encoded as hexadecimal.");
}
static int ICACHE_FLASH_ATTR action(HttpdConnData *conn,bool reset) {
  char value[16];ApiField field={"action",value,sizeof value,0};
  if(!apiForm(conn,&field,1))return HTTPD_CGI_DONE;
  if(os_strcmp(value,reset?"reset_mcu":"clear_capture"))return apiError(conn,422,"INVALID_ACTION","Choose an explicit serial action.");
  if(reset) {
    if(!idle(conn))return HTTPD_CGI_DONE;
    if(flashConfig.reset_pin<0)return apiError(conn,409,"NO_RESET_PIN","Configure an attached-MCU reset pin first.");
    serbridgeReset();
  }else consoleCaptureClear();
  return apiResponse(conn,200,serialFormat,NULL);
}
int ICACHE_FLASH_ATTR apiSerialClear(HttpdConnData *conn){return action(conn,false);}
int ICACHE_FLASH_ATTR apiSerialReset(HttpdConnData *conn){return action(conn,true);}
