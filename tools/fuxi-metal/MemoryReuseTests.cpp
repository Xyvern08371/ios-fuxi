/* Actual Vulkan images, fence pinning, bounded cache, pressure and loss. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
int main(int argc,char **){
 using namespace fuxi;Backend backend;auto objects=liveMemoryObjects.load();
 if(argc>1){
  auto *idle=backend.allocateTexture(80,5,16,16);backend.destroy(idle);auto cached=texturePoolBytes.load();assert(cached);
  auto *t=backend.allocateTexture(80,5,64,64);auto *buffer=backend.allocate(16384);
  Operation upload{};upload.texture=t;upload.src=buffer;upload.upload=true;std::function<void()> retire;backend.submit({upload},{},&retire);
  bool failed=false;try{retire();}catch(const std::runtime_error&){failed=true;}assert(failed && backend.poisoned);
  auto held=liveMemoryObjects.load();backend.trimMemory();assert(texturePoolBytes==cached);
  failed=false;try{backend.allocateTexture(80,5,16,16);}catch(const std::runtime_error&){failed=true;}assert(failed);
  backend.destroy(t);backend.destroy(buffer);backend.close();assert(liveMemoryObjects==held);
  puts("PASS: loss/timeout retains both idle pool and unknown DMA, refuses reuse/trim");return 0;
 }
 auto *a=backend.allocateTexture(80,5,16,16);auto memory=a->memory;auto bytes=a->charged;auto *up=backend.allocate(1024),*out=backend.allocate(1024);memset(up->contents,0x43,1024);
 Operation upload{};upload.texture=a;upload.src=up;upload.upload=true;std::function<void()> retire;
 auto before=frameCounts.allocations;backend.submit({upload},{},&retire);
 Operation clear{};clear.texture=a;clear.clearOnly=true;clear.clear[3]=1;std::function<void()> lastRetire;backend.submit({clear},{},&lastRetire);backend.destroy(a);
 auto misses=texturePoolMisses.load();auto *b=backend.allocateTexture(80,5,16,16);assert(b->memory!=memory && texturePoolMisses==misses+1);
 retire();auto *stillLive=backend.allocateTexture(80,5,16,16);assert(stillLive->memory!=memory);
 lastRetire();auto hits=texturePoolHits.load();auto allocated=frameCounts.allocations;auto *c=backend.allocateTexture(80,5,16,16);assert(c->memory==memory && texturePoolHits==hits+1 && frameCounts.allocations==allocated && !c->defined && c->layout==VK_IMAGE_LAYOUT_UNDEFINED);
 upload.texture=c;Operation read{};read.texture=c;read.dst=out;backend.submit({upload,read});assert(!memcmp(up->contents,out->contents,1024));
 backend.destroy(b);backend.destroy(c);backend.destroy(stillLive);assert(backend.resourceBytes()==bytes*3+up->charged+out->charged);
 backend.trimMemoryForTest(UINT64_MAX);assert(backend.resourceBytes()==up->charged+out->charged);
 std::vector<Texture *> textures;for(unsigned i=0;i<66;++i)textures.push_back(backend.allocateTexture(80,5,16,16));for(auto *t:textures)backend.destroy(t);
 assert(texturePoolBytes==bytes*64);backend.trimMemory();assert(!texturePoolBytes);
 // Nearby sizes share a class: a 17x16 texture reuses the pooled block without a new allocation.
 {auto *p=backend.allocateTexture(80,5,16,16);auto *q=backend.allocateTexture(80,5,17,16);auto m=p->memory;auto classBytes=p->charged;backend.destroy(p);auto allocs=frameCounts.allocations,h2=texturePoolHits.load();auto *r=backend.allocateTexture(80,5,17,16);
  assert(r->memory==m && texturePoolHits==h2+1 && frameCounts.allocations==allocs && r->charged==classBytes && classBytes>=r->allocated);backend.destroy(q);backend.destroy(r);backend.trimMemory();}
 // Idle storage still counts toward the device budget; imports must never donate memory.
 auto *cached=backend.allocateTexture(80,5,16,16);backend.destroy(cached);assert(texturePoolBytes==bytes);
 backend.trimMemory();backend.destroy(up);backend.destroy(out);assert(!backend.resourceBytes());backend.close();assert(liveMemoryObjects==objects);
 assert(frameCounts.allocations>before);
 printf("PASS: image memory reuse after fence only, new-image layout/defined state, pixels, cap/idle/pressure/charge/close; Operation=%zu BufferBind=%zu bindings=%zu\n",sizeof(Operation),sizeof(BufferBind),sizeof(Operation::buffers));
}
