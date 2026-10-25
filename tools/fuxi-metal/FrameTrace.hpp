/* Bounded, opt-in compositor timing; no per-frame heap storage. */
#pragma once
#include "TranslationPolicy.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
namespace fuxi {
using FrameClock=std::chrono::steady_clock;
inline uint64_t frameNanos(FrameClock::time_point start) {
 return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(FrameClock::now()-start).count());
}
inline unsigned frameCPUFromRegister(uint64_t value){return unsigned(value&0xfff);}
inline unsigned frameCPU() {
#if defined(__APPLE__) && defined(__aarch64__) && !defined(FUXI_MTL_HOST_TEST)
 // XNU 8796 machine_machdep.h and libsyscall/os/tsd.h: TPIDR_EL0 low 12 bits.
 uint64_t value;__asm__ __volatile__("mrs %0, TPIDR_EL0":"=r"(value));return frameCPUFromRegister(value);
#else
 return 4096;
#endif
}
struct FrameCounts {uint64_t pipelines=0,descriptors=0,allocations=0;};
// Count successful Vulkan pipeline/set/memory creations on the calling thread.
inline thread_local FrameCounts frameCounts;
enum class MergeSplit : size_t {none,encoder,attachments,area,actions,attachmentSample,storage,shadow,fetch,nonDraw,bufferAlias,count};
inline constexpr const char *mergeSplitNames[]={"none","encoder","attachments","area","actions","attachment_sample","storage","shadow","fetch","non_draw","buffer_alias"};
struct FrameSample {
 uint64_t wall=0,cpuBeforeCommit=0,shadowCopy=0,leaseLock=0,backendSubmit=0,gpuWait=0,gpuExec=0;
 uint64_t inflight=0,completionLatency=0;
 uint64_t commitCall=0,commitGap=0,admissionWait=0,snapshot=0,submitQueue=0;
 uint64_t backendLock=0,slotWait=0,flush=0,queueSubmit=0,retireLock=0,invalidate=0,retireHost=0,surfaceRelease=0,scheduledHandlers=0;
 uint64_t queueSubmits=0,imageBarriers=0,readBarriersSkipped=0,crossEncoderMerges=0;
 uint64_t drainWaits=0,conflictWaits=0,slotWaits=0;
 // Last observed handler batch / explicit wait, sampled on the following commit.
 uint64_t priorDelivery=0,priorHandlers=0,priorWaitCompleted=0;
 // Command buffer creation (steady clock); the window derives frame-to-frame cadence.
 uint64_t createdNs=0,interval=0;
 uint64_t mergedDraws=0;std::array<uint64_t,size_t(MergeSplit::count)> mergeSplits{};
 uint64_t draws=0,passes=0,gpuSamples=0;FrameCounts counts{};unsigned cpu=4096;bool encodeCountsUnknown=false;
};
inline thread_local FrameSample *currentFrame=nullptr;
struct FrameTraceScope {
 FrameSample *previous;
 explicit FrameTraceScope(FrameSample *sample):previous(currentFrame){currentFrame=sample;}
 ~FrameTraceScope(){currentFrame=previous;}
};
// Only BackboardGate accesses this, under its existing reporter/completion lock.
class FrameTraceWindow {
 static constexpr size_t capacity=1024;
 std::array<FrameSample,capacity> samples{};
 std::array<uint64_t,capacity> scratch{};
 uint64_t count=0,wallMax=0,draws=0,passes=0,encodeUnknown=0,gpuUnavailable=0;
 uint64_t mergedDraws=0;std::array<uint64_t,size_t(MergeSplit::count)> mergeSplits{};
 FrameCounts counts{};FrameSample totals{};std::array<uint64_t,9> cpus{};
 // Frame cadence vs 60 Hz ticks: <=1, <=2, <=3, >3 intervals of 16.7 ms.
 uint64_t lastCreated=0;std::array<uint64_t,4> cadence{};
public:
 void add(const FrameSample &s) {
  auto &stored=samples[count%capacity];stored=s;
  // Gaps over 100 ms are idle pauses, not cadence; they do not enter the histogram.
  if(s.createdNs && lastCreated && s.createdNs>lastCreated && s.createdNs-lastCreated<100000000){stored.interval=s.createdNs-lastCreated;uint64_t ticks=(stored.interval+8333333)/16666667;++cadence[ticks<=1?0:ticks==2?1:ticks==3?2:3];}
  if(s.createdNs)lastCreated=s.createdNs;++count;wallMax=std::max(wallMax,s.wall);
  draws+=s.draws;passes+=s.passes;counts.pipelines+=s.counts.pipelines;counts.descriptors+=s.counts.descriptors;counts.allocations+=s.counts.allocations;
  mergedDraws+=s.mergedDraws;for(size_t i=1;i<mergeSplits.size();++i)mergeSplits[i]+=s.mergeSplits[i];
  totals.queueSubmits+=s.queueSubmits;totals.imageBarriers+=s.imageBarriers;totals.readBarriersSkipped+=s.readBarriersSkipped;totals.crossEncoderMerges+=s.crossEncoderMerges;
  totals.drainWaits+=s.drainWaits;totals.conflictWaits+=s.conflictWaits;totals.slotWaits+=s.slotWaits;
  gpuUnavailable+=!s.gpuSamples;++cpus[s.cpu<8?s.cpu:8];encodeUnknown+=s.encodeCountsUnknown;
 }
 uint64_t size() const{return count;}
 uint64_t percentile(uint64_t FrameSample::*field,unsigned percent) {
  size_t n=size_t(std::min<uint64_t>(count,capacity));if(!n)return 0;
  for(size_t i=0;i<n;++i)scratch[i]=samples[i].*field;
  std::sort(scratch.begin(),scratch.begin()+n);return scratch[(n*percent+99)/100-1];
 }
 void report(FILE *out=stderr) {
  auto p=[&](uint64_t FrameSample::*field,unsigned percent){return percentile(field,percent)/1e6;};
  double n=count?double(count):1;
  flockfile(out); // Keep the histogram on the same line as its percentiles.
  fprintf(out,"FUXI MTL: frame trace n=%llu samples=%llu wall=%.3f/%.3f/%.3f ms cpu_before_commit=%.3f/%.3f shadow_copy=%.3f/%.3f lease_lock=%.3f/%.3f backend_submit=%.3f/%.3f gpu_wait=%.3f/%.3f gpu_exec=%.3f/%.3f draws/frame=%.2f render_passes/frame=%.2f pipelines/frame=%.2f descriptors/frame=%.2f allocations/frame=%.2f cpu_hist=",
   (unsigned long long)count,(unsigned long long)std::min<uint64_t>(count,capacity),
   p(&FrameSample::wall,50),p(&FrameSample::wall,90),wallMax/1e6,
   p(&FrameSample::cpuBeforeCommit,50),p(&FrameSample::cpuBeforeCommit,90),
   p(&FrameSample::shadowCopy,50),p(&FrameSample::shadowCopy,90),
   p(&FrameSample::leaseLock,50),p(&FrameSample::leaseLock,90),
   p(&FrameSample::backendSubmit,50),p(&FrameSample::backendSubmit,90),
   p(&FrameSample::gpuWait,50),p(&FrameSample::gpuWait,90),
   p(&FrameSample::gpuExec,50),p(&FrameSample::gpuExec,90),draws/n,passes/n,counts.pipelines/n,counts.descriptors/n,counts.allocations/n);
  for(unsigned i=0;i<8;++i)fprintf(out,"%s%u:%llu",i?",":"",i,(unsigned long long)cpus[i]);
  fprintf(out,",unknown:%llu encode_counts_unknown=%llu gpu_exec_unavailable=%llu",(unsigned long long)cpus[8],(unsigned long long)encodeUnknown,(unsigned long long)gpuUnavailable);
  if(systemTranslationPolicy().overlap)fprintf(out," overlap_depth=%.0f/%.0f completion_ms=%.3f/%.3f",double(percentile(&FrameSample::inflight,50)),double(percentile(&FrameSample::inflight,90)),p(&FrameSample::completionLatency,50),p(&FrameSample::completionLatency,90));
  if(systemTranslationPolicy().merge){fprintf(out," merged_draws/frame=%.2f merged_splits=",mergedDraws/n);
   for(size_t i=1;i<mergeSplits.size();++i)fprintf(out,"%s%s:%llu",i==1?"":",",mergeSplitNames[i],(unsigned long long)mergeSplits[i]);}
  fprintf(out," interval=%.3f/%.3f ms cadence_ticks=1:%llu,2:%llu,3:%llu,4+:%llu",p(&FrameSample::interval,50),p(&FrameSample::interval,90),(unsigned long long)cadence[0],(unsigned long long)cadence[1],(unsigned long long)cadence[2],(unsigned long long)cadence[3]);
  fprintf(out," submits/frame=%.2f image_barriers/frame=%.2f read_barriers_skipped/frame=%.2f cross_encoder_merges/frame=%.2f wait_reasons=drain:%llu,conflict:%llu,slots:%llu",totals.queueSubmits/n,totals.imageBarriers/n,totals.readBarriersSkipped/n,totals.crossEncoderMerges/n,(unsigned long long)totals.drainWaits,(unsigned long long)totals.conflictWaits,(unsigned long long)totals.slotWaits);
  auto stage=[&](const char *name,uint64_t FrameSample::*field){fprintf(out," %s=%.3f/%.3f",name,p(field,50),p(field,90));};
  stage("commit_call",&FrameSample::commitCall);stage("commit_gap",&FrameSample::commitGap);stage("admission_wait",&FrameSample::admissionWait);stage("snapshot",&FrameSample::snapshot);stage("submit_queue",&FrameSample::submitQueue);
  stage("backend_lock",&FrameSample::backendLock);stage("slot_wait",&FrameSample::slotWait);stage("flush",&FrameSample::flush);stage("queue_submit",&FrameSample::queueSubmit);stage("retire_lock",&FrameSample::retireLock);stage("invalidate",&FrameSample::invalidate);stage("retire_host",&FrameSample::retireHost);stage("surface_release",&FrameSample::surfaceRelease);stage("scheduled_handlers",&FrameSample::scheduledHandlers);
  stage("prior_delivery",&FrameSample::priorDelivery);stage("prior_handlers",&FrameSample::priorHandlers);stage("prior_wait_completed",&FrameSample::priorWaitCompleted);
  fputc('\n',out);
  funlockfile(out);
  count=wallMax=draws=passes=encodeUnknown=gpuUnavailable=mergedDraws=0;mergeSplits.fill(0);counts={};totals={};cpus.fill(0);cadence.fill(0);
 }
};
}
