#!/usr/bin/env python3
"""Actual failed-association radio recovery, including asynchronous cancellation."""

import _paths
from source_text import SourceText
from pathlib import Path
import subprocess, os

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".build-tests/wifi-recovery"
BUILD.mkdir(parents=True, exist_ok=True)
cgi = SourceText((ROOT / "src/core/cgiwifi.c").read_text())
recovery = cgi[
    cgi.index("static void ICACHE_FLASH_ATTR resetTimerCb(") : cgi.index(
        "// Reassociate timer to delay change"
    )
]
timeout_event = cgi[
    cgi.index("  case EVENT_STAMODE_DHCP_TIMEOUT:")
    + len("  case EVENT_STAMODE_DHCP_TIMEOUT:") : cgi.index("  case EVENT_STAMODE_AUTHMODE_CHANGE:")
].replace("    break;", "")
source = (
    r"""
#include <assert.h>
#include "wifi_recovery.c"
#define DBG(...) ((void)0)
#define RESET_TIMEOUT 15000
static ETSTimer resetTimer;
static bool networkPaused,recoveryScheduled;
static struct softap_config apconf;
static unsigned resetArms;
void log_uart(bool v){(void)v;}
bool wifi_set_opmode(uint8_t);
bool wifi_softap_set_config(struct softap_config *);
static uint8_t mode=3,state=STATION_NO_AP_FOUND,channel=13;
static bool policy=true,failChannel;
static unsigned disconnects,changes;
uint8_t wifi_get_opmode(void){return mode;}
bool wifi_set_opmode(uint8_t v){mode=v;return true;}
bool wifi_softap_set_config(struct softap_config *v){(void)v;return true;}
uint8_t wifi_station_get_connect_status(void){return state;}
bool wifi_station_set_reconnect_policy(bool v){policy=v;return true;}
bool wifi_station_disconnect(void){disconnects++;state=STATION_IDLE;return true;}
bool wifi_softap_get_config(struct softap_config *v){v->channel=1;return true;}
bool wifi_set_channel(uint8_t v){changes++;if(failChannel)return false;channel=v;return true;}
void os_timer_disarm(ETSTimer *v){v->armed=false;}
void os_timer_setfn(ETSTimer *v,void (*cb)(void *),void *arg){v->callback=cb;v->arg=arg;}
void os_timer_arm(ETSTimer *v,unsigned ms,bool repeat){assert(!repeat);if(v==&resetTimer)resetArms++;v->ms=ms;v->armed=true;}
static void fire(void){assert(channelTimer.armed);channelTimer.armed=false;channelTimer.callback(channelTimer.arg);}
"""
    + recovery
    + "\nstatic void timeoutEvent(void){"
    + timeout_event
    + "}\n"
    + r"""
int main(void){
 assert(wifiRecoveryBegin()&&!policy&&!wifiRecoveryStatus());
 assert(!wifiRecoveryPoll(STATION_CONNECTING)&&!disconnects);
 wifiRecoveryObserve(REASON_NO_AP_FOUND);assert(wifiRecoveryPoll(STATION_CONNECTING));fire();
 assert(wifiRecoveryStatus()==STATION_NO_AP_FOUND);wifiRecoveryBegin();disconnects=changes=0;channel=13;state=STATION_NO_AP_FOUND;
 assert(wifiRecoveryPoll(STATION_NO_AP_FOUND)&&disconnects==1&&state==STATION_IDLE&&wifiRecoveryStatus()==STATION_NO_AP_FOUND);
 assert(channel==13&&channelTimer.armed&&channelTimer.ms==500);fire();assert(channel==1&&changes==1&&!channelTimer.armed);
 assert(wifiRecoveryPoll(STATION_IDLE)&&disconnects==1);
 wifiRecoveryConnected();assert(policy&&!wifiRecoveryStatus());
 state=STATION_WRONG_PASSWORD;wifiRecoveryObserve(REASON_HANDSHAKE_TIMEOUT);assert(wifiRecoveryPoll(STATION_IDLE));assert(wifiRecoveryStatus()==STATION_WRONG_PASSWORD);wifiRecoveryBegin();
 assert(wifiRecoveryBegin());assert(wifiRecoveryPoll(STATION_WRONG_PASSWORD));state=STATION_GOT_IP;unsigned before=changes;fire();assert(changes==before);
 wifiRecoveryConnected();assert(!wifiRecoveryStatus()&&!channelTimer.armed);
 state=STATION_CONNECT_FAIL;assert(wifiRecoveryBegin());assert(wifiRecoveryPoll(state));failChannel=true;
 fire();assert(channelTimer.armed);fire();assert(channelTimer.armed);fire();assert(!channelTimer.armed&&attempts==3);
 failChannel=false;state=STATION_CONNECTING;mode=3;wifiRecoveryBegin();
 resetTimer.armed=false;recoveryScheduled=false;timeoutEvent();
 assert(wifiRecoveryDhcpFailed()&&resetTimer.armed&&recoveryScheduled);
 wifiRecoveryObserve(0);assert(wifiRecoveryDhcpFailed());
 resetTimerCb(NULL);assert(wifiRecoveryStatus()==WIFI_RECOVERY_DHCP_TIMEOUT);fire();
 wifiRecoveryConnected();assert(!wifiRecoveryDhcpFailed());
 wifiRecoveryDhcpTimeout();wifiRecoveryAssociated();assert(!wifiRecoveryDhcpFailed());
 wifiRecoveryDhcpTimeout();wifiRecoveryBegin();assert(!wifiRecoveryDhcpFailed());
 resetTimer.armed=false;recoveryScheduled=false;state=STATION_IDLE;
 networkPaused=true;unsigned arms=resetArms;scheduleRecovery();assert(resetArms==arms);
 networkPaused=false;scheduleRecovery();assert(resetArms==arms+1&&recoveryScheduled&&resetTimer.ms==15000);
 scheduleRecovery();assert(resetArms==arms+1); /* repeated disconnect events must not postpone recovery */
 state=STATION_NO_AP_FOUND;resetTimer.armed=false;resetTimerCb(NULL);assert(wifiRecoveryStatus()==STATION_NO_AP_FOUND&&!recoveryScheduled);
 scheduleRecovery();assert(resetArms==arms+1);fire();assert(channel==1);
 wifiRecoveryBegin();state=STATION_GOT_IP;mode=3;resetTimerCb(NULL);assert(mode==1&&recoveryScheduled);
 resetTimer.armed=false;resetTimerCb(NULL);assert(!recoveryScheduled&&!resetTimer.armed);
 state=STATION_NO_AP_FOUND;scheduleRecovery();resetTimerCb(NULL);assert(mode==3&&wifiRecoveryStatus()==STATION_NO_AP_FOUND);fire();
 puts("PASS actual Wi-Fi recovery: stop terminal-failure retries, retain failure telemetry, defer channel restore, bounded failures, cancel on IP and explicit retry");
}
"""
)
(BUILD / "test.c").write_text(source)
subprocess.run(
    [
        "cc",
        "-std=c11",
        "-g",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-parameter",
        "-DCHANGE_TO_STA",
        "-fsanitize=address,undefined",
        "-I" + str(ROOT / "tests/support/api-stubs"),
        "-I" + str(ROOT / "src/core"),
        str(BUILD / "test.c"),
        "-o",
        str(BUILD / "test"),
    ],
    check=True,
)
subprocess.run(
    [str(BUILD / "test")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"}
)
