#!/usr/bin/env python3
"""Execute actual bridge policy with pinned SDK timeout/congestion branch."""
import _paths
from pathlib import Path
import subprocess, argparse

root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk',type=Path,required=True)
args=parser.parse_args()
sdk=args.sdk
b=root/'.build-tests/sdk-rto'
b.mkdir(parents=True,exist_ok=True)
s=(sdk/'third_party/lwip/core/tcp.c').read_text(encoding='latin1')
at=s.index('        /* Increase the retransmission timer if it is running */')
end=s.index('\n      }\n    }\n    /* Check if',at)
timer=s[at:end]
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define RTO_HOST_TEST 1
#define ICACHE_FLASH_ATTR
#define os_memset memset
#define os_memcmp memcmp
#define os_sprintf sprintf
#define TF_INFR 4
#define ESTABLISHED 4
#define SYN_SENT 2
#define LWIP_DEBUGF(...) do {} while(0)
#define LWIP_MIN(a,b) ((a)<(b)?(a):(b))
typedef uint32_t uint32;
struct pbuf {struct pbuf *next;void *payload;unsigned len;};
struct tcp_seg {struct tcp_seg *next;struct pbuf *p;};
struct tcp_pcb { struct tcp_pcb *next; unsigned state,flags,local_port,remote_port;
 void *callback_arg; uint32_t remote_ip,lastack,snd_lbb; unsigned snd_buf,snd_queuelen; int rtime,rto,sa,sv; unsigned nrtx,cwnd,snd_wnd,ssthresh,mss;
 struct tcp_seg *unacked,*unsent; };
struct esp_tcp {unsigned local_port,remote_port;unsigned char remote_ip[4];};
struct espconn {union {struct esp_tcp *tcp;} proto;};
typedef struct espconn_buf {void *payload;unsigned len,unsent;struct espconn_buf *pnext;} espconn_buf;
typedef struct {struct espconn *pespconn;struct {struct tcp_pcb *pcb;espconn_buf *pbuf;} pcommon;} espconn_msg;
static struct tcp_pcb *tcp_active_pcbs;
static uint32_t now;
uint32 system_get_time(void) {return now;}
void tcp_rexmit_rto(struct tcp_pcb *);
static unsigned system_get_data_of_array_8(const unsigned char *p,unsigned n) {return p[n];}
static const unsigned char tcp_backoff[]={1,2,3,4,5,6,7,7,7,7,7,7,7};
static unsigned vendor_fast,vendor_rto;
void vendor_tcp_rexmit_rto(struct tcp_pcb *p) {vendor_rto++; p->nrtx++;}
void vendor_tcp_rexmit_fast(struct tcp_pcb *p) {vendor_fast++;p->nrtx++;}
void vendor_tcp_slowtmr(void) {
 struct tcp_pcb *pcb; unsigned eff_wnd;
 for(pcb=tcp_active_pcbs;pcb;pcb=pcb->next) {
'''
tests=r'''
int main(void) {
 struct tcp_seg seg={0};
 struct esp_tcp tcp={.local_port=23,.remote_port=4321,.remote_ip={192,168,4,2}};
 struct espconn c={.proto.tcp=&tcp};
 struct tcp_pcb p={.state=4,.local_port=23,.remote_port=4321,.lastack=100,
    .snd_buf=1072,.snd_lbb=100,.rto=4,.rtime=0,.sa=8,.sv=3,.snd_wnd=65535,.cwnd=2144,.mss=536};
 memcpy(&p.remote_ip,tcp.remote_ip,4);tcp_active_pcbs=&p;p.callback_arg=&c;
 sdk_rto_attach(&c);assert(p.snd_buf==1072 && window_grants==0);
 p.unacked=&seg;p.snd_buf=1071;sdk_rto_prepare_send(&c);
 assert(p.snd_buf==1071 && window_declined==1);
 p.unacked=NULL;p.snd_buf=1072;sdk_rto_prepare_send(&c);
 assert(p.snd_buf==2144 && window_grants==1);
 sdk_rto_prepare_send(&c);assert(window_grants==1);
 puts("PASS no credit change during SYN-ACK ownership; grant once before first data after handshake");
 now=1000;sdk_rto_sent(&c);
 assert(tracked[0].count==1 && tracked[0].estimate.samples==0);
 p.unacked=&seg;now=2000;tcp_slowtmr();assert(vendor_rto==0 && p.rto==4);
 puts("PASS no measured RTT: original SDK RTO remains unchanged");
 p.lastack+=536;now=21000;sdk_rto_acked(&c);
 assert(samples==1 && tracked[0].estimate.srtt==20000 && last_base==200000);
 now=30000;sdk_rto_sent(&c);p.rtime=0;
 now=229999;tcp_slowtmr();assert(vendor_rto==0);
 now=230000;tcp_slowtmr();assert(vendor_rto==1 && p.nrtx==1 && p.cwnd==536);
 assert(min_fire_elapsed==200000);
 puts("PASS absolute RTT-derived deadline: no early firing despite slow-timer phase; SDK reduces cwnd");
 now=629999;tcp_slowtmr();assert(vendor_rto==1);
 now=630000;tcp_slowtmr();assert(vendor_rto==2 && p.nrtx==2);
 now=650000;p.lastack+=536;p.nrtx=0;sdk_rto_acked(&c);
 assert(samples==1 && ambiguous==1);
 puts("PASS exponential backoff and Karn exclusion after an RTO");
 now=700000;sdk_rto_sent(&c);now=720000;p.lastack+=536;sdk_rto_acked(&c);
 assert(samples==2);
 now=800000;sdk_rto_sent(&c);p.rtime=0;
 now=900000;p.lastack+=100;tcp_slowtmr();assert(vendor_rto==2);
 now=1099999;tcp_slowtmr();assert(vendor_rto==2);
 now=1100000;tcp_slowtmr();assert(vendor_rto==3);
 puts("PASS partial ACK advances restart the deadline without using an ambiguous RTT sample");
 sdk_rto_detach(&c);sdk_rto_attach(&c);now=1200000;p.nrtx=0;sdk_rto_sent(&c);
 now=1210000;tcp_rexmit_fast(&p);now=1220000;sdk_rto_acked(&c);
 assert(samples==2 && ambiguous==2 && fast_retries==1 && vendor_fast==1);
 puts("PASS fast retransmission also excludes RTT samples");
 sdk_rto_detach(&c);tcp.local_port=80;p.local_port=80;sdk_rto_attach(&c);
 assert(!by_conn(&c));p.rtime=0;p.rto=4;now=1300000;tcp_slowtmr();assert(p.rto==4);
 puts("PASS HTTP and detached connections retain vendor policy");
 tcp.local_port=23;p.local_port=23;sdk_rto_attach(&c);now=UINT32_MAX-10000;
 sdk_rto_sent(&c);now=9999;sdk_rto_acked(&c);
 assert(tracked[0].estimate.srtt==20000);
 struct rto_estimator e={0};rto_sample(&e,20000);rto_sample(&e,40000);
 assert(e.srtt==22500 && e.variation==12500);
 assert(rto_backed_off(&e,0)==200000 && rto_backed_off(&e,2)==800000);
 assert(rto_backed_off(&e,100)==25600000);
 rto_sample(&e,RTO_MAX_US);assert(rto_base(&e)<=RTO_MAX_US);
 e=(struct rto_estimator){.srtt=RTO_MAX_US,.variation=RTO_MAX_US,.samples=1};
 assert(rto_base(&e)==RTO_MAX_US && rto_backed_off(&e,7)==RTO_MAX_US);
 now=1500000;sdk_rto_sent(&c);p.unacked=&seg;
 p.callback_arg=&seg; /* A newly allocated SDK connection reuses this PCB address. */
 p.rtime=0;p.rto=4;now=2000000;
 unsigned previous=vendor_rto;tcp_slowtmr();
 assert(vendor_rto==previous && p.rto==4);
 p.callback_arg=&c;sdk_rto_detach(&c);
 puts("PASS PCB address reuse cannot apply an old connection's RTO policy");
 // Real SDK ownership query: partial writes, borrowing, copies and node reuse.
 char caller[536],sdk_data[536];
 espconn_buf record={.payload=caller,.len=536,.unsent=268};
 espconn_msg message={.pespconn=&c,.pcommon={.pcb=&p,.pbuf=&record}};
 struct pbuf body={.payload=sdk_data,.len=536};seg.p=&body;seg.next=NULL;
 p.callback_arg=&message;p.snd_lbb=1536;p.unacked=&seg;p.unsent=NULL;
 uint32_t cursor;assert(sdk_tx_sequence(&c,&cursor)&&cursor==1536);
 assert(!sdk_tx_copy_complete(&c,caller,536,1536));
 record.unsent=0;p.snd_lbb=1268;assert(!sdk_tx_copy_complete(&c,caller,536,1536));
 p.snd_lbb=1536;body.payload=caller+20;assert(!sdk_tx_copy_complete(&c,caller,536,1536));
 body.payload=sdk_data;assert(sdk_tx_copy_complete(&c,caller,536,1536));
 body.next=&body;assert(!sdk_tx_copy_complete(&c,caller,536,1536));body.next=NULL;
 record.pnext=&record;assert(!sdk_tx_copy_complete(&c,caller,536,1536));record.pnext=NULL;
 message.pespconn=NULL;assert(!sdk_tx_copy_complete(&c,caller,536,1536));message.pespconn=&c;
 p.snd_lbb=32;assert(sdk_tx_copy_complete(&c,caller,536,16));
 puts("PASS actual ownership query: copy frontier, pending SDK record, aliased TCP payload, cycles, connection identity and sequence wrap");
 char out[1024];sdk_rto_json(out);assert(strlen(out)<sizeof(out));
 puts("PASS microsecond wrap, estimator update order, capped arithmetic, backoff and bounded JSON");
}
'''
native=b/'native.c'
native.write_text(prefix+timer+'\n}}\n'+(root/'src/serial/sdk_rto.c').read_text()+tests,encoding='latin1')
subprocess.run(['cc','-I'+str(root/'src/serial'),'-std=c11','-Wall','-Wextra','-Werror',
 '-fsanitize=address,undefined','-g',str(native),'-o',str(b/'native')],check=True)
r=subprocess.run([str(b/'native')],text=True,capture_output=True)
(b/'native.log').write_text(r.stdout+r.stderr)
print(r.stdout,end='');r.check_returncode()
