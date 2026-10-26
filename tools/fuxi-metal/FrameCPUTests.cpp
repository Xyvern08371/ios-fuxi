/* Deterministic bounded CPU sampling controls. Original host test, no GPU. */
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
 {FrameStageTimer off(nullptr,&FrameSample::pipe);FrontendTimer disabled(nullptr);}
 assert(reads==0);
 assert(frameCPUDelta(UINT64_MAX,5)==0 && frameCPUDelta(5,UINT64_MAX)==0 && frameCPUDelta(8,7)==0);
 for(unsigned i=0;i<120;++i){
  FrontendTimer outer(&frontend);fakeCPU+=10;
  {FrontendTimer nested(&frontend);fakeCPU+=20;}
  outer.stop();outer.stop();fakeCPU+=30; // Client work between methods is in the CPU span.
 }
 frontend.copyTo(sample);frontend.copyTo(sample);
 if(systemTranslationPolicy().frameCPU){assert(sample.frontendCPU==7200 && sample.frontendWall>0 && reads==2);}
 else assert(!reads && !sample.frontendCPU && !sample.frontendWall);
 FrontendTimes drawTimes;for(unsigned i=0;i<149;++i){FrontendDrawTimer draw(&drawTimes);}
 FrameSample drawSample;drawTimes.copyTo(drawSample);
 if(systemTranslationPolicy().frameCPU)assert(drawTimes.drawCalls==149 && drawSample.frontendDrawSamples==10 && drawSample.frontendDraw>0);
 else assert(!drawTimes.drawCalls && !drawSample.frontendDrawSamples && !drawSample.frontendDraw);
 auto before=reads.load();
 for(unsigned i=0;i<120;++i){FrameStageTimer timer(&sample,&FrameSample::pipe);fakeCPU+=12;}
 try{FrameStageTimer timer(&sample,&FrameSample::pipe);throw 1;}catch(int){}
 assert(sample.pipe>0 && !sample.pipeCPU && reads==before);
 // Sequential encoding on a different thread never subtracts unrelated clocks.
 FrontendTimes migrated;{FrontendTimer timer(&migrated);}
 std::thread worker([&]{FrontendTimer timer(&migrated);fakeCPU=100000;});worker.join();
 FrameSample other;migrated.copyTo(other);assert(!other.frontendCPU);
 assert(reads==before+(systemTranslationPolicy().frameCPU?1:0));
 before=reads;FrontendTimes failed;fakeCPU=UINT64_MAX;{FrontendTimer timer(&failed);}
 fakeCPU=20;failed.copyTo(other);assert(!other.frontendCPU);
 assert(reads==before+(systemTranslationPolicy().frameCPU?2:0));
 {FrameTraceScope scope(&sample);before=reads;
  auto value=frameKernelCall([]{return 7;});assert(value==7 && reads==before && !sample.kernelIOCPU);
  assert(bool(sample.kernelIO)==systemTranslationPolicy().frameTrace);
 }
 puts("PASS: bounded CPU reads, level 0/1 disabled, 120 nested methods, gaps, migration, stop/exception/failure");
}
