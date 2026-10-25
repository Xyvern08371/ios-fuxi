/* Original host controls for bounded frame statistics and thread attribution. */
#define FUXI_MTL_HOST_TEST
#include "FrameTrace.hpp"
#include <cassert>
#include <cstring>
#include <thread>
int main(){
 using namespace fuxi;
 assert(frameCPUFromRegister(0xabcdef000ULL|6)==6);
 assert(frameCPUFromRegister(0xabc000ULL|0xfff)==4095);
 assert(frameCPU()==4096); // host has no Darwin register convention
 FrameTraceWindow window;assert(window.percentile(&FrameSample::wall,50)==0);
 for(unsigned i=1;i<=100;++i){FrameSample s;s.backendCPU=i*10000;s.frontendCPU=i*20000;s.frontendWall=i*30000;s.pipe=i*40000;s.desc=i*50000;s.record=i*60000;s.kernelIO=i*70000;s.pipeCPU=i*8000;s.commitCall=i*1000;s.commitGap=i*2000;s.planHits=119;s.planMisses=1;s.queueSubmits=2;s.imageBarriers=7;s.readBarriersSkipped=3;s.crossEncoderMerges=1;s.drainWaits=1;s.wall=i*1000000ULL;s.completionLatency=s.wall;s.inflight=i%2+1;s.cpu=i%7;s.draws=2;s.passes=1;s.mergedDraws=1;s.mergeSplits[size_t(MergeSplit::encoder)]=2;s.counts={1,2,3};window.add(s);}
 assert(window.size()==100 && window.percentile(&FrameSample::wall,50)==50000000 && window.percentile(&FrameSample::wall,90)==90000000);
 FILE *out=tmpfile();assert(out);window.report(out);rewind(out);char line[4096];assert(fgets(line,sizeof(line),out));
 assert(strstr(line,"frame trace n=100 samples=100 wall=50.000/90.000/100.000 ms"));
 assert(strstr(line,"plan_hits/frame=119.00 plan_misses/frame=1.00"));
 assert(strstr(line,"draws/frame=2.00 render_passes/frame=1.00 pipelines/frame=1.00 descriptors/frame=2.00 allocations/frame=3.00"));
 if(systemTranslationPolicy().overlap)assert(strstr(line,"overlap_depth=1/2 completion_ms=50.000/90.000"));else assert(!strstr(line,"overlap_depth="));
 if(systemTranslationPolicy().merge)assert(strstr(line,"merged_draws/frame=1.00 merged_splits=encoder:200,attachments:0"));else assert(!strstr(line,"merged_draws/frame="));
 assert(strstr(line,"submits/frame=2.00 image_barriers/frame=7.00 read_barriers_skipped/frame=3.00 cross_encoder_merges/frame=1.00 wait_reasons=drain:100,conflict:0,slots:0"));
 assert(strstr(line,"commit_call=0.050/0.090 commit_gap=0.100/0.180"));
 assert(strstr(line,"prior_wait_completed=0.000/0.000 backend_cpu=0.500/0.900 frontend_cpu=1.000/1.800 frontend_wall=1.500/2.700 pipe=2.000/3.600 desc=2.500/4.500 record=3.000/5.400 kernel_io=3.500/6.300"));
 assert(strstr(line,"pipe_cpu=0.400/0.720 desc_cpu=0.000/0.000 record_cpu=0.000/0.000 flush_cpu=0.000/0.000 kernel_io_cpu=0.000/0.000 queue_submit_cpu=0.000/0.000 cpu_sampled_submits/frame=0.00 cpu_omitted_submits/frame=0.00\n"));
 assert(!window.size());fclose(out);
 // Overflow retains the newest 1024 samples; window max includes evicted data.
 FrameSample s;s.wall=9999999999;window.add(s);
 for(unsigned i=0;i<2048;++i){s.wall=1000000;window.add(s);}
 assert(window.size()==2049 && window.percentile(&FrameSample::wall,90)==1000000);
 out=tmpfile();assert(out);window.report(out);rewind(out);assert(fgets(line,sizeof(line),out));fclose(out);
 assert(strstr(line,"plan_hits/frame=0.00 plan_misses/frame=0.00"));
 assert(strstr(line,"n=2049 samples=1024 wall=1.000/1.000/10000.000"));
 assert(strstr(line,"submits/frame=0.00 image_barriers/frame=0.00"));
 if(systemTranslationPolicy().merge)assert(strstr(line,"merged_draws/frame=0.00 merged_splits=encoder:0"));
 FrameSample first,second;assert(!currentFrame);
 {FrameTraceScope a(&first);assert(currentFrame==&first);
  {FrameTraceScope b(&second);assert(currentFrame==&second);}assert(currentFrame==&first);
  std::thread worker([]{assert(!currentFrame && frameCounts.allocations==0);frameCounts.allocations=3;});worker.join();
  assert(!frameCounts.allocations);
 }assert(!currentFrame);
 // Real host thread CPU distinguishes a sleep from work on this same thread.
 auto wallStart=FrameClock::now();auto cpuStart=frameThreadCPU();assert(cpuStart!=UINT64_MAX);
 std::this_thread::sleep_for(std::chrono::milliseconds(40));
 auto sleptCPU=frameCPUDelta(cpuStart,frameThreadCPU());assert(sleptCPU<frameNanos(wallStart)/2);
 cpuStart=frameThreadCPU();while(frameCPUDelta(cpuStart,frameThreadCPU())<2000000){}
 assert(frameCPUDelta(cpuStart,frameThreadCPU())>=2000000);
 puts("PASS: bounded frame percentiles/reset/overflow/max, log fields, CPU mask and thread-local attribution");
}
