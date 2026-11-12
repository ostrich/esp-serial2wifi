#include <esp8266.h>
#include "network_config.h"

bool ICACHE_FLASH_ATTR networkIP(const char *text,uint32_t *result) {
  uint32_t value=0;
  for(unsigned i=0;i<4;i++) {
    if(*text<'0'||*text>'9')return false;
    unsigned octet=0,n=0;
    while(*text>='0'&&*text<='9') {octet=octet*10+*text++-'0';if(++n>3||octet>255)return false;}
    value|=octet<<(i*8);
    if(i<3 ? *text++!='.' : *text!=0)return false;
  }
  *result=value;return true;
}
static bool ICACHE_FLASH_ATTR unicast(uint32_t ip) {
  unsigned first=ip&255;
  return first && first!=127 && first<224 && ip!=0xffffffffU && ip!=0;
}
bool ICACHE_FLASH_ATTR networkAPValid(uint32_t ip) {
  unsigned host=ip>>24;
  return unicast(ip)&&host>0&&host<255;
}
bool ICACHE_FLASH_ATTR networkStatic(const char *ip,const char *mask,const char *gateway,
    const char *dns1,const char *dns2,uint32_t out[5]) {
  const char *values[]={ip,mask,gateway,dns1,dns2};
  for(unsigned i=0;i<5;i++)if(!networkIP(values[i],out+i))return false;
  /* Convert the network-order mask to a host integer without depending on SDK macros. */
  uint32_t m=out[1],big=((m&255)<<24)|((m&0xff00)<<8)|((m>>8)&0xff00)|(m>>24);
  uint32_t host=~big;
  if(!big||host<3||(host&(host+1)))return false; /* contiguous /1 .. /30 */
  if(!unicast(out[0])||(out[2]&&!unicast(out[2]))||(out[3]&&!unicast(out[3]))||(out[4]&&!unicast(out[4])))return false;
  if(out[2]&&((out[0]&m)!=(out[2]&m)||out[0]==out[2]))return false;
  for(unsigned i=0;i<3;i+=2)if(out[i]&&(!(out[i]&~m)||(out[i]&~m)==~m))return false;
  return true;
}
bool ICACHE_FLASH_ATTR networkPassword(const char *value,bool protected) {
  size_t n=os_strlen(value);
  if(!protected)return n==0;
  if(n>=8&&n<=63) {
    for(size_t i=0;i<n;i++)if((unsigned char)value[i]<32||(unsigned char)value[i]>126)return false;
    return true;
  }
  if(n!=64)return false;
  for(unsigned i=0;i<64;i++)if(!((value[i]>='0'&&value[i]<='9')||
      (value[i]>='a'&&value[i]<='f')||(value[i]>='A'&&value[i]<='F')))return false;
  return true;
}
bool ICACHE_FLASH_ATTR networkHostname(const char *value) {
  size_t n=os_strlen(value);if(!n||n>31||value[0]=='-'||value[n-1]=='-')return false;
  for(size_t i=0;i<n;i++)if(!((value[i]>='a'&&value[i]<='z')||
      (value[i]>='A'&&value[i]<='Z')||(value[i]>='0'&&value[i]<='9')||value[i]=='-'))return false;
  return true;
}
bool ICACHE_FLASH_ATTR networkMac(const char *value,uint8_t out[6]) {
  if(os_strlen(value)!=17)return false;
  for(unsigned i=0;i<6;i++) {
    unsigned byte=0;
    for(unsigned j=0;j<2;j++) {
      unsigned char c=*value++;unsigned v;
      if(c>='0'&&c<='9')v=c-'0';else if(c>='a'&&c<='f')v=c-'a'+10;
      else if(c>='A'&&c<='F')v=c-'A'+10;else return false;
      byte=(byte<<4)|v;
    }
    out[i]=byte;if(i<5&&*value++!=':')return false;
  }
  unsigned any=0;for(unsigned i=0;i<6;i++)any|=out[i];
  return any&&!(out[0]&1);
}
