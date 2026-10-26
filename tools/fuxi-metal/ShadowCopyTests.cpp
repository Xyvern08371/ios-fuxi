/* Full/ranged snapshots through production Vulkan submit/retirement and pixels. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
int main(){
 using namespace fuxi;Backend backend;ShadowPool pool;constexpr size_t length=4*1024*1024;
 auto *buffer=backend.allocate(length),*read=backend.allocate(16*16*4);auto *target=backend.allocateTexture(80,4,16,16);auto *pipeline=backend.compilePipeline(nullptr,"control",nullptr,"control");
 bool old=getenv("SHADOW_OLD_ADAPTER");assert(pipeline->shadowWindows[1][0].kind==(old?0u:1u));
 std::vector<uint8_t> cpu(length),reference,actual;uint64_t copied=0;
 auto run=[&](bool full,unsigned frame){
  size_t offset=65536+frame*256;float color[]={.25f,.5f,.75f,1};std::fill(cpu.begin(),cpu.end(),0);memcpy(cpu.data()+offset,color,sizeof(color));memset(buffer->contents,3,length);
  ShadowRanges ranges(length);ranges.binding(full?ShadowWindow{}:pipeline->shadowWindows[1][0],offset);ShadowSnapshot snapshot;snapshot.add(buffer->contents,cpu.data(),ranges);snapshot.capture(pool);
  assert(snapshot.bytes()==(full || old?length:16));std::fill(cpu.begin(),cpu.end(),255);
  Operation draw{};draw.texture=target;draw.pipeline=pipeline;draw.vertices=6;draw.viewport={0,16,16,-16,0,1};draw.scissor={{0,0},{16,16}};draw.buffers[1][0]={buffer,offset,false};
  std::function<void()> retire;backend.submit({draw},{},systemTranslationPolicy().overlap?&retire:nullptr,[&]{copied+=snapshot.apply(pool);});if(retire)retire();
  assert(((uint8_t *)buffer->contents)[0]==(full || old?0:3));assert(((uint8_t *)buffer->contents)[offset]==((uint8_t *)color)[0]);
  Operation get{};get.texture=target;get.dst=read;backend.submit({get});return std::vector<uint8_t>((uint8_t *)read->contents,(uint8_t *)read->contents+16*16*4);
 };
 for(unsigned frame=0;frame<3;++frame){reference=run(true,frame);actual=run(false,frame);assert(actual==reference);for(size_t i=0;i<actual.size();i+=4){assert(actual[i]==191 && actual[i+1]==128 && actual[i+2]==64 && actual[i+3]==255);}}
 assert(copied==3*(length+(old?length:16)));backend.destroy(pipeline);backend.destroy(target);backend.destroy(read);backend.destroy(buffer);
 printf("PASS: lavapipe full/ranged pixel equality, post-commit writes, prepare/retirement (%s adapter, %s submit)\n",old?"old/full":"window",systemTranslationPolicy().overlap?"overlap":"synchronous");
}
