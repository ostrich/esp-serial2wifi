#!/usr/bin/env python3
"""Actual Telnet UART settings parser with fragmented/interleaved clients."""
import _paths
from pathlib import Path
import os,subprocess,shutil,socket,pty,select,termios,threading,time,struct
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'.build-tests/rfc2217';OUT.mkdir(parents=True,exist_ok=True)
s=(ROOT/'src/serial/serbridge.c').read_text();functions=s[s.index('#define IAC'):s.index('static bool ICACHE_FLASH_ATTR\nholdUartInput(')]
cleanup=s[s.index('static void ICACHE_FLASH_ATTR\nserbridgeDisconCb('):s.index('// Connection reset callback')]
functions+='\nstatic void releaseTxQueue(serbridgeConnData *c){(void)c;}\nstatic void sdk_rto_detach(struct espconn *c){(void)c;}\n'+cleanup
source=r'''
#include <assert.h>
#include <esp8266.h>
#include "serbridge.h"
#include "uart.h"
#include "config.h"
#define ESPCONN_OK 0
#define os_printf(...) ((void)0)
static unsigned lastPin,lastLevel;
#define GPIO_OUTPUT_SET(a,b) ((void)(lastPin=(a)),(void)(lastLevel=(b)))
#define GPIO_DIS_OUTPUT(a) ((void)0)
#define PIN_FUNC_SELECT(a,b) ((void)0)
#define READ_PERI_REG(a) 0U
static void os_delay_us(unsigned n){(void)n;}
FlashConfig flashConfig;
static int8_t mcu_reset_pin=-1,mcu_isp_pin=-1;
static uint8_t in_mcu_flashing;
static unsigned char response[128],raw[128];static unsigned used,rawUsed,writes,formats,baudCalls,lastBaud,registerBits;
static bool failSave,pauseResponse,failResponse;
static int espbuffsend(serbridgeConnData *c,const char *p,uint16 n){if(pauseResponse)c->tx_recovering=true;if(failResponse)c->tx_failed=true;assert(used+n<=sizeof response);memcpy(response+used,p,n);used+=n;return 0;}
bool configSave(void){writes++;return !failSave;}
void uart0_config(uint8_t b,uint8_t p,uint8_t s){registerBits=CALC_UARTMODE(b,p,s);formats++;}
void uart0_baud(int n){baudCalls++;lastBaud=n;}
void uart0_write_char(char c){assert(rawUsed<sizeof raw);raw[rawUsed++]=c;}
'''+functions+r'''
static unsigned packet(unsigned char *out,unsigned command,const unsigned char *data,unsigned n){
 unsigned used=0;out[used++]=IAC;out[used++]=SB;out[used++]=44;out[used++]=command;
 for(unsigned i=0;i<n;i++){out[used++]=data[i];if(data[i]==IAC)out[used++]=IAC;}
 out[used++]=IAC;out[used++]=SE;return used;
}
static void setting(serbridgeConnData *c,unsigned cmd,unsigned char value){unsigned char p[16];unsigned n=packet(p,cmd,&value,1);used=0;for(unsigned i=0;i<n;i++)telnetUnwrap(c,p+i,1);}
static void expect(unsigned cmd,unsigned char value){assert(used==7&&response[0]==255&&response[1]==250&&response[2]==44&&response[3]==cmd+100&&response[4]==value&&response[5]==255&&response[6]==240);}
int main(int argc,char **argv){
 if(argc>1) {
  (void)argv;serbridgeConnData c={.conn_mode=cmTelnet};
  for(;;) {
   unsigned char header[2],data[64];
   if(fread(header,1,2,stdin)!=2)break;
   unsigned n=header[0]|header[1]<<8;assert(n<=sizeof data);
   assert(fread(data,1,n,stdin)==n);used=rawUsed=0;telnetUnwrap(&c,data,n);
   unsigned char sizes[4]={used,used>>8,rawUsed,rawUsed>>8};
   fwrite(sizes,1,4,stdout);fwrite(response,1,used,stdout);fwrite(raw,1,rawUsed,stdout);fflush(stdout);
  }
  return 0;
 }
 serbridgeConnData probe={0};unsigned char first[]={IAC},next[]={DO,BinaryOpt};
 assert(telnetProbe(&probe,first,1)==-1&&probe.telnet_probe_iac);
 assert(telnetProbe(&probe,next,2)==1&&probe.telnet_state==TN_iac);
 used=0;telnetUnwrap(&probe,next,2);assert(used==9&&!rawUsed);
 unsigned char confirm[]={IAC,DO,EchoOpt,IAC,DO,SgaOpt,IAC,WILL,BinaryOpt};
 used=0;telnetUnwrap(&probe,confirm,sizeof confirm);assert(used==3&&response[1]==DO);
 used=0;telnetUnwrap(&probe,confirm,sizeof confirm);assert(!used); // no negotiation loops
 unsigned char disable[]={IAC,DONT,EchoOpt};telnetUnwrap(&probe,disable,3);assert(used==3&&response[1]==WONT);
 used=0;telnetUnwrap(&probe,disable,3);assert(!used);
 probe=(serbridgeConnData){0};assert(telnetProbe(&probe,first,1)==-1);unsigned char x='X';assert(telnetProbe(&probe,&x,1)==2);
 unsigned char wire[]={0,'A',255,'\r','B'};probe.conn_mode=cmTelnet;used=0;telnetSendData(&probe,(char *)wire,sizeof wire);
 unsigned char nvt[]={0,'A',255,255,'\r',0,'B'};assert(used==sizeof nvt&&!memcmp(response,nvt,used));
 probe.telnet_local=1;used=0;telnetSendData(&probe,(char *)wire,sizeof wire);
 unsigned char binary[]={0,'A',255,255,'\r','B'};assert(used==sizeof binary&&!memcmp(response,binary,used));
 probe.conn_mode=cmTransparent;used=0;telnetSendData(&probe,(char *)wire,sizeof wire);assert(used==sizeof wire&&!memcmp(response,wire,used));
 probe=(serbridgeConnData){0};rawUsed=0;unsigned char cr[]={'\r',0,'X'};telnetUnwrap(&probe,cr,3);assert(rawUsed==2&&raw[0]=='\r'&&raw[1]=='X');
 probe.telnet_remote=1;rawUsed=0;telnetUnwrap(&probe,cr,3);assert(rawUsed==3&&raw[1]==0);rawUsed=used=0;

 serbridgeConnData a={0},b={0};flashConfig.baud_rate=115200;flashConfig.data_bits=EIGHT_BITS;flashConfig.parity=NONE_BITS;flashConfig.stop_bits=ONE_STOP_BIT;
 unsigned char negotiation[]={IAC,DO,ComPortOpt};telnetUnwrap(&a,negotiation,3);assert(used==3&&response[1]==WILL&&!rawUsed);used=0;
 negotiation[1]=WONT;telnetUnwrap(&a,negotiation,3);assert(!used&&!rawUsed&&a.telnet_state==TN_normal);
 setting(&a,3,0);expect(3,1);assert(!writes&&!formats);
 setting(&a,3,2);expect(3,2);assert(flashConfig.parity==ODD_BITS&&formats==1&&(registerBits&3)==3);
 setting(&a,3,3);expect(3,3);assert(flashConfig.parity==EVEN_BITS&&(registerBits&3)==2);
 setting(&a,3,5);expect(3,3);assert(writes==2);
 failSave=true;setting(&a,3,1);expect(3,3);assert(flashConfig.parity==EVEN_BITS&&formats==2);failSave=false;
 setting(&a,2,5);expect(2,5);assert(flashConfig.data_bits==FIVE_BITS);
 setting(&a,4,3);expect(4,3);assert(flashConfig.stop_bits==ONE_HALF_STOP_BIT&&((registerBits>>4)&3)==2);
 setting(&a,4,2);expect(4,2);assert(flashConfig.stop_bits==TWO_STOP_BIT);
 unsigned char p[16],q[16],rateA[]={0,1,0xc2,0},rateB[]={0,0,0xe1,0};unsigned n=packet(p,1,rateA,4),m=packet(q,1,rateB,4);
 telnetUnwrap(&a,p,6);telnetUnwrap(&b,q,m);assert(lastBaud==57600);telnetUnwrap(&a,p+6,n-6);assert(lastBaud==115200&&baudCalls==2);
 unsigned char escaped[]={0,0,255,255};n=packet(p,1,escaped,4);used=0;for(unsigned i=0;i<n;i++)telnetUnwrap(&a,p+i,1);
 assert(lastBaud==65535&&used==12&&response[3]==101&&response[6]==255&&response[7]==255&&response[8]==255&&response[9]==255);
 unsigned char zero[4]={0};n=packet(p,1,zero,4);used=0;telnetUnwrap(&a,p,n);assert(response[3]==101&&used==12);
 failSave=true;n=packet(p,1,rateA,4);telnetUnwrap(&a,p,n);assert(lastBaud==65535&&flashConfig.baud_rate==65535);failSave=false;
 unsigned char literal[]={255,255,'X'};telnetUnwrap(&a,literal,sizeof literal);assert(rawUsed==2&&raw[0]==255&&raw[1]=='X');
 assert(a.telnet_state==TN_normal&&b.telnet_state==TN_normal);
 // Per-client transitions are idempotent, and teardown releases BREAK/ISP state.
 setting(&a,SetControl,RTS_ON);setting(&a,SetControl,RTS_ON);assert(in_mcu_flashing==1&&a.programming_owned);
 flashConfig.swap_uart=true;setting(&a,SetControl,BRK_ON);assert(lastPin==15&&lastLevel==0&&breakOwner==&a);
 setting(&b,SetControl,BRK_OFF);assert(breakOwner==&a);
 struct espconn closing={.reverse=&a};a.conn=&closing;serbridgeDisconCb(&closing);
 assert(!in_mcu_flashing&&!a.programming_owned&&!breakOwner&&!tn_break&&lastPin==15&&lastLevel==1);
 flashConfig.swap_uart=false;setting(&b,SetControl,BRK_ON);assert(lastPin==1&&breakOwner==&b);breakState(&b,false);
 // A negotiation reply can enter recovery halfway through a TCP input chunk.
 probe=(serbridgeConnData){0};used=rawUsed=0;pauseResponse=true;unsigned char pending[]={IAC,DO,BinaryOpt,'X'};
 assert(telnetUnwrap(&probe,pending,sizeof pending)==3&&probe.tx_recovering&&!rawUsed);
 pauseResponse=false;probe.tx_recovering=false;assert(telnetUnwrap(&probe,pending+3,1)==1&&rawUsed==1&&raw[0]=='X');
 probe=(serbridgeConnData){0};used=rawUsed=0;failResponse=true;
 assert(telnetUnwrap(&probe,pending,sizeof pending)==3&&probe.tx_failed&&!rawUsed);failResponse=false;
 puts("PASS actual RFC2217 UART settings: server101–104 replies, escaped baud bytes, fragmented/interleaved clients, live Odd/stop/data changes, queries/invalid values and persistence rollback");
}
'''
(OUT/'test.c').write_text(source)
subprocess.run(['cc','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-funsigned-char','-Wno-ignored-qualifiers','-Wno-misleading-indentation','-fsanitize=address,undefined',
 *['-I'+str(ROOT/p) for p in ['tests/support/api-stubs','src/core','src/serial','include']],str(OUT/'test.c'),'-o',str(OUT/'test')],check=True)
subprocess.run([str(OUT/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})

# Exercise the production parser against the installed stock client in a real PTY.
# No device connection or serial traffic: a loopback socket feeds the C parser.
if shutil.which('telnet'):
    # A negative test port requests normal Telnet negotiation on an ephemeral port.
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen()
    master,slave=pty.openpty();captured=bytearray();negotiation=[];errors=[];stop=threading.Event()
    worker=subprocess.Popen([str(OUT/'test'),'--bridge'],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
    def exact(n):
        data=bytearray()
        while len(data)<n:
            chunk=worker.stdout.read(n-len(data))
            if not chunk:raise RuntimeError('parser worker exited')
            data.extend(chunk)
        return bytes(data)
    def server():
        try:
            conn,_=listener.accept()
            with conn:
                conn.settimeout(.1)
                while not stop.is_set():
                    try:data=conn.recv(64)
                    except socket.timeout:continue
                    if not data:break
                    worker.stdin.write(struct.pack('<H',len(data))+data);worker.stdin.flush()
                    a,b=struct.unpack('<HH',exact(4));reply=exact(a);raw=exact(b)
                    negotiation.append((data.hex(),reply.hex()))
                    if reply:conn.sendall(reply)
                    captured.extend(raw)
        except Exception as e:errors.append(e)
    thread=threading.Thread(target=server,daemon=True);thread.start()
    client=subprocess.Popen(['telnet','-8','--','127.0.0.1','-'+str(listener.getsockname()[1])],stdin=slave,stdout=slave,stderr=slave)
    try:
        end=time.monotonic()+4
        while time.monotonic()<end:
            flags=termios.tcgetattr(slave)[3]
            if not flags&(termios.ICANON|termios.ECHO):break
            time.sleep(.02)
        else:raise AssertionError(('stock telnet did not enter raw/no-echo mode',negotiation,errors,os.read(master,4096)))
        output=bytearray()
        while select.select([master],[],[],.05)[0]:output.extend(os.read(master,4096))
        # A keystroke and Konsole-style replies must reach the UART immediately,
        # without Enter, local echo, caret expansion or conversion of binary data.
        payload=b'x\x1b[?62;1;4c\x1b]11;rgb:2323/2626/2727\x1b\\\r\x00\xff'
        os.write(master,payload);end=time.monotonic()+3
        while len(captured)<len(payload) and time.monotonic()<end:time.sleep(.01)
        assert bytes(captured)==payload,(bytes(captured),payload)
        assert not select.select([master],[],[],.15)[0],'local echo leaked'
        print('PASS stock telnet -8: raw/no-echo PTY, immediate keys/terminal replies, binary CR/NUL/IAC and bounded negotiation')
    finally:
        stop.set();client.terminate();client.wait(timeout=3);thread.join(timeout=2)
        worker.stdin.close();worker.wait(timeout=3);listener.close();os.close(master);os.close(slave)
    assert not errors,errors
else:
    print('SKIP stock-client integration: telnet is not installed')
