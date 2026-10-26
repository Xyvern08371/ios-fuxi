/* Host controls use the same tracker, packer and pool as the frontend. */
#include "ShadowRanges.hpp"
#include <cassert>
#include <cstdio>
#include <limits>
#include <thread>
using namespace fuxi;
int main(){
 ShadowRanges merged(1024);merged.add(80,20);merged.add(20,20);merged.add(40,40);merged.add(15,5);merged.add(90,5);assert(merged.bytes()==85);
 unsigned intervals=0;merged.each([&](auto r){assert(r.offset==15 && r.size==85);++intervals;});assert(intervals==1);
 merged.add(1024,0);assert(merged.bytes()==85);merged.add(UINT64_MAX,2);assert(merged.isFull() && merged.bytes()==1024);
 // A byte-mask oracle checks arbitrary insertion order, overlap and adjacency.
 ShadowRanges random(1024);std::array<bool,1024> mask{};uint32_t seed=123;
 for(unsigned i=0;i<300;++i){seed=1664525*seed+1013904223;unsigned offset=seed%1024;seed=1664525*seed+1013904223;unsigned size=seed%33;size=std::min(size,1024-offset);random.add(offset,size);for(unsigned j=offset;j<offset+size;++j)mask[j]=true;
  std::array<bool,1024> covered{};random.each([&](auto r){for(uint64_t j=r.offset;j<r.offset+r.size;++j)covered[j]=true;});assert(covered==mask);
 }
 ShadowRanges negative(1024);negative.binding({1,0,-8,24},128);assert(negative.bytes()==24);negative.each([](auto r){assert(r.offset==120);});
 negative.binding({1,0,std::numeric_limits<int64_t>::min(),1},0);assert(negative.isFull());
 for(auto w:{ShadowWindow{},ShadowWindow{3,0,0,4},ShadowWindow{1,0,1000,32},ShadowWindow{1,0,0,UINT64_MAX}}){ShadowRanges r(1024);r.binding(w,8);assert(r.isFull());}
 ShadowRanges unused(1024);unused.binding({2,0,0,0},0);assert(!unused.bytes());
 ShadowRanges vertices(4096);vertices.vertex(256,2,6,false,32,8,29);assert(vertices.bytes()==168);vertices.each([](auto r){assert(r.offset==328);});
 vertices.vertex(0,0,3,true,32,0,29);assert(vertices.isFull());
 ShadowRanges badVertex(1024);badVertex.vertex(0,UINT32_MAX,3,false,32,0,29);assert(badVertex.isFull());
 ShadowPool pool;auto storage=pool.take(1024);auto *address=storage.data.get();pool.put(std::move(storage));storage=pool.take(512);assert(storage.data.get()==address);pool.put(std::move(storage));
 constexpr size_t ringBytes=4*1024*1024,draws=130,needed=draws*256;
 std::vector<uint8_t> cpu(ringBytes),gpu(ringBytes,0);ShadowSnapshot first,second;
 auto ranges=[&](unsigned frame){ShadowRanges r(ringBytes);for(unsigned i=0;i<draws;++i){uint64_t offset=65536+frame*needed+i*256;r.vertex(offset,0,6,false,32,0,31);r.vertex(offset,0,6,false,32,16,31);r.binding({1,0,0,64},offset+192);}assert(r.bytes()==needed);return r;};
 // CPU writes both inside and outside the proven window after capture.
 for(unsigned frame=0;frame<3;++frame){std::fill(cpu.begin(),cpu.end(),uint8_t(19+frame));auto r=ranges(frame);first.add(gpu.data(),cpu.data(),r);first.capture(pool);assert(first.bytes()==needed);std::fill(cpu.begin(),cpu.end(),77);
  std::fill(gpu.begin(),gpu.end(),3);assert(first.apply(pool)==needed);for(size_t i=0;i<ringBytes;++i)assert(gpu[i]==(i>=65536+frame*needed && i<65536+(frame+1)*needed?19+frame:3));
 }
 // Pending snapshots never share storage, even when the same ring is reused.
 auto r=ranges(0);std::fill(cpu.begin(),cpu.end(),11);first.add(gpu.data(),cpu.data(),r);first.capture(pool);std::fill(cpu.begin(),cpu.end(),22);second.add(gpu.data(),cpu.data(),r);second.capture(pool);std::fill(cpu.begin(),cpu.end(),33);
 assert(first.apply(pool)==needed && gpu[65536]==11);assert(second.apply(pool)==needed && gpu[65536]==22);
 ShadowRanges full(ringBytes);full.binding({},123);first.add(gpu.data(),cpu.data(),full);first.capture(pool);cpu.back()=99;assert(first.apply(pool)==ringBytes && gpu.back()==33);
 std::array<uint8_t,128> otherCPU{},otherGPU{};otherCPU.fill(42);ShadowRanges other(128);other.add(48,16);std::fill(cpu.begin(),cpu.end(),14);
 first.add(gpu.data(),cpu.data(),r);first.add(otherGPU.data(),otherCPU.data(),other);first.capture(pool);otherCPU.fill(99);assert(first.apply(pool)==needed+16 && gpu[65536]==14);
 for(unsigned i=0;i<128;++i)assert(otherGPU[i]==(i>=48 && i<64?42:0));
 // Return/take from separate commit and submit threads uses the pool lock.
 std::thread worker([&]{for(unsigned i=0;i<100;++i){auto s=pool.take(2048);assert(s.data && s.capacity>=2048);pool.put(std::move(s));}});
 for(unsigned i=0;i<100;++i){auto s=pool.take(1024);assert(s.data && s.capacity>=1024);pool.put(std::move(s));}worker.join();
 printf("PASS: shadow ranges, immutable commits, full fallback, two flights, pool reuse; CA-like ring bytes/frame before=%zu after=%zu (snapshot and backend copy each)\n",ringBytes,needed);
}
