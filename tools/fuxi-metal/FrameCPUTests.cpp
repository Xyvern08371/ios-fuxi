/* Deterministic clock/attribution controls. Original host test, no GPU. */
#define FUXI_MTL_HOST_TEST
#include <cstdint>
#include <atomic>
static thread_local uint64_t fakeCPU=0;
static std::atomic<unsigned> reads{0};
static uint64_t testCPU(){++reads;return fakeCPU;}
#define FUXI_MTL_THREAD_CPU_NS testCPU
#include "FrameTrace.hpp"
#include <cassert>
#include <thread>
int main(){
 using namespace fuxi;
 FrameSample sample;FrontendTimes frontend;
 {FrameStageTimer off(nullptr,&FrameSample::pipe,&FrameSample::pipeCPU);FrontendTimer disabled(nullptr);}
 assert(reads==0);
 assert(frameCPUDelta(UINT64_MAX,5)==0 && frameCPUDelta(5,UINT64_MAX)==0 && frameCPUDelta(8,7)==0);
 {FrontendTimer outer(&frontend);fakeCPU=10;
  {FrontendTimer nested(&frontend);fakeCPU=30;} // convenience calls counted once
  fakeCPU=50;outer.stop();outer.stop();
 }
 assert(frontend.cpu==50 && reads==2);
 fakeCPU=500; // CA code between calls must not enter the accumulator.
 {FrontendTimer second(&frontend);fakeCPU=520;}
 std::thread worker([&]{fakeCPU=10000;FrontendTimer onOtherThread(&frontend);fakeCPU=10030;});worker.join();
 frontend.copyTo(sample);assert(sample.frontendCPU==100 && sample.frontendWall>0);
 {FrameStageTimer timer(&sample,&FrameSample::pipe,&FrameSample::pipeCPU);fakeCPU+=12;timer.stop();fakeCPU+=300;}
 assert(sample.pipeCPU==12 && sample.pipe>0);
 try{FrameStageTimer timer(&sample,&FrameSample::pipe,&FrameSample::pipeCPU);fakeCPU+=8;throw 1;}catch(int){}
 assert(sample.pipeCPU==20);
 fakeCPU=UINT64_MAX;
 {FrameStageTimer timer(&sample,&FrameSample::pipe,&FrameSample::pipeCPU);fakeCPU=20;}
 assert(sample.pipeCPU==20); // failed clock read must never wrap
 // The overlap path can set currentFrame even with frame tracing disabled.
 {FrameTraceScope scope(&sample);auto before=reads.load();
  auto value=frameKernelCall([]{return 7;});assert(value==7);
  if(!systemTranslationPolicy().frameTrace)assert(reads==before && !sample.kernelIO);
  else assert(reads==before+2 && sample.kernelIO>0);
 }
 puts("PASS: CPU hook, disabled clocks, nesting, inter-call gaps, thread migration, stop/exception/failure");
}
