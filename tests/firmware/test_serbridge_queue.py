#!/usr/bin/env python3
"""Exercise actual block queue ownership, SDK faults and multiple clients."""
import _paths
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[2]
source = (root / 'src/serial/serbridge.c').read_text()
header = (root / 'src/serial/serbridge.h').read_text()
a = source.index('static void ICACHE_FLASH_ATTR\nreleaseCopiedTxBlocks(')
b = source.index('void ICACHE_FLASH_ATTR\nconsole_process', a)
functions = source[a:b]
a = source.index('static bool ICACHE_FLASH_ATTR\nholdUartInput(')
b = source.index('// Generate a reset pulse',a)
input_functions = source[a:b]
a = header.index('typedef struct TxBlock')
b = header.index('} serbridgeConnData;', a) + len('} serbridgeConnData;')
records = header[a:b]
macros = '\n'.join(re.findall(r'^#define (?:TX_BLOCK_BYTES|MAX_TXBUFFER) .+$', header, re.M))
prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef int8_t sint8;
typedef uint16_t uint16;
#define ICACHE_FLASH_ATTR
#define ESPCONN_OK 0
#define os_memcpy memcpy
#define os_memmove memmove
struct Borrow { const uint8_t *data;unsigned length;bool owned; };
struct espconn {void *reverse;struct Borrow pending[4];unsigned count,head;uint32_t sequence;uint8_t wire[100000];unsigned wire_len;};
enum connModes {cmInit,cmPGMInit,cmTransparent,cmPGM,cmTelnet};
static struct {unsigned received,max_callback,rejected,hold_errors,hold_calls,alloc_errors,high_water;unsigned net_send_errors,net_drops,net_alloc_errors,net_packets,net_bytes,net_max_ack_us,net_min_heap,net_queue_high_water;int net_last_error;unsigned net_fail_pending,net_fail_flights,net_fail_age_us,net_fail_sndbuf,net_fail_slots,net_fail_snd_nxt,net_fail_rcv_nxt;} input_stats;
static void sdk_rto_prepare_send(struct espconn *c){(void)c;}
static void sdk_rto_sent(struct espconn *c){(void)c;}
static void sdk_rto_acked(struct espconn *c){(void)c;}
static unsigned copy_mode,defer_copy;
static unsigned allocations,max_request,fail_alloc,heap=64000,credits=2144,disconnected,reject_send,now=100;
static void *os_malloc(size_t n) {
 if(fail_alloc){fail_alloc--;return NULL;}
 if(n>max_request)max_request=n;
 void *p=malloc(n);assert(p);allocations++;return p;
}
static void os_free(void *p){assert(p&&allocations);allocations--;free(p);}
static unsigned system_get_free_heap_size(void){return heap;}
static unsigned system_get_time(void){return now;}
struct espconn_packet {unsigned snd_buf_size,snd_queuelen,packseqno,packseq_nxt;};
static int espconn_get_packet_info(struct espconn *c,struct espconn_packet *p){p->snd_buf_size=credits;p->snd_queuelen=8-2*c->count;p->packseqno=4096;p->packseq_nxt=2048;return 0;}
static int espconn_abort(struct espconn *c){(void)c;disconnected++;return 0;}
static int sdk_tx_sequence(struct espconn *c,uint32_t *out){*out=c->sequence;return 1;}
static int sdk_tx_copy_complete(struct espconn *c,const void *data,uint16_t len,uint32_t end){
 (void)data;(void)len;if(!copy_mode || (int32_t)(c->sequence-end)<0)return 0;
 for(unsigned i=0;i<c->count;i++)if(!c->pending[(c->head+i)%4].owned)return 0;
 return 1;
}
static int espconn_sent(struct espconn *c,uint8_t *data,unsigned size){
 assert(c->count<4&&size<=credits&&size<=536&&size>0);
 unsigned length=reject_send&&size>11?11:size;
 bool owned=copy_mode&&!defer_copy;
 const uint8_t *bytes=data;
 if(owned){uint8_t *saved=malloc(length);assert(saved);memcpy(saved,data,length);bytes=saved;}
 c->pending[(c->head+c->count)%4]=(struct Borrow){bytes,length,owned};c->count++;credits-=length;
 if(!defer_copy)c->sequence+=length;
 if(reject_send){reject_send=0;return -1;}return 0;
}
'''
input_stubs = r'''
#define MAX_CONN 4
#define MAX_RXBUFFER (2*1460)
#define READ_PERI_REG(r) 0U
#define UART_STATUS(n) 0
#define UART0 0
#define UART_TXFIFO_CNT_S 0
#define UART_TXFIFO_CNT 0xff
#define GPIO_OUTPUT_SET(a,b) ((void)(a),(void)(b),resets++)
#define GPIO_DIS_OUTPUT(a) ((void)(a))
static int mcu_reset_pin=0,mcu_isp_pin=1;
static unsigned holds,unholds,resets,uartUsed;
static char uartWire[40000];
static serbridgeConnData connData[MAX_CONN];
static int espconn_recv_hold(struct espconn *s){assert(s);holds++;return 0;}
static int espconn_recv_unhold(struct espconn *s){assert(s);unholds++;return 0;}
static int espconn_disconnect(struct espconn *s){assert(s);disconnected++;return 0;}
static void os_delay_us(unsigned n){(void)n;}
static void serledFlash(unsigned n){(void)n;}
static void programmingState(serbridgeConnData *c,bool on){c->programming_owned=on;}
static void uart0_tx_buffer(char *p,unsigned n){assert(uartUsed+n<=sizeof uartWire);memcpy(uartWire+uartUsed,p,n);uartUsed+=n;}
static uint16 telnetUnwrap(serbridgeConnData *c,uint8_t *p,unsigned n){(void)c;uart0_tx_buffer((char*)p,n);return n;}
'''
tests = r'''
static void init(serbridgeConnData *c,struct espconn *s){memset(c,0,sizeof *c);memset(s,0,sizeof *s);c->conn=s;c->readytosend=true;s->reverse=c;}
static void ack(serbridgeConnData *c){
 struct espconn *s=c->conn;assert(s->count);
 struct Borrow p=s->pending[s->head];
 assert(s->wire_len+p.length<=sizeof s->wire);
 // Read only at completion: ASan catches freed pointers and checks below catch mutation.
 memcpy(s->wire+s->wire_len,p.data,p.length);s->wire_len+=p.length;
 if(p.owned)free((void*)p.data);
 s->head=(s->head+1)%4;s->count--;credits+=p.length;now+=11000;
 serbridgeSentCb(s);
}
static void drain(serbridgeConnData *c){
 unsigned limit=10000;
 while(c->txbufferlen){assert(limit--&&!c->tx_failed);now+=11000;sendtxbuffer(c);assert(c->conn->count);ack(c);}
 assert(!c->txhead&&!c->txtail&&!c->txbuffersent&&!c->txflightcount);
}
static void close_conn(serbridgeConnData *c){
 struct espconn *s=c->conn;
 while(s->count){if(s->pending[s->head].owned)free((void*)s->pending[s->head].data);credits+=s->pending[s->head].length;s->head=(s->head+1)%4;s->count--;}
 s->reverse=NULL;releaseTxQueue(c);serbridgeSentCb(s);
}
static void recoveryTests(void){
 static uint8_t output[4000];for(unsigned i=0;i<sizeof output;i++)output[i]=i%251;
 char input[]="queued input";serbridgeConnData *c=connData;struct espconn s;
 unsigned aborts=disconnected;init(c,&s);credits=0;uartUsed=holds=unholds=0;
 assert(espbuffsend(c,(char*)output,sizeof output)==0&&c->tx_recovering&&!c->tx_failed);
 assert(queueUartInput(c,input,sizeof input));pumpUartInput(NULL);
 assert(!uartUsed&&!unholds&&c->rxbufferlen==sizeof input&&!s.count);
 now+=600000;credits=2144;pumpUartInput(NULL);assert(c->tx_recovering&&s.count==4);
 drain(c);assert(!c->tx_recovering&&disconnected==aborts);
 assert(s.wire_len==sizeof output&&!memcmp(s.wire,output,sizeof output));
 pumpUartInput(NULL);pumpUartInput(NULL);assert(uartUsed==sizeof input&&!memcmp(uartWire,input,sizeof input));
 assert(unholds==1&&!c->rxbufferlen);os_free(c->rxbuffer);c->rxbuffer=NULL;close_conn(c);
 // A quiet small flight can stall too; retry ticks must never duplicate it.
 init(c,&s);credits=2144;uartUsed=0;assert(!espbuffsend(c,(char*)output,64));now+=11000;pumpUartInput(NULL);assert(s.count==1);
 now+=300000;pumpUartInput(NULL);assert(c->tx_recovering);assert(queueUartInput(c,input,sizeof input));
 for(unsigned i=0;i<10;i++)pumpUartInput(NULL);
 assert(!uartUsed&&s.count==1);
 ack(c);assert(!c->tx_recovering);pumpUartInput(NULL);assert(uartUsed==sizeof input&&s.wire_len==64&&!memcmp(s.wire,output,64));os_free(c->rxbuffer);c->rxbuffer=NULL;close_conn(c);
 // One client's stalled output must not pause a healthy client's input.
 struct espconn other;init(c,&s);credits=0;assert(!espbuffsend(c,(char*)output,sizeof output));
 init(c+1,&other);uartUsed=0;assert(queueUartInput(c,input,sizeof input));assert(queueUartInput(c+1,"healthy",7));pumpUartInput(NULL);
 assert(c->rxbufferlen==sizeof input&&uartUsed==7&&!memcmp(uartWire,"healthy",7));os_free((c+1)->rxbuffer);(c+1)->rxbuffer=NULL;close_conn(c+1);(c+1)->conn=NULL;
 // No target reset while recovering; it executes once after recovery.
 c->programming_pending=true;unsigned before=resets;pumpUartInput(NULL);assert(resets==before&&c->programming_pending);
 credits=2144;now+=11000;drain(c);pumpUartInput(NULL);assert(resets==before+2&&!c->programming_pending&&c->programming_owned);pumpUartInput(NULL);assert(resets==before+2);os_free(c->rxbuffer);c->rxbuffer=NULL;close_conn(c);
 // Low heap enters recovery early; a separate resume threshold prevents flapping.
 init(c,&s);credits=0;assert(!espbuffsend(c,(char*)output,64));heap=3000;pumpUartInput(NULL);assert(c->tx_recovering);
 heap=6000;pumpUartInput(NULL);assert(c->tx_recovering);heap=64000;pumpUartInput(NULL);assert(!c->tx_recovering);credits=2144;drain(c);close_conn(c);
 // Exhausting the bounded recovery deadline rejects queued input and aborts once.
 init(c,&s);credits=0;assert(!espbuffsend(c,(char*)output,700));assert(queueUartInput(c,input,sizeof input));
 now+=300000;pumpUartInput(NULL);assert(c->tx_recovering);now+=4999999;pumpUartInput(NULL);assert(!c->tx_failed);
 now++;pumpUartInput(NULL);assert(c->tx_failed&&!c->rxbufferlen&&!c->tx_recovering&&disconnected==aborts+1);
 pumpUartInput(NULL);assert(disconnected==aborts+1);os_free(c->rxbuffer);c->rxbuffer=NULL;close_conn(c);
 // Unsigned microsecond deadlines work across the SDK clock wrap.
 init(c,&s);now=UINT32_MAX-10000;credits=0;assert(!espbuffsend(c,(char*)output,64));now+=300000;pumpUartInput(NULL);assert(c->tx_recovering&&!c->tx_failed);
 credits=2144;drain(c);assert(!c->tx_recovering);close_conn(c);c->conn=NULL;assert(!allocations);
 puts("PASS recovery: delayed ACKs, exact output/input, no duplicate retry, independent clients, deferred reset, heap hysteresis, bounded timeout and clock wrap");
}
int main(void){
 static uint8_t data[60000];for(unsigned i=0;i<sizeof data;i++)data[i]=(i*13)%251;
 serbridgeConnData c;struct espconn s;init(&c,&s);
 // Credit deferral retains ordered bytes; later appends never mutate borrowed data.
 credits=0;assert(espbuffsend(&c,(char*)data,600)==0);assert(c.txbufferlen==600&&allocations==2&&!s.count);
 assert(espbuffsend(&c,(char*)data+600,100)==0);
 credits=2144;now+=11000;assert(sendtxbuffer(&c)==0);assert(s.count==2);
 assert(espbuffsend(&c,(char*)data+700,200)==0);drain(&c);
 assert(s.wire_len==900&&!memcmp(s.wire,data,900)&&allocations==0);
 // Partial native send credit splits a block without releasing it prematurely.
 init(&c,&s);credits=200;now+=11000;assert(espbuffsend(&c,(char*)data,700)==0);
 drain(&c);assert(s.wire_len==700&&!memcmp(s.wire,data,700)&&!allocations);
 // Many allocation/ACK cycles preserve order and do not retain idle buffers.
 init(&c,&s);credits=2144;
 for(unsigned i=0;i<50;i++){assert(espbuffsend(&c,(char*)data+700*i,700)==0);drain(&c);}
 assert(s.wire_len==35000&&!memcmp(s.wire,data,35000)&&!allocations);
 // TCP owns copied bytes: release redundant app storage before the ACK.
 init(&c,&s);credits=2144;copy_mode=1;
 assert(espbuffsend(&c,(char*)data,4096)==0);
 assert(s.count==4 && c.txbufferlen==4096 && c.txbuffersent==2144 && allocations==4);
 drain(&c);assert(s.wire_len==4096&&!memcmp(s.wire,data,4096)&&!allocations);
 // A successful send can still have an unfinished SDK copy; retain its source.
 init(&c,&s);credits=2144;defer_copy=1;
 assert(espbuffsend(&c,(char*)data,536)==0 && allocations==1);
 now+=11000;sendtxbuffer(&c);assert(allocations==1);
 struct Borrow *partial=&s.pending[s.head];uint8_t *finished=malloc(partial->length);
 memcpy(finished,partial->data,partial->length);partial->data=finished;partial->owned=true;
 s.sequence+=partial->length;defer_copy=0;
 sendtxbuffer(&c);assert(!allocations && c.txbufferlen==536 && s.count==1);
 ack(&c);assert(!c.txbufferlen && s.wire_len==536&&!memcmp(s.wire,data,536));
 // Sequence wrap cannot change copy/ACK lifetime decisions.
 init(&c,&s);credits=2144;s.sequence=UINT32_MAX-100;
 assert(espbuffsend(&c,(char*)data,700)==0);drain(&c);
 assert(!allocations && s.wire_len==700&&!memcmp(s.wire,data,700));copy_mode=0;
 // Two small clients allocate two blocks, not two maximum-size queues.
 serbridgeConnData other;struct espconn second;init(&c,&s);init(&other,&second);credits=0;
 assert(espbuffsend(&c,(char*)data,64)==0&&espbuffsend(&other,(char*)data+64,64)==0);
 assert(allocations==2&&max_request==sizeof(TxBlock));credits=2144;drain(&c);drain(&other);
 assert(s.wire_len==64&&!memcmp(s.wire,data,64));assert(second.wire_len==64&&!memcmp(second.wire,data+64,64));
 // Enforce the payload ceiling and abort; buffers release only after SDK cleanup.
 init(&c,&s);credits=0;unsigned before=input_stats.net_drops;c.rxbufferlen=9;c.rxbufferpos=3;
 assert(espbuffsend(&c,(char*)data,MAX_TXBUFFER+20)==-128);
 assert(c.tx_failed&&c.txbufferlen==MAX_TXBUFFER&&input_stats.net_drops==before+20&&disconnected==1);
 assert(!c.rxbufferlen&&!c.rxbufferpos);unsigned aborts=disconnected;assert(espbuffsend(&c,(char*)data,1)==-128&&disconnected==aborts);
 assert(input_stats.net_queue_high_water==MAX_TXBUFFER);
 assert(input_stats.net_fail_pending==MAX_TXBUFFER&&input_stats.net_fail_flights==0);
 assert(input_stats.net_fail_sndbuf==0&&input_stats.net_fail_slots==8);
 assert(input_stats.net_fail_snd_nxt==4096&&input_stats.net_fail_rcv_nxt==2048);
 close_conn(&c);assert(!allocations);
 // Allocation failure and the recovery heap reserve are observable and bounded.
 init(&c,&s);fail_alloc=1;before=input_stats.net_alloc_errors;
 assert(espbuffsend(&c,(char*)data,32)==-128&&input_stats.net_alloc_errors==before+1);close_conn(&c);
 init(&c,&s);heap=1024;assert(espbuffsend(&c,(char*)data,32)==-128);close_conn(&c);heap=64000;
 // A real SDK failure may borrow a prefix: no retry or early free is safe.
 init(&c,&s);credits=2144;reject_send=1;before=input_stats.net_send_errors;
 assert(espbuffsend(&c,(char*)data,536)==-1&&c.tx_failed&&s.count==1);
 assert(input_stats.net_send_errors==before+1&&allocations==1);
 now+=11000;sendtxbuffer(&c);assert(s.count==1);ack(&c);
 assert(s.wire_len==11&&!memcmp(s.wire,data,11)&&allocations==1);
 close_conn(&c);assert(!allocations);
 recoveryTests();
 puts("PASS actual TCP block queue: deferred/partial credit, exact ordering, borrowed-buffer lifetime, small multiple clients, bounds, OOM/reserve and ambiguous SDK failure");
}
'''
build = root / '.build-tests/bridge'
build.mkdir(parents=True, exist_ok=True)
cfile = build / 'test.c'
cfile.write_text(prelude + '\n' + macros + '\n' + records + '\n' + input_stubs + '\n' + functions + input_functions + tests)
subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                str(cfile), '-o', str(build / 'test')], check=True)
subprocess.run([str(build / 'test')], check=True)
