#!/usr/bin/env python3
"""Offline production-code scanout models; no device or reference execution.

Checks display ownership, the CPU pixel oracle, real surface-cache reservation
and framebuffer integration. MMIO/clock/cache/DMA completion remain models.
"""
import argparse
import shlex
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define FG_GUARD
typedef int boolean_t;
#define FALSE 0
static int interruptsOn=1;
static boolean_t ml_set_interrupts_enabled(boolean_t value) {int old=interruptsOn;interruptsOn=value;return old;}
static unsigned fg_scanout_requested, fg_scanout_panel120_requested, fg_busy, fg_client_busy;
static unsigned ready=1,abiLive=1,abiResult,observations,reads,writes,starts,failAddress;
static uint64_t ticks=1000;
static uint64_t mach_absolute_time(void) {return ticks;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) {*ns=t*1000;}
static uint32_t ctl[1024],intf[1024],dsi[1024];
static struct {unsigned pa,value;} trace[1024];
static unsigned traceCount,dsiReads,dmaStarts,pendingDma,noDone,busyAfterDone,staleDone,frozenClock,failWriteAddress;
static unsigned errorOnDone,tearReadbackMismatch;
static volatile uint32_t *fuxi_ctl0=ctl,*fuxi_intf1=intf;
static int fuxi_gpu_ready(void) {return ready;}
static unsigned modelDrain,modelTe,modelPanel120;
static unsigned fg_scanout_owned_for_model(void);
static int PE_parse_boot_argn(const char *name,void *value,int size) {
 unsigned v=!strcmp(name,"fuxiscante")?modelTe:!strcmp(name,"fuxiscandrain")?modelDrain:
     !strcmp(name,"fuxipanel120")?modelPanel120:0;
 if(!v || size<4)return 0;*(unsigned *)value=v;return 1;
}
static int fg_abi_live_for_scanout(void) {return abiLive;}
static int fg_abi_observe(void) {observations++;if(!abiResult)fg_client_busy=0;return abiResult;}
static uint64_t model_counter(const char *r) {return !strcmp(r,"cntfrq_el0")?1000000:ticks;}
#define rsr(r) model_counter(r)
static void delay(unsigned us) {if(!frozenClock)ticks+=us;}
static unsigned fg_access(uint64_t pa,uint64_t *value,unsigned wr,unsigned wide) {
 assert(!wide);
 if(pa==failAddress || (wr && pa==failWriteAddress))return 1;
 uint32_t *reg;
 if(pa>=0xae16000 && pa<0xae17000)reg=&ctl[(pa-0xae16000)/4];
 else if(pa>=0xae94000 && pa<0xae95000)reg=&dsi[(pa-0xae94000)/4];
 else {assert(pa>=0xae36000 && pa<0xae37000);reg=&intf[(pa-0xae36000)/4];}
 if(wr){
  writes++;assert(traceCount<1024);trace[traceCount].pa=pa;trace[traceCount++].value=*value;
  if(pa>=0xae94000 && pa<0xae95000) {
   assert(interruptsOn);
   assert(fg_scanout_owned_for_model());
   assert(!(intf[0x2b4/4]&0x80000000u) && !ctl[0x1c/4] && !fg_busy && !starts);
   assert(!intf[0x2b0/4] || intf[0x2b0/4]>=2400);
  }
  if(pa==0xae36288 || pa==0xae3628c)assert(!interruptsOn);
  if(pa==0xae94110) { // Odd bits enable interrupts; even bits are W1C status.
   uint32_t clear=staleDone?((uint32_t)*value&~1u):(uint32_t)*value;
   *reg=(*reg&0x55555555u&~clear)|((uint32_t)*value&0xaaaaaaaau);
  } else *reg=(uint32_t)*value;
  if(pa==0xae94090){assert(*value==1 && dsi[0x15c/4]==0x30006 && dsi[0x04c/4]==52);
   dmaStarts++;pendingDma=2;dsi[0x008/4]=3;}
  if(pa==0xae1601c){assert(!fg_busy && !(intf[0x2b4/4]&0x80000000u));starts++;}
 } else {
  reads++;
  if(pa>=0xae94000 && pa<0xae95000)dsiReads++;
  if(pa==0xae94110 && pendingDma && !noDone && !--pendingDma){
   dsi[0x110/4]|=1;dsi[0x008/4]=busyAfterDone?4:0;
   if(errorOnDone)dsi[0x110/4]|=1u<<24;
  }
  *value=*reg;
  if(tearReadbackMismatch && pa==0xae36288 && dmaStarts)*value^=1;
 }
 return 0;
}
'''
TEST = r'''
static unsigned fg_scanout_owned_for_model(void) {return fg_scanout_owned;}
static void reset(void) {
 memset(ctl,0,sizeof(ctl));memset(intf,0,sizeof(intf));memset(dsi,0,sizeof(dsi));intf[0x2b4/4]=0x80000001u;
 fg_scanout_requested=fg_scanout_owned=fg_scanout_failed=fg_scanout_writable=0;
 fg_scanout_reserved=fg_busy=fg_client_busy=0;
 fg_scanout_checked=fg_scanout_checks=0;
 fg_scanout_te_mode=fg_scanout_wait_te=0;fg_scanout_drain_us=FG_SCAN_DRAIN_US;modelTe=modelDrain=0;
 fg_scanout_panel120_requested=fg_scanout_panel120_attempted=fg_scanout_panel120_active=0;
 fg_scanout_period_us=16667;fg_scanout_explicit_drain=modelPanel120=0;
 traceCount=dsiReads=dmaStarts=pendingDma=noDone=busyAfterDone=staleDone=frozenClock=failWriteAddress=0;
 interruptsOn=1;
 errorOnDone=tearReadbackMismatch=0;
 fg_scanout_last_start=0;fg_scanout_starts=0;memset(fg_scanout_iv,0,sizeof(fg_scanout_iv));
 ticks=1000;ready=abiLive=1;abiResult=observations=reads=writes=starts=failAddress=0;
}
static void panel120_setup(void) {
 modelPanel120=1;
 intf[0x284/4]=1;intf[0x288/4]=0x180085;intf[0x28c/4]=0xfff0;intf[0x294/4]=2400;
 intf[0x2b0/4]=2400;
 dsi[0x004/4]=0x1f5;dsi[0x03c/4]=0xc60000ab;dsi[0x084/4]=0x80011066;
 dsi[0x110/4]=0x100203; // stale DMA done, BTA done, DMA+MDP interrupt enables
 dsi[0x04c/4]=0x54;
}
static void panel120_drain(uint8_t *panel) {
 fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);
 assert(!fuxi_gpu_scanout_acquire() && !dsiReads && !dmaStarts);
 ticks+=20000;
}
static void panel120_tests(uint8_t *panel) {
 // Exact boot-arg admission; no ownership means no DSI read, write, or command.
 reset();panel120_setup();modelPanel120=0;panel120_drain(panel);
 assert(fuxi_gpu_scanout_acquire()==1 && !fg_scanout_panel120_requested && !dsiReads && !dmaStarts);
 reset();panel120_setup();modelPanel120=2;panel120_drain(panel);
 assert(fuxi_gpu_scanout_acquire()==1 && !fg_scanout_panel120_requested && !dsiReads && !dmaStarts);
 reset();panel120_setup();fuxi_gpu_scanout_configure((uintptr_t)panel,0,0);
 assert(fuxi_gpu_scanout_acquire()==1 && !reads && !writes);
 reset();panel120_setup();panel120_drain(panel);
 ctl[0x1c/4]=1;assert(!fuxi_gpu_scanout_acquire() && !dsiReads);
 ctl[0x1c/4]=0;intf[0x2b0/4]=2399;assert(!fuxi_gpu_scanout_acquire() && !dsiReads);
 intf[0x2b0/4]=2400;fg_busy=1;assert(!fuxi_gpu_scanout_acquire() && !dsiReads);fg_busy=0;
 assert(!fuxi_gpu_scanout_acquire() && fg_scanout_panel120_active && dmaStarts==1 && !starts);
 assert(fg_scanout_period_us==8333 && fg_scanout_drain_us==10000 && !fg_scanout_writable);
 // Literal embedded-packet oracle: exact DCS bytes/types, only final LAST,
 // ff payload padding, odd-DWORD zero padding outside the 52-byte length.
 static const uint32_t expected[]={
  0x40390003,0xff5a5af0,0x00150060,0x40390004,0xbd3000b0,0x001504bd,
  0x40390004,0xf21600b0,0x40390003,0xff7001f2,0x00150ff7,0xc0390003,0xffa5a5f0,0};
 unsigned fifo=0,triggerIndex=0,firstTear=0,lastDone=0;
 for(unsigned i=0;i<traceCount;i++){
  if(trace[i].pa==0xae9417c){assert(fifo<14 && trace[i].value==expected[fifo]);fifo++;}
  if(trace[i].pa==0xae94090)triggerIndex=i;
  if(trace[i].pa==0xae94110 && (trace[i].value&1))lastDone=i;
  if(trace[i].pa==0xae36288 && !firstTear)firstTear=i;
  assert(trace[i].pa!=0xae94094); // Never generate an MDP transfer to send DCS.
 }
 assert(fifo==14 && triggerIndex>14 && firstTear>lastDone && lastDone>triggerIndex);
 // Check ordering around FIFO setup/kickoff/cleanup, not just final register contents.
 unsigned t=1; // First write disabled AR.
 assert(trace[t].pa==0xae94084 && trace[t++].value==0x80000064);
 assert(trace[t].pa==0xae94110 && trace[t++].value==0x201);
 assert(trace[t].pa==0xae941ec && trace[t++].value==1);
 assert(trace[t].pa==0xae941ec && trace[t++].value==0);
 assert(trace[t].pa==0xae9415c && trace[t++].value==0x30006);
 t+=14;
 assert(trace[t].pa==0xae9403c && trace[t++].value==0x120000ab);
 assert(trace[t].pa==0xae9404c && trace[t++].value==52);
 assert(trace[t].pa==0xae94090 && trace[t++].value==1);
 assert(trace[t].pa==0xae94110 && trace[t++].value==0x201);
 assert(dsi[0x03c/4]==0xc60000ab && dsi[0x084/4]==0x80011066 && !dsi[0x15c/4] && dsi[0x04c/4]==0x54);
 assert(dsi[0x110/4]==0x100202); // Preserve unrelated W1C status, restore interrupt masks.
 // Tear-check counter enable is VSYNC bit19, not TEAR_CHECK_EN at +284.
 assert(trace[firstTear].value==0x100085);
 assert(trace[firstTear+1].pa==0xae36288 && trace[firstTear+1].value==0x100042);
 assert(trace[firstTear+2].pa==0xae3628c && trace[firstTear+2].value==0xfff0);
 assert(trace[firstTear+3].pa==0xae36288 && trace[firstTear+3].value==0x180042);
 assert(intf[0x288/4]==0x180042 && intf[0x284/4]==1 && intf[0x294/4]==2400);
 ticks=fg_scanout_after-1;assert(!fuxi_gpu_scanout_acquire());ticks++;
 assert(fuxi_gpu_scanout_acquire()==1 && fuxi_gpu_scanout_publish()==1 && starts==1);
 ctl[0x1c/4]=0;ticks+=10000;assert(fuxi_gpu_scanout_acquire()==1 && dmaStarts==1); // once
 // Gaps now count 8.333 ms periods, including 16.667ms as two periods.
 ticks=fg_scanout_last_start+8333;assert(fuxi_gpu_scanout_publish()==1);
 ctl[0x1c/4]=0;ticks+=16667;assert(fuxi_gpu_scanout_acquire()==1 && fuxi_gpu_scanout_publish()==1);
 assert(fg_scanout_iv[0]==1 && fg_scanout_iv[1]==1 && !fg_scanout_iv[2] && !fg_scanout_iv[3]);
 // Preflight busy: bounded abort, no FIFO/tear writes, no retry, 60 Hz retained.
 reset();panel120_setup();panel120_drain(panel);dsi[0x008/4]=4;
 unsigned before=writes;uint64_t began=ticks;
 assert(fuxi_gpu_scanout_acquire()==-1 && ticks-began==20000 && writes==before && !dmaStarts);
 assert(!fg_scanout_panel120_active && fg_scanout_period_us==16667 && intf[0x288/4]==0x180085);
 before=dsiReads;assert(fuxi_gpu_scanout_acquire()==-1 && dsiReads==before);
 // Invalid inherited state refuses the switch without trying panel initialization.
 reset();panel120_setup();panel120_drain(panel);intf[0x288/4]=0x100085;
 before=writes;assert(fuxi_gpu_scanout_acquire()==1 && writes==before && !dmaStarts);
 // Stale completion may not satisfy a new request. No command after clear timeout.
 reset();panel120_setup();panel120_drain(panel);staleDone=1;
 assert(fuxi_gpu_scanout_acquire()==-1 && !dmaStarts && !fg_scanout_panel120_active && !starts);
 assert(intf[0x288/4]==0x180085);
 // Lost done or stuck busy after done: hold, no cleanup/rollback/tear/START,
 // no second attempt. No assumption that a partially sent command can be undone.
 for(unsigned fault=0;fault<3;fault++){
  reset();panel120_setup();panel120_drain(panel);
  noDone=fault!=1;busyAfterDone=fault==1;frozenClock=fault==2;
  began=ticks;assert(fuxi_gpu_scanout_acquire()==-1 && dmaStarts==1 && !fg_scanout_panel120_active && !starts);
  assert(fg_scanout_period_us==16667 && fg_scanout_drain_us==20000 && intf[0x288/4]==0x180085);
  assert(trace[traceCount-1].pa==0xae94090 && dsiReads<2100);
  assert(frozenClock || ticks-began>=20000);
  before=writes;assert(fuxi_gpu_scanout_acquire()==-1 && fuxi_gpu_scanout_publish()==-1 && writes==before);
 }
 // MMIO failure during FIFO loading and tear programming also hold publication.
 for(unsigned fault=0;fault<3;fault++){
  reset();panel120_setup();panel120_drain(panel);failWriteAddress=fault==2?0xae3628c:fault?0xae36288:0xae9417c;
  assert(fuxi_gpu_scanout_acquire()==-1 && !fg_scanout_panel120_active && !starts && interruptsOn);
 }
 // Transfer error and tear-register readback mismatch cannot activate 120Hz.
 for(unsigned fault=0;fault<2;fault++){
  reset();panel120_setup();panel120_drain(panel);errorOnDone=fault==0;tearReadbackMismatch=fault==1;
  assert(fuxi_gpu_scanout_acquire()==-1 && dmaStarts==1 && !fg_scanout_panel120_active && !starts && interruptsOn);
 }
 // Explicit drain is in microseconds and survives a successful mode change.
 reset();panel120_setup();modelDrain=4000;panel120_drain(panel);
 assert(!fuxi_gpu_scanout_acquire() && fg_scanout_panel120_active && fg_scanout_drain_us==4000);
 // TE gate uses the same line margin, twice as fast counter, 20ms stall budget,
 // and a 10ms fallback. The inherited 60 Hz transfer was drained first.
 reset();panel120_setup();modelTe=1;panel120_drain(panel);
 intf[0x298/4]=(1u<<16)|2500;
 assert(!fuxi_gpu_scanout_acquire() && fg_scanout_panel120_active && !fg_scanout_drain_us);
 intf[0x298/4]=(2u<<16)|2410;assert(!fuxi_gpu_scanout_acquire());
 intf[0x298/4]=(2u<<16)|2464;assert(fuxi_gpu_scanout_acquire()==1 && fuxi_gpu_scanout_publish()==1);
 ctl[0x1c/4]=0;ticks=fg_scanout_started+19999;
 assert(!fuxi_gpu_scanout_acquire() && fg_scanout_te_mode);
 ticks++;assert(!fuxi_gpu_scanout_acquire() && !fg_scanout_te_mode && fg_scanout_drain_us==10000);
 assert(fuxi_gpu_scanout_acquire()==1);
 puts("PASS: panel120 opt-in, exact HS DCS/FIFO protocol, safe boundary, bounded abort/hold, tear math/order, 120Hz floor/TE/gaps");
}
int main(void) {
 reset();assert(fuxi_gpu_scanout_acquire()==1 && fuxi_gpu_scanout_publish()==0);
 assert(fuxi_gpu_scanout_reserve());fuxi_gpu_scanout_release();assert(!reads && !writes && !observations);
 uint8_t *source=malloc(4544u*2436),*panel=malloc(4320u*2400);assert(source && panel);
 // Only exact option value 1 activates the lane/checker.
 fuxi_gpu_scanout_configure((uintptr_t)panel,2,1);assert(!fg_scanout_requested && !fg_scanout_checks);
 fuxi_gpu_scanout_configure((uintptr_t)panel,1,2);assert(fg_scanout_requested && !fg_scanout_checks);
 ready=0;assert(fuxi_gpu_scanout_acquire()==1 && !reads && !writes);ready=1;
 assert(!fuxi_gpu_scanout_acquire() && fuxi_gpu_scanout_owned() && !fg_scanout_writable);
 assert(intf[0x2b4/4]==1 && writes==1 && !starts);
 assert(fuxi_gpu_scanout_publish()==-1 && !starts);
 ticks+=19999;assert(!fuxi_gpu_scanout_acquire());
 ticks++;intf[0x2b0/4]=100;assert(!fuxi_gpu_scanout_acquire());
 intf[0x2b0/4]=2400;ctl[0x1c/4]=1;assert(!fuxi_gpu_scanout_acquire());
 // The read pointer keeps counting while idle; it must not block the drain.
 ctl[0x1c/4]=0;intf[0x298/4]=1200;assert(fuxi_gpu_scanout_acquire()==1 && fg_scanout_writable);
 // Existing console/display kick must not start an owned transfer, even idle.
 fuxi_display_kick();assert(!ctl[0x1c/4]);
 fg_client_busy=1;abiResult=1;assert(!fuxi_gpu_scanout_reserve() && !fg_scanout_reserved);
 abiResult=0;assert(fuxi_gpu_scanout_reserve() && !fg_client_busy && fg_scanout_reserved);
 assert(!fuxi_gpu_scanout_reserve());fuxi_gpu_scanout_release();assert(!fg_scanout_reserved);
 // A submitted copy cannot publish until the fence has retired.
 fg_busy=1;assert(!fuxi_gpu_scanout_acquire() && fuxi_gpu_scanout_publish()==-1 && !starts);
 fg_busy=0;assert(fuxi_gpu_scanout_publish()==1 && starts==1 && !fg_scanout_writable);
 assert(!fuxi_gpu_scanout_acquire());ctl[0x1c/4]=0;ticks+=20000;
 assert(fuxi_gpu_scanout_acquire()==1);
 // Drain timeout and unexpected AR are fail-closed, never an early CPU fallback.
 ctl[0x1c/4]=1;ticks=fg_scanout_timeout;assert(fuxi_gpu_scanout_acquire()==-1);
 assert(!fg_scanout_writable && fuxi_gpu_scanout_publish()==-1 && starts==1);
 reset();fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);assert(!fuxi_gpu_scanout_acquire());
 intf[0x2b4/4]|=0x80000000u;assert(fuxi_gpu_scanout_acquire()==-1 && !starts);
 reset();fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);failAddress=0xae362b4;
 assert(fuxi_gpu_scanout_acquire()==-1 && !writes && !starts);
 // CPU row-copy oracle: the exact centered crop, padding and every BGRA/alpha byte.
 reset();fuxi_gpu_scanout_configure((uintptr_t)panel,1,1);
 for(unsigned y=0;y<2436;y++)for(unsigned x=0;x<1136;x++)
  ((uint32_t *)source)[y*1136+x]=(y*0x314159u)^(x*0x1234567u);
 fg_scanout_source=(uintptr_t)source;fg_scanout_pitch=4544;
 fg_scanout_sx=22;fg_scanout_sy=18;fg_scanout_dx=fg_scanout_dy=0;
 fg_scanout_w=1080;fg_scanout_h=2400;
 for(unsigned y=0;y<2400;y++)memcpy(panel+y*4320,source+(y+18)*4544+22*4,4320);
 for(unsigned frame=0;frame<4;frame++)assert(fg_scanout_verify());
 assert(fg_scanout_checked==4);panel[0]^=1;assert(fg_scanout_verify()); // first four only
 fg_scanout_checked=0;assert(!fg_scanout_verify() && fg_scanout_checked==1);panel[0]^=1;
 panel[4320u*2400-1]^=0x80;assert(!fg_scanout_verify());panel[4320u*2400-1]^=0x80;
 fg_scanout_checked=0;fg_scanout_w=17;fg_scanout_h=3;fg_scanout_dx=3;fg_scanout_dy=5;
 memset(panel,0x5a,4320u*2400);
 for(unsigned y=0;y<3;y++)memcpy(panel+(y+5)*4320+3*4,source+(y+18)*4544+22*4,17*4);
 assert(fg_scanout_verify() && panel[0]==0x5a);
 // TE gate (fuxiscante=1): no floor; a TE after START plus a margin, then the
 // usual START/write-line checks. A TE racing START only delays.
 reset();modelTe=1;fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);
 assert(fg_scanout_te_mode==1 && fg_scanout_drain_us==0);
 intf[0x294/4]=2400;intf[0x298/4]=(5u<<16)|3000;intf[0x2b0/4]=2400;
 assert(!fuxi_gpu_scanout_acquire() && fuxi_gpu_scanout_owned() && fg_scanout_te_init==2400);
 assert(!fuxi_gpu_scanout_acquire() && fg_scanout_wait_te==1);           // no TE yet
 intf[0x298/4]=(6u<<16)|2410;assert(!fuxi_gpu_scanout_acquire() && fg_scanout_wait_te==2); // just after TE
 unsigned xfer=fg_scanout_wait_xfer;
 intf[0x298/4]=(6u<<16)|2500;intf[0x2b0/4]=100;assert(!fuxi_gpu_scanout_acquire() && fg_scanout_wait_xfer==xfer+1);
 intf[0x2b0/4]=2400;assert(fuxi_gpu_scanout_acquire()==1);
 assert(fuxi_gpu_scanout_publish()==1 && starts==1 && fg_scanout_te_frame==6);
 ctl[0x1c/4]=0;assert(!fuxi_gpu_scanout_acquire());                       // same TE frame
 intf[0x298/4]=(8u<<16)|2401;assert(fuxi_gpu_scanout_acquire()==1);       // two TEs: done
 // A stalled TE count falls back to the default floor from that START.
 assert(fuxi_gpu_scanout_publish()==1 && starts==2);ctl[0x1c/4]=0;
 ticks+=FG_SCAN_TE_STALL_US-1;assert(!fuxi_gpu_scanout_acquire() && fg_scanout_te_mode==1);
 ticks++;assert(!fuxi_gpu_scanout_acquire() && !fg_scanout_te_mode && fg_scanout_drain_us==FG_SCAN_DRAIN_US);
 assert(fuxi_gpu_scanout_acquire()==1 && !fg_scanout_failed);
 // An explicit floor still applies with the TE gate.
 reset();modelTe=1;modelDrain=4000;fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);
 assert(fg_scanout_te_mode==1 && fg_scanout_drain_us==4000);
 // Window summary: TE rate from the frame count, START gaps in 60 Hz periods.
 reset();fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);intf[0x2b0/4]=2400;
 assert(!fuxi_gpu_scanout_acquire());
 for(unsigned i=0;i<599;i++){
  ticks+=i%4==3?33333:16667;intf[0x298/4]=((i*2u+2)<<16)|2500; // TE at 120 Hz
  fg_scanout_writable=1;ctl[0x1c/4]=0;assert(fuxi_gpu_scanout_publish()==1);
 }
 assert(fg_scanout_iv[0]==449 && fg_scanout_iv[1]==149 && !fg_scanout_iv[2] && !fg_scanout_iv[3]);
 ticks+=16667;intf[0x298/4]=(1200u<<16)|2500;fg_scanout_writable=1;ctl[0x1c/4]=0;
 assert(fuxi_gpu_scanout_publish()==1 && !fg_scanout_iv[0] && !fg_scanout_iv[1] && fg_scanout_starts%600==0);
 panel120_tests(panel);
 free(source);free(panel);
 puts("PASS: actual scanout gate, delayed transfer/fence/START ordering, busy reservation, timeout/MMIO/AR fail-close, TE gate/stall fallback/window summary, full crop/alpha/tail pixel oracle");
}
'''


def compile_run(tmp, name, source, cpp=False, includes=()):
    src = tmp / (name + ('.cpp' if cpp else '.c'))
    src.write_text(source)
    exe = tmp / name
    subprocess.run(['clang++' if cpp else 'clang', '-std=c++17' if cpp else '-std=gnu11',
                    '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', *['-I'+str(p) for p in includes], str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def kernel_syntax(buildtree):
    """Compile only our sources against saved XNU headers; execute no tree scripts."""
    sync = module('scanout_syntax_sync', 'tools/sync-fuxi-unixnu.py')
    sync.FILES = {'iokit/Kernel/arm/SweetSurface.h': 'SweetSurface.h'}
    surface_header = sync.baseline()['iokit/Kernel/arm/SweetSurface.h']
    config = buildtree / 'build/xnu.obj/DEVELOPMENT_ARM64_FUXI'
    if not config.exists():
        config = buildtree / 'build/xnu.obj/DEVELOPMENT_ARM64_SWEET'
    with tempfile.TemporaryDirectory(prefix='fuxi-scanout-syntax-') as temp:
        temp = Path(temp)
        dev, arm = temp / 'bsd/dev', temp / 'iokit/Kernel/arm'
        dev.mkdir(parents=True)
        arm.mkdir(parents=True)
        (arm / 'SweetSurface.h').write_text(surface_header)
        for path in (ROOT / 'xnu/fuxi-adreno').iterdir():
            if path.suffix in ('.h', '.inc', '.c'):
                (dev / path.name).write_bytes(path.read_bytes())
        for name in ('gpu.c', 'gpu-a740.h', 'gpu-zap.h', 'gpu-handoff.h', 'gpu-clock.h'):
            (dev / name).write_bytes((ROOT / 'shim/fuxi-probe' / name).read_bytes())
        (arm / 'SweetSurface.cpp').write_bytes((ROOT / 'xnu/fuxi-scanout/SweetSurface.cpp').read_bytes())
        # Check the modified console function against the complete preexisting TU.
        pe = temp / 'pexpert/arm/pe_serial.c'
        pe.parent.mkdir(parents=True)
        original = (buildtree / 'xnu/pexpert/arm/pe_serial.c').read_text()
        before = module('scanout_kick_sync', 'tools/sync-fuxi-scanout.py').KICK_BEFORE
        pe.write_text(original)
        if before not in original:
            # The saved 0081 tree predates Fuxi. Apply only pe_serial text deltas
            # in our temporary tree, never mutate or run the saved checkout.
            for patch in sorted((ROOT / 'xnu/patches').glob('*.patch')):
                if patch.name < '0082' or patch.name >= '0108':
                    continue
                for chunk in re.split(r'(?=^--- a/)', patch.read_text(), flags=re.M):
                    if chunk.startswith('--- a/pexpert/arm/pe_serial.c\n'):
                        check = subprocess.run(['git', 'apply', '--check', '-'], input=chunk,
                                               text=True, cwd=temp, capture_output=True)
                        if check.returncode:
                            subprocess.run(['git', 'apply', '--reverse', '--check', '-'], input=chunk,
                                           text=True, cwd=temp, check=True, capture_output=True)
                        else:
                            subprocess.run(['git', 'apply', '--whitespace=nowarn', '-'], input=chunk,
                                           text=True, cwd=temp, check=True, capture_output=True)
            original = pe.read_text()
        assert before in original
        pe.write_text(original.replace(before, (ROOT / 'xnu/fuxi-scanout/display-kick.c').read_text()))
        # Compile the new union delta against the complete local saved XNU TU.
        lookup = temp / 'bsd/vfs/vfs_lookup.c'
        lookup.parent.mkdir(parents=True)
        lookup.write_bytes((buildtree / 'xnu/bsd/vfs/vfs_lookup.c').read_bytes())
        (lookup.parent / 'fuxi_unixnu_lookup.h').write_bytes(
            (ROOT / 'xnu/fuxi-unixnu/fuxi_unixnu_lookup.h').read_bytes())
        subprocess.run(['git', 'apply', '--check', str(ROOT / 'xnu/patches/0110-fuxi-union-lower-directory.patch')],
                       cwd=temp, check=True)
        subprocess.run(['git', 'apply', str(ROOT / 'xnu/patches/0110-fuxi-union-lower-directory.patch')],
                       cwd=temp, check=True)
        for comp, path in [('bsd', dev / 'fuxi_adreno.c'), ('iokit', arm / 'SweetSurface.cpp'), ('pexpert', pe), ('bsd', lookup)]:
            cwd = config / comp / 'DEVELOPMENT'
            old_root = re.search(r'^SOURCE_DIR=(.+)/xnu$', (cwd / 'Makefile').read_text(), re.M)[1]
            raw = (cwd / '.CFLAGS').read_text().replace('\0', '')
            raw = re.sub(r'(-DCONFIG_NMBCLUSTERS=\(\(1024 \* 512\) / MCLBYTES\))', lambda m: shlex.quote(m[1]), raw)
            saved = shlex.split(raw)[1:]
            if comp == 'iokit':
                saved += shlex.split((cwd / '.CXXFLAGS').read_text().replace('\0', ''))[1:]
            cmd = ['clang++' if comp == 'iokit' else 'clang', '--target=arm64-apple-macos14',
                   '-fsyntax-only', '-DFUXI', '-mgeneral-regs-only', '-Werror', '-Wno-c99-designator',
                   '-Wno-reorder-init-list', '-D__builtin_xnu_type_signature(...)=""',
                   '-D__builtin_xnu_type_summary(...)=0', '-D__builtin_xnu_types_compatible(...)=1',
                   '-I'+str(dev), '-I'+str(arm)]
            skip = False
            for arg in saved:
                if skip:
                    skip = False
                    continue
                if arg == '-arch':
                    skip = True
                    continue
                arg = arg.replace(old_root, str(buildtree))
                arg = re.sub(r'-I.*?\.sdk/+System/Library/Frameworks/Kernel\.framework/Versions/A/PrivateHeaders/platform',
                             '-I'+str(buildtree / 'sdk-kernel-platform/platform'), arg)
                cmd.append(arg)
            subprocess.run(cmd + [str(path)], cwd=cwd, check=True)
            print('PASS: complete '+path.name+' syntax against saved XNU kernel headers/flags')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu-buildtree', type=Path, help='optional saved XNU build tree for full-TU syntax checks')
    args = parser.parse_args()
    subprocess.run(['python3', str(ROOT / 'tools/sync-fuxi-scanout.py'), '--check'], check=True)
    inc = (ROOT / 'xnu/fuxi-adreno/fuxi_gpu_scanout.inc').read_text()
    inc = re.sub(r'__asm__ volatile\([^;]*\);', '(void)0;', inc)
    kick = (ROOT / 'xnu/fuxi-scanout/display-kick.c').read_text()
    kick = re.sub(r'__asm__ volatile\([^;]*\);', '(void)0;', kick)
    cache = module('scanout_cache', 'tools/test-fuxi-adreno-cache.py')
    display = module('scanout_display', 'tools/test-fuxi-unixnu.py')
    with tempfile.TemporaryDirectory(prefix='fuxi-scanout-test-') as tmp:
        tmp = Path(tmp)
        compile_run(tmp, 'gate', PREFIX + inc + kick + TEST)
        # Compile final production SweetSurface, including the reservation changes.
        source = (ROOT / 'xnu/fuxi-scanout/SweetSurface.cpp').read_text()
        declarations = source[source.index('struct sweet_gpu_backing'):source.index('static bool sweet_gpu_init')]
        funcs = ''.join(cache.submit.function(source, n) for n in (
            'sweet_gpu_init', 'sweet_gpu_drop', 'sweet_gpu_release', 'sweet_surface_gpu_copy',
            'sweet_surface_gpu_poll', 'sweet_gpu_writer', 'sweet_surface_gpu_stats'))
        stubs = '''\nstatic bool reserved,busy,owned;
static int fuxi_gpu_scanout_owned() {return owned;}
static int fuxi_gpu_scanout_reserve() {if(busy)return 0;assert(!reserved);reserved=true;return 1;}
static void fuxi_gpu_scanout_release() {assert(reserved);reserved=false;}
'''
        extra = '''
 ready=true;busy=true;owned=true;
 IOMemoryDescriptor pendingMemory;SweetSurface pendingSurface;pendingSurface.memory=&pendingMemory;
 unsigned beforeMaps=maps,beforeCopies=copies;
 assert(sweet_surface_gpu_copy(&pendingSurface,0x10000,0,0,0,0,1080,10)==3);
 assert(!reserved && maps==beforeMaps && copies==beforeCopies && pendingSurface.refs==1);
 busy=false;pendingSurface.gpuWriters=1;
 assert(sweet_surface_gpu_copy(&pendingSurface,0x10000,0,0,0,0,1080,10)==3 && !reserved);
 pendingSurface.gpuWriters=0;owned=false;
'''
        model = cache.MODEL.replace(' IOLockFree(sweet_gpu_lock);', extra + ' IOLockFree(sweet_gpu_lock);')
        compile_run(tmp, 'cache', cache.PREFIX + stubs + declarations + funcs + model, cpp=True)
        source = (ROOT / 'xnu/fuxi-unixnu/SweetFramebuffer.cpp').read_text()
        declaration = source[source.index('class AppleCLCD :'):source.index('OSDefineMetaClassAndStructors(AppleCLCD')]
        declaration = declaration.replace(': public IOMobileFramebuffer', '').replace('OSDeclareDefaultStructors(AppleCLCD);', 'public:')
        methods = ''.join(display.function(source, n) for n in (
            'AppleCLCD::presentWantedLocked', 'AppleCLCD::wakePresentLocked', 'sweet_fb_rearm',
            'AppleCLCD::vsyncFired', 'AppleCLCD::scanoutLog', 'get32', 'AppleCLCD::swap', 'AppleCLCD::present'))
        methods = re.sub(r'__asm__ volatile\s*\([^;]*\);', '(void)0;', methods)
        extra = '''
 // Gate deferral preserves dirty swap/pixels; no publication while waiting.
 fb.fScanout=true;fb.fScanoutLog=true;scanOwned=true;scanPublish=1;gpuMode=2;pollMode=1;
 fb.swap(rec);scanAcquire=0;
 unsigned prior=scanPublications;uint64_t saved=pixels[0];
 fb.vsyncFired(&fb,&timer);assert(fb.fDirty && !fb.fGpuPending && pixels[0]==saved && scanPublications==prior);
 scanAcquire=1;fb.vsyncFired(&fb,&timer);assert(fb.fGpuPending && scanPublications==prior);
 pollMode=0;fb.vsyncFired(&fb,&timer);assert(scanPublications==prior);
 pollMode=1;fb.vsyncFired(&fb,&timer);assert(!fb.fGpuPending && scanPublications==prior+1);
 // Busy GPU ring keeps newest swap; no CPU copy or premature transfer.
 gpuMode=3;fb.swap(rec);prior=scanPublications;fb.vsyncFired(&fb,&timer);
 assert(fb.fDirty && !fb.fGpuPending && scanPublications==prior);
 gpuMode=-1;fb.vsyncFired(&fb,&timer);assert(fb.fDirty && scanPublications==prior);
 // CPU fallback is legal after acquisition and publishes only after stores.
 gpuMode=0;fb.vsyncFired(&fb,&timer);assert(!fb.fDirty && scanPublications==prior+1);
 // Power-off is gated too; the completed black frame is explicitly sent once.
 fb.fPowerOff=true;fb.fFadeStep=0;scanAcquire=0;prior=scanPublications;
 pixels[0]=0x55;fb.vsyncFired(&fb,&timer);assert(pixels[0]==0x55 && !fb.fFadeStep && scanPublications==prior);
 scanAcquire=1;fb.vsyncFired(&fb,&timer);assert(!pixels[0] && fb.fFadeStep==1 && scanPublications==prior+1);
 // Fast ownership polls must not produce extra 60 Hz notifications/counts.
 fb.fPowerOff=false;fb.fDirty=false;client.fPort=1;
 uint64_t notifications=client.notifications,count=fb.fVsyncCount;
 fb.fNextTick=ticks+100;
 fb.vsyncFired(&fb,&timer);assert(client.notifications==notifications && fb.fVsyncCount==count);
 ticks=fb.fNextTick;fb.vsyncFired(&fb,&timer);
 assert(client.notifications==notifications+1 && fb.fVsyncCount==count+1);
 client.fPort=0;
 // Periodic diagnostic branch, including actual log formatting arguments.
 fb.fScanoutFrames=119;fb.scanoutLog(false,216,42000,0);assert(fb.fScanoutFrames==120);
'''
        model = display.DISPLAY_TEST.replace(' puts("PASS: actual framebuffer', extra + ' puts("PASS: actual framebuffer')
        compile_run(tmp, 'framebuffer', display.DISPLAY_PREFIX + declaration + methods + model,
                    cpp=True, includes=(ROOT / 'xnu/fuxi-unixnu',))
    print('PASS: production scanout integration; real panel completion/cache coherency still require device qualification')
    if args.xnu_buildtree:
        kernel_syntax(args.xnu_buildtree.resolve())


if __name__ == '__main__':
    main()
