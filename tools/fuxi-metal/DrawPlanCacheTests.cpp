/* Exact plans, bounded replacement and deterministic pipeline address reuse; no GPU. */
#define FUXI_MTL_HOST_TEST
#include "DrawPlanCache.hpp"
#include <cassert>
#include <cstdio>
#include <new>
struct Pipeline {unsigned tag;};
int main(){
 using namespace fuxi;DrawPlanCache<Pipeline> cache;Pipeline a{1},b{2},variant{3};unsigned calls=0;
 auto uncached=[&](Pipeline *p,const DrawInput &input,Pipeline *&resolved,DrawOutput &out){
  ++calls;resolved=p==&a?&variant:p;memset(&out,int(p->tag+input.count),sizeof(out));out.count=input.count;out.first=input.start;out.indexOffset=input.indexOffset;
 };
 auto draw=[&](Pipeline *p,const DrawInput &input,Pipeline *&resolved,DrawOutput &out){if(!cache.find(p,input,resolved,out)){uncached(p,input,resolved,out);cache.insert(p,input,resolved,out);}};
 DrawInput inputs[2]{};inputs[0].count=3;inputs[1].count=6;inputs[1].buffers[1][15]={256,16,1};
 for(unsigned i=0;i<4;++i){auto *p=i%2?&b:&a;auto &input=inputs[i%2];Pipeline *resolved=nullptr,*expected=nullptr;DrawOutput out{},reference{};
  draw(p,input,resolved,out);assert(calls==(i?2u:1u));unsigned saved=calls;uncached(p,input,expected,reference);calls=saved;assert(resolved==expected && !memcmp(&out,&reference,sizeof(out)));
 }
 // The same requested pipeline can interleave complete inputs too.
 Pipeline *resolved=nullptr;DrawOutput out{};draw(&a,inputs[1],resolved,out);assert(calls==3);draw(&a,inputs[0],resolved,out);draw(&a,inputs[1],resolved,out);assert(calls==3);
 cache.invalidate(&variant);assert(!cache.find(&a,inputs[0],resolved,out) && !cache.find(&a,inputs[1],resolved,out));assert(cache.find(&b,inputs[1],resolved,out));
 cache.invalidate(&b);assert(!cache.find(&b,inputs[1],resolved,out));
 alignas(Pipeline) unsigned char storage[sizeof(Pipeline)];auto *p=new(storage) Pipeline{4};
 draw(p,inputs[0],resolved,out);assert(calls==4);cache.invalidate(p);p->~Pipeline();p=new(storage) Pipeline{9};
 assert(!cache.find(p,inputs[0],resolved,out));draw(p,inputs[0],resolved,out);assert(calls==5 && resolved==p);DrawOutput reference{};Pipeline *expected=nullptr;uncached(p,inputs[0],expected,reference);assert(!memcmp(&out,&reference,sizeof(out)));cache.invalidate(p);p->~Pipeline();
 DrawPlanCache<Pipeline> bounded;DrawInput input{};DrawOutput value{};
 for(unsigned i=0;i<33;++i){input.count=i;value.count=i;bounded.insert(&a,input,&variant,value);}
 input.count=0;assert(!bounded.find(&a,input,resolved,out));
 for(unsigned i=1;i<33;++i){input.count=i;assert(bounded.find(&a,input,resolved,out) && resolved==&variant && out.count==i);}
 puts("PASS: A/B hits, complete-input interleaving, byte-identical plans, requested/resolved invalidation, address reuse and bounded eviction");
}
