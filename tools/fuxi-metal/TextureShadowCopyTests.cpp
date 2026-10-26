/* Authored sampled textures through production repitch, snapshots and retirement. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
using namespace fuxi;
static void aliasReadback(Backend &b){
 auto *buffer=b.allocate(65536);auto *t=b.linearTexture(buffer,80,5,16,8,0,64);assert(!t->shadow && t->borrowedMemory);
 for(unsigned load:{2u,1u}){Operation clear{};clear.texture=t;clear.clearOnly=true;clear.load=load;clear.clear[0]=.25f;clear.clear[1]=.5f;clear.clear[2]=.75f;clear.clear[3]=1;
  std::function<void()> retire;b.submit({clear},{},systemTranslationPolicy().overlap?&retire:nullptr);if(retire)retire();
  auto *bytes=(uint8_t *)buffer->contents;for(unsigned i=0;i<16*8;++i)assert(bytes[i*4]==191 && bytes[i*4+1]==128 && bytes[i*4+2]==64 && bytes[i*4+3]==255);
 }
 b.destroy(t);b.destroy(buffer);
}
int main(){
 Backend b;ShadowPool pool;auto baseline=b.resourceBytes();aliasReadback(b);
 auto *targetBuffer=b.allocate(65536);auto *target=b.linearTexture(targetBuffer,80,5,16,16,0,64);assert(!target->shadow && target->borrowedMemory);auto *out=b.allocate(25*16*16*4);auto *sampler=b.allocateSampler(false,false);
 CompileOptions options;auto *pipeline=b.compilePipeline(nullptr,"control",nullptr,"control",options);
 struct Source {Buffer *buffer;Texture *texture;std::vector<uint8_t> cpu;std::unique_ptr<ShadowCache> cache=std::make_unique<ShadowCache>();};std::vector<Source> sources;
 // Four supplied mismatch shapes, repeated to 25 allocations; allocation sizes are synthetic.
 const uint32_t shapes[][4]={{621,358,768,1},{230,186,480,30},{138,138,256,1},{635,147,768,1}};
 uint64_t before=0,after=0,repitched=0;
 for(unsigned i=0;i<25;++i){auto &d=shapes[i%4];size_t length=638137+(i==24?19:0);auto *buffer=b.allocate(length);auto *texture=b.linearTexture(buffer,d[3],1,d[0],d[1],256,d[2]);assert(texture->shadow);
  sources.push_back({buffer,texture,std::vector<uint8_t>(length)});before+=length;ShadowRanges r(length);r.texture(256,d[2],d[0],d[1],b.texelBytes(d[3]));assert(!r.isFull());after+=r.bytes();repitched+=uint64_t(d[0])*d[1]*b.texelBytes(d[3]);
 }
 assert(before==15953444 && after==3346977 && repitched*5==13719600);
 auto run=[&](bool full,unsigned frame,bool fallback,bool cached=false,bool repeat=false){
  ShadowSnapshot snapshot;uint64_t bytes=0;std::vector<Operation> ops;
  for(unsigned i=0;i<sources.size();++i){auto &s=sources[i];auto *t=s.texture;auto pixel=b.texelBytes(t->format);std::fill(s.cpu.begin(),s.cpu.end(),0xa7);if(!repeat)memset(s.buffer->contents,3,s.buffer->length);
   for(unsigned y=0;y<t->height;++y)for(unsigned x=0;x<t->width;++x)for(unsigned c=0;c<pixel;++c)s.cpu[256+y*t->backingRow+x*pixel+c]=uint8_t(i*13+frame*47+x*3+y*5+c*61);
   ShadowRanges r(s.buffer->length);r.texture(256,t->backingRow,t->width,t->height,pixel);if(full || fallback)r.full(fallback?ShadowFullReason::useResource:ShadowFullReason::other);bytes+=r.bytes();if(cached)snapshot.addCached(s.buffer->contents,s.cpu.data(),r,*s.cache);else snapshot.add(s.buffer->contents,s.cpu.data(),r);
   Operation draw{};draw.texture=target;draw.pipeline=pipeline;draw.samples[0]=t;draw.samplers[0]=sampler;draw.viewport={0,16,16,-16,0,1};draw.scissor={{0,0},{16,16}};
   // Five draws per source model ~125 CA draws. Distinct encoder IDs prevent cross-pass merging.
   draw.encoderID=i+1;for(unsigned j=0;j<5;++j){draw.load=j?1:2;ops.push_back(draw);}
   Operation read{};read.texture=target;read.dst=out;read.to=i*16*16*4;ops.push_back(read);
  }
  snapshot.capture(pool);assert(snapshot.bytes()==bytes);if(repeat)assert(!snapshot.copiedBytes());for(auto &s:sources)std::fill(s.cpu.begin(),s.cpu.end(),255);
  FrameSample trace;std::function<void()> retire;uint64_t copied=0;
  {FrameTraceScope scope(&trace);if(cached){b.submit(ops,{},systemTranslationPolicy().overlap?&retire:nullptr,{},&snapshot,&pool);copied=trace.shadowCopyBytes;}else b.submit(ops,{},systemTranslationPolicy().overlap?&retire:nullptr,[&]{copied=snapshot.apply(pool);});}if(retire)retire();
  // Check every source so equality cannot hide an earlier texture's corruption.
  auto *p=(uint8_t *)out->contents;for(size_t i=0;i<sources.size();++i){auto *t=sources[i].texture;
   for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){unsigned sx=x*(t->width-1)/15,sy=y*(t->height-1)/15;uint8_t r=uint8_t(i*13+frame*47+sx*3+sy*5),g=t->format==30?uint8_t(r+61):0;size_t at=i*16*16*4+(y*16+x)*4;assert(p[at]==0 && p[at+1]==g && p[at+2]==r && p[at+3]==255);}
   assert(((uint8_t *)sources[i].buffer->contents)[0]==(full || fallback?0xa7:3));
  }
  assert(!memcmp(targetBuffer->contents,p+out->length-16*16*4,16*16*4));
  assert(copied==(repeat?0:bytes) && trace.textureRepitchedBytes==repitched*5);return std::vector<uint8_t>(p,p+out->length);
 };
 for(unsigned frame=0;frame<3;++frame){auto reference=run(true,frame,false);assert(run(false,frame,false)==reference);assert(run(false,frame,true)==reference);assert(run(false,frame,false,true)==reference);assert(run(false,frame,false,true,true)==reference);}
 printf("PASS: 25 linear textures/125 draws, CPU writes between frames, frozen post-commit bytes, sampled pitch mismatch full/bounded/fallback pixels; per-stage CPU bytes %llu -> %llu; GPU repitch bytes unchanged %llu (%s)\n",(unsigned long long)before,(unsigned long long)after,(unsigned long long)(repitched*5),systemTranslationPolicy().overlap?"overlap":"synchronous");
 for(auto &s:sources){b.destroy(s.texture);b.destroy(s.buffer);}b.destroy(pipeline);b.dropPipelineCacheForTest();b.destroy(sampler);b.destroy(out);b.destroy(target);b.destroy(targetBuffer);b.trimMemory();assert(b.resourceBytes()==baseline);
}
