#!/usr/bin/env python3
"""Execute pinned SDK write-admission and ACK-credit code with both capacities."""

import _paths
from source_text import SourceText
from pathlib import Path
import subprocess, argparse

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdk", type=Path, required=True)
args = parser.parse_args()
b = root / ".build-tests/sdk-credit"
b.mkdir(parents=True, exist_ok=True)
sdk = args.sdk / "third_party/lwip/core"
s = SourceText((sdk / "tcp_out.c").read_text(encoding="latin1"))
start = s.index("static err_t ICACHE_FLASH_ATTR\ntcp_write_checks(")
i = s.index("{", start) + 1
depth = 1
while depth:
    depth += (s[i] == "{") - (s[i] == "}")
    i += 1
check = s[start:i]
s = SourceText((sdk / "tcp_in.c").read_text(encoding="latin1"))
start = s.index("      pcb->acked = (u16_t)(ackno - pcb->lastack);")
end = s.index("pcb->snd_buf += pcb->acked;", start) + len("pcb->snd_buf += pcb->acked;")
ack = s[start:end]
prefix = r"""
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef int err_t;typedef uint16_t u16_t;
#define ICACHE_FLASH_ATTR
#define LWIP_DEBUGF(...) do{}while(0)
#define LWIP_ASSERT(s,e) assert(e)
#define TCP_STATS_INC(...) do{}while(0)
#define TCP_SND_QUEUELEN 8
#define TCP_SNDQUEUELEN_OVERFLOW 65532
#define ESTABLISHED 4
#define CLOSE_WAIT 7
#define SYN_SENT 2
#define SYN_RCVD 3
#define ERR_OK 0
#define ERR_CONN -11
#define ERR_MEM -1
#define TF_NAGLEMEMERR 128
struct tcp_pcb {unsigned state,flags,snd_buf,snd_queuelen,lastack,acked;void *unacked,*unsent;};
"""
tests = r"""
}
int main(void) {
 for(unsigned cap=1072;cap<=2144;cap+=1072) {
  struct tcp_pcb p={.state=4,.snd_buf=cap,.lastack=1000};
  unsigned count=0;
  while(tcp_write_checks(&p,536)==ERR_OK) {
   p.snd_buf-=536;p.snd_queuelen++;p.unacked=&p;count++;
  }
  assert(count==cap/536 && p.snd_buf==0 && p.snd_queuelen==count);
  ack_credit(&p,1000+cap);assert(p.snd_buf==cap);
  printf("PASS actual SDK admission/ACK code: %u segments admitted, byte credit restored to %u\n",count,cap);
 }
 struct tcp_pcb p={.state=4,.snd_buf=2144,.snd_queuelen=8,.unacked=&p};
 assert(tcp_write_checks(&p,536)==ERR_MEM);
 p.snd_queuelen=0;p.unacked=NULL;p.state=0;
 assert(tcp_write_checks(&p,536)==ERR_CONN);
 puts("PASS original eight-pbuf and closed-connection guards retained");
}
"""
(b / "credit-native.c").write_text(
    prefix
    + check
    + "\nstatic void ack_credit(struct tcp_pcb *pcb,unsigned ackno) {\n"
    + ack
    + tests,
    encoding="latin1",
)
subprocess.run(
    [
        "cc",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        str(b / "credit-native.c"),
        "-o",
        str(b / "credit-native"),
    ],
    check=True,
)
r = subprocess.run([str(b / "credit-native")], capture_output=True, text=True)
(b / "credit-native.log").write_text(r.stdout + r.stderr)
print(r.stdout, end="")
r.check_returncode()
