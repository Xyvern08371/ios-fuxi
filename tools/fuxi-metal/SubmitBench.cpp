/* 120 authored sampled draws; CPU prepare attribution excludes fence wait/JIT. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include "M7Reference.h"
int main(int argc,char **argv){
 assert(argc==1 || argc==2);
 using namespace fuxi;
 Backend b;auto baseline=liveMemoryObjects.load();auto *src=b.allocateTexture(80,5,16,16),*other=b.allocateTexture(80,5,16,16),*dst=b.allocateTexture(80,5,16,16);auto *data=b.allocate(1024),*read=b.allocate(1024);
 Operation upload{};upload.texture=src;upload.src=data;upload.upload=true;memset(data->contents,91,1024);b.submit({upload});upload.texture=other;memset(data->contents,37,1024);b.submit({upload});
 void *library=nullptr;Buffer *vertices=nullptr,*ones=nullptr;CompileOptions options{};
 if(argc==2){
  std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> air{std::istreambuf_iterator<char>(in),{}};
  char why[512]{};library=Translator::get().open(air.data(),air.size(),why,sizeof(why));assert(library);
  options.attributes[0]={29,0,0};options.attributes[1]={29,8,0};options.attributes[2]={9,16,0};options.strides[0]=20;
  options.constantCount[1]=1;options.constants[1][0].type=53;options.constants[1][0].size=1;options.constants[1][0].bytes[0]=1;
  vertices=b.allocate(sizeof(fuxiM7Vertices));memcpy(vertices->contents,fuxiM7Vertices,sizeof(fuxiM7Vertices));
  ones=b.allocate(16);for(unsigned c=0;c<4;++c)((float *)ones->contents)[c]=1;
 }
 auto *p=b.compilePipeline(library,library?"fuxi_layer_vertex":"control",library,library?"fuxi_layer_fragment":"control",options);auto *sam=b.allocateSampler(false,false,1);
 Operation draw{};draw.encoderID=1;draw.texture=dst;draw.pipeline=p;draw.samples[0]=src;draw.samplers[0]=sam;draw.vertices=6;draw.viewport={0,16,16,-16,0,1};draw.scissor={{0,0},{16,16}};draw.sampleCount[0]=1;
 if(library){draw.vertices=4;draw.topology=Topology::TriangleStrip;draw.buffers[0][0]={vertices,0,false};draw.buffers[0][2]={ones,0,true};draw.buffers[1][1]={ones,0,true};}
 bool encoderBoundaries=getenv("MERGE_BENCH_ENCODERS")!=nullptr;
 std::vector<Operation> draws(120,draw);for(unsigned i=0;i<120;++i){draws[i].samples[0]=(i%2)?other:src;draws[i].load=i?1:2;if(encoderBoundaries)draws[i].encoderID=i+1;}
 for(unsigned i=0;i<3;++i)b.submit(draws);
 FrameTraceWindow window;uint64_t ns=0,descriptor=0,record=0,objects=0,sets=0,passes=0,merged=0,gpu=0;
 auto db=b.submitDescriptorsNanos.load(),rb=b.submitRecordNanos.load(),ob=b.submitObjectsNanos.load(),sb=frameCounts.descriptors;
 for(unsigned i=0;i<40;++i){FrameSample sample;{FrameTraceScope scope(&sample);b.submit(draws);}bool mergedRun=systemTranslationPolicy().merge && (!encoderBoundaries || (systemTranslationPolicy().perf&2));assert(sample.draws==120 && sample.passes==(mergedRun?1u:120u) && sample.mergedDraws==(mergedRun?119u:0u));if(systemTranslationPolicy().merge && encoderBoundaries)assert(sample.mergeSplits[size_t(MergeSplit::encoder)]==((systemTranslationPolicy().perf&2)?0u:119u));window.add(sample);ns+=sample.backendSubmit;passes+=sample.passes;merged+=sample.mergedDraws;gpu+=sample.gpuExec;}
 descriptor=b.submitDescriptorsNanos-db;record=b.submitRecordNanos-rb;objects=b.submitObjectsNanos-ob;sets=frameCounts.descriptors-sb;
 Operation get{};get.texture=dst;get.dst=read;b.submit({get});for(unsigned i=0;i<1024;++i)assert(((uint8_t *)read->contents)[i]==37);
 printf("BENCH fast=%u frames=40 draws=4800 submit_us/draw=%.3f descriptors_us/draw=%.3f objects_us/draw=%.3f record_us/draw=%.3f sets/frame=%.2f\n",unsigned(systemTranslationPolicy().submitFast),ns/4800e3,descriptor/4800e3,objects/4800e3,record/4800e3,sets/40.0);
 printf("MERGE_BENCH enabled=%u fast=%u passes/frame=%.2f merged/frame=%.2f cpu_ms/frame=%.3f gpu_ms/frame=%.3f\n",unsigned(systemTranslationPolicy().merge),unsigned(systemTranslationPolicy().submitFast),passes/40.0,merged/40.0,ns/40e6,gpu/40e6);
 if(systemTranslationPolicy().merge)printf("BOUNDARY_BENCH encoders/frame=%u cpu_us/draw=%.3f gpu_us/draw=%.3f\n",encoderBoundaries?120u:1u,ns/4800e3,gpu/4800e3);
 printf("CPUOPT_BENCH draws/frame=120 median_submit_ms=%.3f median_pipe_ms=%.3f planner=%s\n",window.percentile(&FrameSample::backendSubmit,50)/1e6,window.percentile(&FrameSample::pipe,50)/1e6,library?"external":"fixture");
 // Both flights keep pipelines, samplers, views, attachments and samples alive
 // after all caller references and the object cache have been dropped.
 std::function<void()> first,second;b.submit(draws,{},&first);b.submit(draws,{},&second);
 b.destroy(vertices);b.destroy(ones);
 b.destroy(p);b.dropPipelineCacheForTest();b.destroy(sam);b.destroy(src);b.destroy(other);b.destroy(dst);b.destroy(data);b.destroy(read);
 assert(liveMemoryObjects>baseline);first();assert(liveMemoryObjects>baseline);second();b.close();if(library)Translator::get().close(library);assert(liveMemoryObjects==baseline);
 puts("PASS: sampled flights retain texture/view/framebuffer/pipeline/sampler after client and cache destruction");
}
