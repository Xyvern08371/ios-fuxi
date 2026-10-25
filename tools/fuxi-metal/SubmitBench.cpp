/* 140 authored sampled draws; CPU prepare attribution excludes fence wait/JIT. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
int main(){
 using namespace fuxi;
 Backend b;auto baseline=liveMemoryObjects.load();auto *src=b.allocateTexture(80,5,16,16),*other=b.allocateTexture(80,5,16,16),*dst=b.allocateTexture(80,5,16,16);auto *data=b.allocate(1024),*read=b.allocate(1024);
 Operation upload{};upload.texture=src;upload.src=data;upload.upload=true;memset(data->contents,91,1024);b.submit({upload});upload.texture=other;memset(data->contents,37,1024);b.submit({upload});
 auto *p=b.compilePipeline(nullptr,"control",nullptr,"control");auto *sam=b.allocateSampler(false,false,1);
 Operation draw{};draw.texture=dst;draw.pipeline=p;draw.samples[0]=src;draw.samplers[0]=sam;draw.vertices=6;draw.viewport={0,16,16,-16,0,1};draw.scissor={{0,0},{16,16}};draw.sampleCount[0]=1;
 std::vector<Operation> draws(140,draw);for(unsigned i=0;i<140;++i){draws[i].samples[0]=(i%2)?other:src;draws[i].load=i?1:2;}
 for(unsigned i=0;i<3;++i)b.submit(draws);
 uint64_t ns=0,descriptor=0,record=0,objects=0,sets=0;
 auto db=b.submitDescriptorsNanos.load(),rb=b.submitRecordNanos.load(),ob=b.submitObjectsNanos.load(),sb=frameCounts.descriptors;
 for(unsigned i=0;i<40;++i){FrameSample sample;{FrameTraceScope scope(&sample);b.submit(draws);}assert(sample.draws==140);ns+=sample.backendSubmit;}
 descriptor=b.submitDescriptorsNanos-db;record=b.submitRecordNanos-rb;objects=b.submitObjectsNanos-ob;sets=frameCounts.descriptors-sb;
 Operation get{};get.texture=dst;get.dst=read;b.submit({get});for(unsigned i=0;i<1024;++i)assert(((uint8_t *)read->contents)[i]==37);
 printf("BENCH fast=%u frames=40 draws=5600 submit_us/draw=%.3f descriptors_us/draw=%.3f objects_us/draw=%.3f record_us/draw=%.3f sets/frame=%.2f\n",unsigned(systemTranslationPolicy().submitFast),ns/5600e3,descriptor/5600e3,objects/5600e3,record/5600e3,sets/40.0);
 // Both flights keep pipelines, samplers, views, attachments and samples alive
 // after all caller references and the object cache have been dropped.
 std::function<void()> first,second;b.submit(draws,{},&first);b.submit(draws,{},&second);
 b.destroy(p);b.dropPipelineCacheForTest();b.destroy(sam);b.destroy(src);b.destroy(other);b.destroy(dst);b.destroy(data);b.destroy(read);
 assert(liveMemoryObjects>baseline);first();assert(liveMemoryObjects>baseline);second();b.close();assert(liveMemoryObjects==baseline);
 puts("PASS: sampled flights retain texture/view/framebuffer/pipeline/sampler after client and cache destruction");
}
