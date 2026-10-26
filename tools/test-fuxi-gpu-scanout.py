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
static unsigned fg_scanout_requested, fg_scanout_panel120_requested, fg_scanout_double_requested, fg_busy, fg_client_busy;
static unsigned ready=1,abiLive=1,abiResult,observations,reads,writes,starts,failAddress;
static uint64_t ticks=1000,machScale=1;
static uint64_t mach_absolute_time(void) {return ticks*machScale;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) {*ns=t*1000/machScale;}
static void nanoseconds_to_absolutetime(uint64_t ns,uint64_t *t) {*t=ns*machScale/1000;}
static uint32_t ctl[1024],intf[1024],dsi[1024],sspp[1024];
static struct {unsigned pa,value;} trace[8192];
static unsigned traceCount,dsiReads,dmaStarts,pendingDma,noDone,busyAfterDone,staleDone,frozenClock,failWriteAddress;
static unsigned errorOnDone,tearReadbackMismatch;
static volatile uint32_t *fuxi_ctl0=ctl,*fuxi_intf1=intf;
static int fuxi_gpu_ready(void) {return ready;}
static unsigned modelDrain,modelTe,modelPanel120,modelDouble;
static uint8_t modelB[4320u*2400];
static unsigned failBMap;
static uintptr_t fuxi_gpu_nc_pointer(uint64_t pa,uint64_t size) {
 assert(pa==0xb8a00000u && size==4320u*2400);return failBMap?0:(uintptr_t)modelB;
}
static unsigned fg_scanout_owned_for_model(void);
static unsigned fg_scanout_double_for_model(void);
static int PE_parse_boot_argn(const char *name,void *value,int size) {
 unsigned v=!strcmp(name,"fuxiscante")?modelTe:!strcmp(name,"fuxiscandrain")?modelDrain:
     !strcmp(name,"fuxipanel120")?modelPanel120:!strcmp(name,"fuxiscandouble")?modelDouble:0;
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
 if(pa>=0xae05000 && pa<0xae06000)reg=&sspp[(pa-0xae05000)/4];
 else if(pa>=0xae16000 && pa<0xae17000)reg=&ctl[(pa-0xae16000)/4];
 else if(pa>=0xae94000 && pa<0xae95000)reg=&dsi[(pa-0xae94000)/4];
 else {assert(pa>=0xae36000 && pa<0xae37000);reg=&intf[(pa-0xae36000)/4];}
 if(wr){
  writes++;assert(traceCount<8192);trace[traceCount].pa=pa;trace[traceCount++].value=*value;
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
  if(pa==0xae1601c){
   if(fg_scanout_double_for_model()) {
    assert(!intf[0x2b0/4] || intf[0x2b0/4]>=2400);
    assert(traceCount>=3 && trace[traceCount-2].pa==0xae16018 && trace[traceCount-3].pa==0xae05014);
    assert(ctl[0x18/4]&1);
   }
   assert(!fg_busy && !(intf[0x2b4/4]&0x80000000u));starts++;}
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
static unsigned fg_scanout_double_for_model(void) {return fg_scanout_double_active;}
static void reset(void) {
 memset(sspp,0,sizeof(sspp));sspp[0x14/4]=0xb8000000u;
 fg_scanout_double_requested=fg_scanout_double_active=fg_scanout_double_initialized=fg_scanout_front=0;
 fg_scanout_back_ready=fg_scanout_flips=fg_scanout_ready_drops=fg_scanout_retries=0;
 fg_scanout_current_match=fg_scanout_current_mismatch=modelDouble=failBMap=0;
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
 ticks=1000;machScale=1;ready=abiLive=1;abiResult=observations=reads=writes=starts=failAddress=0;
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
static int te_sample(struct fg_te_observer *te,unsigned frame,uint64_t time) {
 ticks=time;intf[0x298/4]=((frame&0xffffu)<<16)|2500;
 return fuxi_gpu_scanout_te(te);
}
static void te_estimator_tests(void) {
 reset();struct fg_te_observer te={0};unsigned before=reads;
 assert(!fuxi_gpu_scanout_te(&te) && reads==before && !te.initialized);
 fg_scanout_owned=1;
 // Both mach timebase conversion and measured 60.4/120.8 Hz, with jitter
 // independent of the TE phase. Counter wrap is not a discontinuity.
 for(unsigned scale=1;scale<=3;scale+=2)for(unsigned mode=0;mode<2;mode++) {
  memset(&te,0,sizeof(te));machScale=scale;
  fg_scanout_period_us=mode?8333:16667;
  unsigned period=mode?8278:16556,frame=65530,valid=0;
  uint64_t origin=1000000;
  for(uint64_t t=origin+100;t<origin+period*160ull;t+=417+(t%113)) {
   unsigned n=(unsigned)((t-origin)/period);
   int available=te_sample(&te,frame+n,t);
   if(n<2)assert(!available);
   if(available) {
    valid++;
    uint64_t edge=(origin+(uint64_t)n*period)*scale;
    assert(te.frame==((frame+n)&0xffff));
    assert(te.edge<=t*scale && te.edge+600*scale>=edge && te.edge<=edge+600*scale);
    if(n>80)assert(te.period+100*scale>=period*scale && te.period<=(period+100)*scale);
   }
  }
  assert(valid>1000 && !writes && !observations && !starts);
  // A frozen count expires instead of extrapolating indefinitely.
  assert(!te_sample(&te,te.frame,ticks+period*2));
  assert(!te.samples);
 }
 machScale=1;fg_scanout_period_us=16667;memset(&te,0,sizeof(te));
 assert(!te_sample(&te,1,1000000));
 assert(!te_sample(&te,1,1016400));assert(!te_sample(&te,2,1016800));
 assert(!te_sample(&te,2,1033000));assert(te_sample(&te,3,1033400));
 // Established phase survives a copy spanning two confirmed edges without
 // teaching its uncertain bracket to the period filter.
 uint64_t lockedPeriod=te.period,lockedEdge=te.edge;
 assert(te_sample(&te,5,1066700) && te.edge==lockedEdge+lockedPeriod*2 && te.period==lockedPeriod);
 assert(te_sample(&te,5,1067000)); // same counter is still fresh
 // Skipped counter, backward/reset count, long polling gap and clock reversal
 // all force reacquisition. A read fault cannot expose the previous snapshot.
 assert(!te_sample(&te,7,1067100) && !te.samples);
 assert(!te_sample(&te,6,1067500) && !te.samples);
 assert(!te_sample(&te,7,1073300) && !te.samples);
 assert(!te_sample(&te,7,1000) && !te.samples);
 failAddress=FG_SCAN_INTF+0x298;assert(!fuxi_gpu_scanout_te(&te) && !te.initialized);
 failAddress=0;fg_scanout_owned=0;before=reads;
 assert(!fuxi_gpu_scanout_te(&te) && reads==before);
 // Ownership and a panel timing switch each start with fresh warmup.
 fg_scanout_owned=1;assert(!te_sample(&te,10,2000000));
 fg_scanout_period_us=8333;assert(!te_sample(&te,11,2008300) && te.nominal_us==8333 && !te.samples);
 puts("PASS: actual lock-free TE observer: jitter/period filter, mach units, 60/120Hz, wrap, stall/MMIO/ownership/gap/reset fallback");
}
static void double_setup(uint8_t *panel) {
 reset();modelDouble=1;modelTe=1;
 intf[0x294/4]=2400;intf[0x298/4]=(1u<<16)|2500;intf[0x2b0/4]=2400;
 memset(panel,0x5a,4320u*2400);memset(modelB,0xcc,sizeof(modelB));
 fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);
 assert(!fuxi_gpu_scanout_acquire());
 intf[0x298/4]=(2u<<16)|2500;
 assert(fuxi_gpu_scanout_acquire()==1 && fuxi_gpu_scanout_double());
 assert(!memcmp(panel,modelB,sizeof(modelB)) && !fg_scanout_front);
}
static void double_tests(uint8_t *panel) {
 for(unsigned option=0;option<3;option++) {
  reset();modelDouble=option;fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);
  assert(fg_scanout_double_requested==(option==1));
 }
 reset();modelDouble=1;fuxi_gpu_scanout_configure((uintptr_t)panel,0,0);
 assert(!fg_scanout_double_requested && !fuxi_gpu_scanout_double() && !reads && !writes);
 // The production oracle must read back the GPU's back buffer, never front.
 double_setup(panel);fg_scanout_checks=4;fg_scanout_source=(uintptr_t)panel;
 fg_scanout_pitch=4320;fg_scanout_sx=fg_scanout_sy=fg_scanout_dx=fg_scanout_dy=0;
 fg_scanout_w=1080;fg_scanout_h=2400;
 assert(fg_scanout_verify());modelB[sizeof(modelB)-1]^=1;assert(!fg_scanout_verify());
 modelB[sizeof(modelB)-1]^=1;fg_scanout_front=1;fg_scanout_source=(uintptr_t)modelB;
 assert(fg_scanout_verify());panel[0]^=1;assert(!fg_scanout_verify());
 double_setup(panel);ctl[0x18/4]=0x80000800u;
 fg_busy=1;assert(fuxi_gpu_scanout_publish()==-1 && !fuxi_gpu_scanout_pending() && !starts);
 fg_busy=0;unsigned t=traceCount;
 assert(fuxi_gpu_scanout_publish()==1 && starts==1 && fg_scanout_front==1);
 assert(trace[t].pa==0xae05014 && trace[t].value==0xb8a00000u);
 assert(trace[t+1].pa==0xae16018 && trace[t+1].value==0x80000801u);
 assert(trace[t+2].pa==0xae1601c && trace[t+2].value==1);
 // Copy acquisition overlaps an active transfer. Publication waits for BOTH
 // transfer indicators and a later TE; ring/copy busy can never issue START.
 intf[0x2b0/4]=100;assert(fuxi_gpu_scanout_acquire()==1);
 fg_busy=1;assert(!fuxi_gpu_scanout_retry() && fuxi_gpu_scanout_publish()==-1 && starts==1);
 fg_busy=0;assert(fuxi_gpu_scanout_publish()==2 && fuxi_gpu_scanout_pending());
 assert(fuxi_gpu_scanout_retry()==2 && starts==1); // same TE
 intf[0x298/4]=(3u<<16)|2500;
 assert(fuxi_gpu_scanout_retry()==2 && starts==1); // START busy
 ctl[0x1c/4]=0;assert(fuxi_gpu_scanout_retry()==2 && starts==1); // write-line busy
 intf[0x2b0/4]=2400;t=traceCount;
 assert(fuxi_gpu_scanout_retry()==1 && starts==2 && !fg_scanout_front && !fuxi_gpu_scanout_pending());
 assert(trace[t].pa==0xae05014 && trace[t].value==0xb8000000u);
 assert(trace[t+1].pa==0xae16018 && trace[t+2].pa==0xae1601c);
 // Complete copies may be ready with no dirty swaps/subscribers: timer has
 // an independent pending query and a retry does not invent another frame.
 assert(!fuxi_gpu_scanout_retry() && starts==2 && fg_scanout_retries>=4);
 // Sample CURRENT_SRC0 read-only, at the 600-START cadence. It is unverified:
 // mismatches count without driving ownership decisions.
 for(unsigned i=0;i<2;i++) {
  ctl[0x1c/4]=0;intf[0x298/4]+=1u<<16;
  assert(fuxi_gpu_scanout_acquire()==1);fg_scanout_starts=599;
  sspp[0xa4/4]=i?0:0xb8a00000u;
  assert(fuxi_gpu_scanout_publish()==1);
 }
 assert(fg_scanout_current_match==1 && fg_scanout_current_mismatch==1);
 for(unsigned i=0;i<traceCount;i++)assert(trace[i].pa!=0xae050a4);
 // CPU fallback and black fill must first drain, keep fPixels=A and restore
 // its complete image even when front=B. It stays single-buffer thereafter.
 double_setup(panel);assert(fuxi_gpu_scanout_publish()==1);
 memset(modelB,0x39,sizeof(modelB));memset(panel,0xcc,4320u*2400);
 assert(!fuxi_gpu_scanout_cpu_acquire() && panel[0]==0xcc && fg_scanout_double_active);
 ctl[0x1c/4]=0;intf[0x298/4]+=1u<<16;intf[0x2b0/4]=100;
 assert(!fuxi_gpu_scanout_cpu_acquire() && panel[0]==0xcc);
 intf[0x2b0/4]=2400;fg_busy=1;assert(!fuxi_gpu_scanout_cpu_acquire());fg_busy=0;
 assert(fuxi_gpu_scanout_cpu_acquire()==1 && !fuxi_gpu_scanout_double() && !memcmp(panel,modelB,sizeof(modelB)));
 assert(sspp[0x14/4]==0xb8000000u && (ctl[0x18/4]&1));
 memset(panel,0,4320u*2400);assert(fuxi_gpu_scanout_publish()==1);
 assert(!fuxi_gpu_scanout_acquire());ctl[0x1c/4]=0;intf[0x298/4]+=1u<<16;
 assert(fuxi_gpu_scanout_acquire()==1 && !fuxi_gpu_scanout_double());
 // Preserve a newer ready image, whether it is A or B, before a CPU
 // partial update discards that unstarted frame.
 for(unsigned front=0;front<2;front++) {
  double_setup(panel);
  if(front) {assert(fuxi_gpu_scanout_publish()==1);ctl[0x1c/4]=0;intf[0x298/4]+=1u<<16;}
  assert(fuxi_gpu_scanout_acquire()==1);
  memset(panel,front?0x77:0x11,4320u*2400);memset(modelB,front?0x11:0x77,sizeof(modelB));
  fg_scanout_back_ready=1;
  assert(fuxi_gpu_scanout_cpu_acquire()==1 && panel[0]==0x77 && panel[4320u*2400-1]==0x77);
  assert(!fuxi_gpu_scanout_pending() && fg_scanout_ready_drops==1);
 }
 // Supplied 120.8Hz/7.3ms transfer: copy 626..1246us into back overlaps
 // the transfer. Each next TE can START, without waiting to begin the copy.
 double_setup(panel);fg_scanout_panel120_active=1;fg_scanout_period_us=8333;
 assert(fuxi_gpu_scanout_publish()==1);
 for(unsigned frame=1;frame<600;frame++) {
  uint64_t began=ticks;intf[0x2b0/4]=100;
  assert(fuxi_gpu_scanout_acquire()==1);
  fg_busy=1;assert(fuxi_gpu_scanout_publish()==-1 && !fuxi_gpu_scanout_retry());
  ticks=began+(frame&1?626:1246);fg_busy=0;
  assert(fuxi_gpu_scanout_publish()==2);
  ticks=began+7300;ctl[0x1c/4]=0;intf[0x2b0/4]=2400;
  assert(fuxi_gpu_scanout_retry()==2); // transfer done, still same TE
  ticks=began+8274;intf[0x298/4]+=1u<<16;
  sspp[0xa4/4]=fg_scanout_front?0xb8000000u:0xb8a00000u;
  assert(fuxi_gpu_scanout_retry()==1);
  if(frame==598)assert(fg_scanout_iv[0]==598 && !fg_scanout_iv[1] && !fg_scanout_iv[2] && !fg_scanout_iv[3]);
 }
 assert(starts==600 && fg_scanout_current_match==1 && !fg_scanout_failed);
 // Every new register access failure holds writes; stop restores A shadow
 // before any future console handoff, with no speculative START.
 const unsigned failures[]={0xae05014,0xae16018,0xae050a4};
 for(unsigned i=0;i<3;i++) {
  double_setup(panel);assert(fuxi_gpu_scanout_publish()==1);
  ctl[0x1c/4]=0;intf[0x298/4]+=1u<<16;assert(fuxi_gpu_scanout_acquire()==1);
  if(i==2)fg_scanout_starts=599;
  if(i==0)failWriteAddress=failures[i];else failAddress=failures[i];
  assert(fuxi_gpu_scanout_publish()==-1 && fg_scanout_failed && !fg_scanout_writable && !fuxi_gpu_scanout_pending());
  if(i==2)assert(sspp[0x14/4]==0xb8000000u && (ctl[0x18/4]&1));
 }
 double_setup(panel);assert(fuxi_gpu_scanout_publish()==1);sspp[0x14/4]=0x12340000u;
 assert(fuxi_gpu_scanout_acquire()==-1 && sspp[0x14/4]==0xb8000000u && starts==1);
 double_setup(panel);assert(fuxi_gpu_scanout_publish()==1);failAddress=0xae05014;
 assert(fuxi_gpu_scanout_acquire()==-1 && !fg_scanout_writable);
 double_setup(panel);assert(fuxi_gpu_scanout_publish()==1);
 fuxi_gpu_scanout_configure((uintptr_t)panel,0,0);
 assert(fg_scanout_owned && fg_scanout_failed && sspp[0x14/4]==0xb8000000u && (ctl[0x18/4]&1));
 assert(fuxi_gpu_scanout_acquire()==-1 && !fuxi_gpu_scanout_pending() && starts==1);
 double_setup(panel);assert(fuxi_gpu_scanout_publish()==1);t=traceCount;
 assert(fg_scanout_stop("modeled stop")==-1);
 assert(trace[t].pa==0xae05014 && trace[t].value==0xb8000000u);
 assert(trace[t+1].pa==0xae16018 && starts==1 && fuxi_gpu_scanout_acquire()==-1);
 double_setup(panel);assert(fuxi_gpu_scanout_publish()==1);failAddress=0xae36298;
 struct fg_te_observer failedTe={0};
 assert(!fuxi_gpu_scanout_te(&failedTe) && fg_scanout_failed && sspp[0x14/4]==0xb8000000u);
 reset();modelDouble=1;sspp[0x14/4]=0xb8a00000u;
 fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);assert(fuxi_gpu_scanout_acquire()==-1 && !starts);
 assert(fg_scanout_owned && sspp[0x14/4]==0xb8000000u && (ctl[0x18/4]&1));
 reset();modelDouble=1;failAddress=0xae05014;
 fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);assert(fuxi_gpu_scanout_acquire()==-1 && !starts);
 reset();modelDouble=1;failBMap=1;fuxi_gpu_scanout_configure((uintptr_t)panel,1,0);
 assert(!fuxi_gpu_scanout_acquire());ticks+=20000;assert(fuxi_gpu_scanout_acquire()==-1 && !starts);
 puts("PASS: actual double ownership, initialization, overlap, deferred publish/TE/retirement, SRC0/flush/START order, sampling, CPU fallback and stop restoration");
}
int main(void) {
 te_estimator_tests();
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
 double_tests(panel);
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
        compile_run(tmp, 'gate', PREFIX + display.TE_OBSERVER + inc + kick + TEST)
        # Compile the exact USB screenshot request body; intercept the physical
        # memcpy to assert its address and bounds without touching device memory.
        probe = (ROOT / 'shim/fuxi-probe/probe.c').read_text()
        body = probe.split('} else if (type == 0xc0 && req == 0x4e) {', 1)[1].split(
            '} else if (type == 0xc0 && req == 0x4f)', 1)[0]
        compile_run(tmp, 'screenshot', r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
static uint32_t base,pitch=4320,format=0x237ff;
static uint64_t copiedFrom;static unsigned copiedBytes;
static uint32_t usb_rd(unsigned block,unsigned offset) {
 assert(block==0xae05000);return offset==0x14?base:offset==0x24?pitch:format;
}
static void mock_copy(void *dst,const void *src,unsigned n) {
 (void)dst;copiedFrom=(uintptr_t)src;copiedBytes=n;
}
#define memcpy mock_copy
static bool screenshot(uint64_t requestedOffset,unsigned length) {
 uint8_t st[6]={0},data[512];unsigned n=0;bool ok=true;
 uint16_t value=requestedOffset;st[4]=requestedOffset>>16;st[5]=requestedOffset>>24;
 copiedFrom=copiedBytes=0;
""" + body + r"""
 return ok;
}
int main(void) {
 const unsigned size=4320u*2400;
 for(unsigned b=0;b<2;b++) {
  base=b?0xb8a00000u:0xb8000000u;
  assert(screenshot(123,1024) && copiedFrom==(uint64_t)base+123 && copiedBytes==512);
  assert(screenshot(size-512,512) && copiedFrom==(uint64_t)base+size-512);
  assert(screenshot(size,0) && !copiedBytes);
  assert(!screenshot(size,1) && !copiedFrom && !copiedBytes);
  assert(!screenshot(size-511,512) && !copiedFrom);
  assert(!screenshot(size+1,0));
 }
 base=0xb9400000u;assert(!screenshot(0,512) && !copiedFrom);
 base=0xb8000000u;pitch=1;assert(!screenshot(0,512));
 pitch=4320;format=0;assert(!screenshot(0,512));
 puts("PASS: actual USB screenshot request, current A/B base, one-frame bounds, pitch/format refusals and length cap");
 return 0;
}
""")
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
            'sweet_fb_te_configure', 'AppleCLCD::teVsyncTick', 'AppleCLCD::teVsyncLog',
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
 // CPU fallback cannot write until its distinct drain/restore guard grants A.
 gpuMode=0;scanCpuAcquire=0;saved=pixels[0];unsigned cpuGuards=scanCpuAcquires;
 fb.vsyncFired(&fb,&timer);assert(fb.fDirty && pixels[0]==saved && scanPublications==prior && scanCpuAcquires==cpuGuards+1);
 scanCpuAcquire=1;fb.vsyncFired(&fb,&timer);assert(!fb.fDirty && scanPublications==prior+1);
 // Power-off is gated too; the completed black frame is explicitly sent once.
 fb.fPowerOff=true;fb.fFadeStep=0;scanAcquire=0;prior=scanPublications;
 pixels[0]=0x55;fb.vsyncFired(&fb,&timer);assert(pixels[0]==0x55 && !fb.fFadeStep && scanPublications==prior);
 scanAcquire=1;scanCpuAcquire=0;fb.vsyncFired(&fb,&timer);
 assert(pixels[0]==0x55 && !fb.fFadeStep && scanPublications==prior);
 scanCpuAcquire=1;fb.vsyncFired(&fb,&timer);assert(!pixels[0] && fb.fFadeStep==1 && scanPublications==prior+1);
 // A ready back buffer alone keeps demand polling alive and gets retried,
 // even with no pending swap, GPU fence, blanking or vsync subscribers.
 fb.fPowerOff=false;fb.fDirty=false;fb.fGpuPending=false;scanBackReady=true;
 unsigned retries=scanRetries,beforeArms=arms;
 assert(fb.presentWantedLocked());sweet_fb_rearm(&fb,&timer);assert(arms==beforeArms+1);
 fb.vsyncFired(&fb,&timer);assert(scanRetries==retries+1);
 scanBackReady=false;
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
        # Run the same framebuffer methods with the real observer too: the
        # counter is derived from the modeled panel clock, independently of
        # scheduled software deadlines. Slow copies straddle edges at 120Hz.
        observer = display.function(inc, 'fuxi_gpu_scanout_te').replace(
            'int fuxi_gpu_scanout_te(', 'static int model_scanout_te(')
        # Compile the actual observer's integer/time arithmetic for the target.
        arm = tmp / 'te-arm.c'
        arm.write_text('#include <stdint.h>\n' + display.TE_OBSERVER + '''
#define FG_SCAN_INTF 0x0ae36000u
#define FG_GUARD
static unsigned fg_scanout_double_active;
static int fg_scanout_stop(const char *reason) {(void)reason;return -1;}
static unsigned fg_scanout_period_us=16667;
int fuxi_gpu_scanout_owned(void);
int fg_scanout_read(unsigned,unsigned *);
uint64_t mach_absolute_time(void);
void nanoseconds_to_absolutetime(uint64_t,uint64_t *);
''' + display.function(inc, 'fuxi_gpu_scanout_te'))
        subprocess.run(['clang', '--target=aarch64-none-elf', '-ffreestanding',
                        '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
                        '-c', str(arm), '-o', str(tmp / 'te-arm.o')], check=True)
        prefix = display.DISPLAY_PREFIX.replace('static bool teAvailable;',
            'static bool teAvailable,teUseEstimator;\nstatic int model_scanout_te(struct fg_te_observer *);')
        prefix = prefix.replace(' teReads++;te->edge=teEdge;',
            ' teReads++;if(teUseEstimator)return model_scanout_te(te);te->edge=teEdge;')
        prefix += r'''
#define FG_SCAN_INTF 0u
#define FG_GUARD
static unsigned fg_scanout_double_active;
static int fg_scanout_stop(const char *reason) {(void)reason;return -1;}
static unsigned fg_scanout_period_us=16667;
static uint64_t panelOrigin,panelPeriod;
static bool panelFrozen;
static unsigned frozenFrame;
static int fg_scanout_read(unsigned address,unsigned *count) {
 assert(address==0x298);
 unsigned frame=panelFrozen?frozenFrame:(unsigned)((ticks-panelOrigin)/panelPeriod);
 *count=(frame&0xffffu)<<16;return 1;
}
''' + observer
        integrated = r'''
 {
 teUseEstimator=true;scanOwned=true;client.fPort=1;
 for(uint64_t period:{16556000ull,8278000ull})for(uint64_t offset:{7800000ull,16000000ull}) {
  AppleCLCD timed={};IOTimerEventSource beat;
  timed.fLock=&lock;timed.fClients=&clients;timed.fVsync=&beat;
  timed.fFlushTicks=1;timed.fScanout=timed.fVsyncTE=true;
  timed.fTickAbs=16667000;timed.fTeOffset=offset;
  panelOrigin=ticks+1000000;panelPeriod=period;ticks=panelOrigin;
  fg_scanout_period_us=period<10000000?8333:16667;
  timed.fNextTick=beat.next=ticks+timed.fTickAbs;
  unsigned lastFrame=~0u,locked=0,fallback=0,iterations=0;
  while(timed.fVsyncCount<650) {
   assert(++iterations<25000);
   if(ticks<beat.next)ticks=beat.next;
   uint64_t count=timed.fVsyncCount;
   timed.vsyncFired(&timed,&beat);
   if(!timed.fTeValid)lastFrame=~0u;
   if(timed.fVsyncCount==count)continue;
   if(timed.fTeValid) {
    if(lastFrame!=~0u)assert(((timed.fTeSentFrame-lastFrame)&0xffffu)==1);
    lastFrame=timed.fTeSentFrame;locked++;
    uint64_t phase=(timed.fTeLastNotify-panelOrigin)%period;
    uint64_t target=offset%period;
    uint64_t error=phase>target?phase-target:target-phase;
    if(error>period/2)error=period-error;
    assert(error<=3000000); // uncertainty grows when a copy hides the edge
    // A 3ms completed copy crosses the next TE for late offsets.
    ticks+=3000000;
   } else fallback++;
  }
  assert(locked>640 && fallback<10);
  // Freeze a real modeled counter, then allow it to advance and reacquire.
  panelFrozen=true;frozenFrame=(unsigned)((ticks-panelOrigin)/period);
  for(unsigned i=0;i<200;i++) {
   if(ticks<beat.next)ticks=beat.next;
   timed.vsyncFired(&timed,&beat);
  }
  assert(!timed.fTeValid && timed.fTeFallback>0);
  panelFrozen=false;
  for(unsigned i=0;i<200;i++) {
   if(ticks<beat.next)ticks=beat.next;
   timed.vsyncFired(&timed,&beat);
  }
  assert(timed.fTeValid);
 }
 teUseEstimator=false;scanOwned=false;client.fPort=0;
 puts("PASS: real TE observer + framebuffer timer: independent 60.4/120.8Hz panel, late offsets, 3ms copies, stall and reacquisition");
 }
'''
        model = model.replace(' puts("PASS: actual framebuffer', integrated + ' puts("PASS: actual framebuffer')
        compile_run(tmp, 'framebuffer', prefix + declaration + methods + model,
                    cpp=True, includes=(ROOT / 'xnu/fuxi-unixnu',))
    print('PASS: production scanout integration; real panel completion/cache coherency still require device qualification')
    if args.xnu_buildtree:
        kernel_syntax(args.xnu_buildtree.resolve())


if __name__ == '__main__':
    main()
