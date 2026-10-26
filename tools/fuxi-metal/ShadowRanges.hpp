/* Commit-owned packed snapshots; unknown access preserves the full-copy path. */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>
#include "VertexProfile.hpp"
namespace fuxi {
struct ShadowWindow {uint32_t kind=0,reserved=0;int64_t start=0;uint64_t size=0;}; // 0 whole, 1 proven argument window, 2 no argument access
static_assert(sizeof(ShadowWindow)==24,"shadow window ABI");
struct ShadowInterval {uint64_t offset,size;};
class ShadowRanges {
 uint64_t length;bool whole=false;std::vector<ShadowInterval> intervals;
public:
 explicit ShadowRanges(uint64_t n):length(n){}
 void full(){whole=true;intervals.clear();}
 void add(uint64_t offset,uint64_t size){
  if(whole)return;if(offset>length || size>length-offset){full();return;}if(!size)return;
  uint64_t end=offset+size;auto first=intervals.begin();while(first!=intervals.end() && first->offset+first->size<offset)++first;
  auto last=first;while(last!=intervals.end() && last->offset<=end){offset=std::min(offset,last->offset);end=std::max(end,last->offset+last->size);++last;}
  if(first==last)intervals.insert(first,{offset,end-offset});else {first->offset=offset;first->size=end-offset;intervals.erase(first+1,last);}
 }
 void binding(const ShadowWindow &w,uint64_t offset){
  if(w.kind==2)return;if(w.kind!=1){full();return;}
  if(w.start<0){uint64_t delta=uint64_t(-(w.start+1))+1;if(delta>offset){full();return;}offset-=delta;}
  else {if(uint64_t(w.start)>length || offset>length-uint64_t(w.start)){full();return;}offset+=uint64_t(w.start);}
  add(offset,w.size);
 }
 void vertex(uint64_t offset,uint32_t first,uint32_t count,bool indexed,uint32_t stride,uint32_t attribute,uint32_t format){
  uint32_t bytes=vertexFormatInfo(format).bytes;if(indexed || !count || first>UINT32_MAX-(count-1) || !vertexFetchFits(length,offset,first+count-1,stride,attribute,format)){full();return;}
  add(offset+uint64_t(first)*stride+attribute,uint64_t(count-1)*stride+bytes);
 }
 template<class F> void each(F &&f) const {if(whole){if(length)f(ShadowInterval{0,length});}else for(auto r:intervals)f(r);}
 uint64_t bytes() const {uint64_t n=0;each([&](auto r){n+=r.size;});return n;}
 bool isFull() const{return whole;}
};
struct ShadowStorage {std::unique_ptr<uint8_t[]> data;size_t capacity=0;};
class ShadowPool {
 std::mutex mutex;std::array<ShadowStorage,2> cached;
public:
 ShadowStorage take(size_t bytes){
  {std::lock_guard<std::mutex> lock(mutex);for(auto &s:cached)if(s.data && s.capacity>=bytes)return std::move(s);}
  return {std::unique_ptr<uint8_t[]>(new uint8_t[bytes]),bytes};
 }
 void put(ShadowStorage storage){
  if(!storage.data)return;std::lock_guard<std::mutex> lock(mutex);
  for(auto &s:cached)if(!s.data){s=std::move(storage);return;}
  auto &s=cached[cached[0].capacity<cached[1].capacity?0:1];if(storage.capacity>s.capacity)s=std::move(storage);
 }
};
struct ShadowSegment {void *destination;const void *source;uint64_t offset,size,packed;};
class ShadowSnapshot {
 ShadowStorage storage;std::vector<ShadowSegment> segments;size_t size=0;
public:
 void add(void *destination,const void *source,const ShadowRanges &ranges){ranges.each([&](auto r){segments.push_back({destination,source,r.offset,r.size,size});size+=r.size;});}
 size_t bytes() const{return size;}
 void capture(ShadowPool &pool){if(!size)return;storage=pool.take(size);for(auto r:segments)memcpy(storage.data.get()+r.packed,(const uint8_t *)r.source+r.offset,r.size);}
 size_t apply(ShadowPool &pool){for(auto r:segments)memcpy((uint8_t *)r.destination+r.offset,storage.data.get()+r.packed,r.size);size_t copied=size;release(pool);return copied;}
 void release(ShadowPool &pool){pool.put(std::move(storage));segments.clear();size=0;}
};
}
