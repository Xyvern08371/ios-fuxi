#!/usr/bin/env python3
"""Per-call ABI timing aggregation, its periodic log line, and wait-slice sleeps.

Compiles the actual stat helpers from fuxi_abi_wire.h and FuxiAdrenoUserClient.cpp
with minimal kernel stubs. No hardware or Apple SDK; device timing is not observed.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

MODEL = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "fuxi_abi_wire.h"
struct IOLock {int held=0;};
static unsigned locks;
static void IOLockLock(IOLock *l) {assert(!l->held);l->held=1;locks++;}
static void IOLockUnlock(IOLock *l) {assert(l->held);l->held=0;}
static uint64_t now,tickScale=1;
static uint64_t mach_absolute_time() {return now;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n) {*n=t*tickScale;}
static unsigned delays;static uint64_t delayUs;
static void delay(unsigned n) {delays++;delayUs+=n;now+=n*1000;}
static unsigned sleeps;
static void IOSleep(unsigned ms) {assert(ms==1);sleeps++;now+=1000000;}
static char logs[8][400];static unsigned logCount;
static void IOLog(const char *fmt,...) {va_list ap;va_start(ap,fmt);assert(logCount<8);vsnprintf(logs[logCount++],400,fmt,ap);va_end(ap);}
%s
%s
%s
int main() {
 // Aggregator: counts, totals, running max, and out-of-range slots ignored.
 fg_abi_stat raw[FG_ABI_STATS]={};
 fg_abi_stat_add(raw,5,10);fg_abi_stat_add(raw,5,30);fg_abi_stat_add(raw,5,20);
 assert(raw[5].count==3 && raw[5].total==60 && raw[5].max==30);
 fg_abi_stat_add(raw,FG_ABI_STATS,1);fg_abi_stat_add(raw,~0u,1);
 for(unsigned i=0;i<FG_ABI_STATS;i++)assert(i==5 || !raw[i].count);
 static_assert(FG_ABI_STAT_LOCK==0 && FG_ABI_STAT_WALK==17 && FG_ABI_STAT_SLEEP==19 && FG_ABI_STATS==20,"slot layout");
 // No lock yet (allocation failure) or no calls: silent, no crash.
 abiStat(5,10);fuxi_gpu_abi_stats_log();assert(!logCount && !locks);
 static IOLock storage;statLock=&storage;
 fuxi_gpu_abi_stats_log();assert(!logCount);
 // One window: selector 5 (submit) 2 calls, avg 501.0 us, max 1000 us; lock wait 1 call 0.4 us.
 tickScale=1;abiStat(5,1500);abiStat(5,1000500);abiStat(0,400);
 fuxi_gpu_abi_stats_log();
 assert(logCount==1);
 assert(!strcmp(logs[0],"FUXI GPU ABI: calls n/avg/max us lock 1/0.4/0 submit 2/501.0/1000\n"));
 fuxi_gpu_abi_stats_log();assert(logCount==1); // window restarted
 // Tick conversion applies, and every slot fits in bounded lines naming each exactly once.
 tickScale=42;logCount=0;
 for(unsigned i=0;i<FG_ABI_STATS;i++)abiStat(i,1000000+i);
 fuxi_gpu_abi_stats_log();
 assert(logCount>=2 && logCount<=3);
 static const char *names[]={"lock","query","alloc","map","free","submit","wait","timestamp","status",
  "set-metadata","get-metadata","create-queue","close-queue","cpu-token","cpu-unmap","device-info","import",
  "walk","zero","sleep"};
 for(unsigned i=0;i<FG_ABI_STATS;i++) {
  char key[64];snprintf(key,sizeof(key)," %%s 1/",names[i]);unsigned seen=0;
  for(unsigned l=0;l<logCount;l++){assert(strlen(logs[l])<350);for(const char *p=logs[l];(p=strstr(p,key));p++)seen++;}
  assert(seen==1);
 }
 assert(strstr(logs[0],"lock 1/42000.0/42000"));
 // Sleeps: fuxigpuwaitus slice (delay) or the 1 ms IOSleep when 0, each recorded as a sleep.
 tickScale=1;now=1000;logCount=0;delays=0;delayUs=0;
 abiPause();abiPause();assert(delays==2 && delayUs==200 && sleeps==0);
 waitPollUs=0;abiPause();assert(sleeps==1 && delays==2);waitPollUs=100;
 tickScale=1;logCount=0;fuxi_gpu_abi_stats_log();
 assert(logCount==1 && strstr(logs[0]," sleep 3/"));
 puts("PASS: ABI stat aggregation/window reset/log format, sleep slices and sleep accounting");
}
'''


def main():
    src = (ROOT/'xnu/fuxi-adreno/FuxiAdrenoUserClient.cpp').read_text()
    stats = src[src.index('static IOLock *statLock;'):src.index('static bool connected;')]
    step = src[src.index('static const char *abiStep('):src.index('class FuxiAdrenoUserClient :')]
    log = src[src.index('extern "C" void fuxi_gpu_abi_stats_log(void)'):src.index('extern "C" void fuxi_gpu_abi_publish(void)')]
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)/'perf.cpp'
        path.write_text(MODEL % (stats, step, log))
        exe = path.with_suffix('')
        subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-format',
                        '-fsanitize=address,undefined', '-I'+str(ROOT/'xnu/fuxi-adreno'), str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
