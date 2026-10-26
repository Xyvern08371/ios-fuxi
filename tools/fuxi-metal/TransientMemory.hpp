/* Quiescent, non-imported allocations only. Caller serializes and owns retirement. */
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace fuxi {
inline std::atomic<uint64_t> texturePoolHits{0},texturePoolMisses{0},texturePoolTrims{0},texturePoolBytes{0};
struct MemoryKey {
 uint64_t bytes{},alignment{};uint32_t format{},usage{},type{};
 bool operator==(const MemoryKey &b) const{return bytes==b.bytes && alignment==b.alignment && format==b.format && usage==b.usage && type==b.type;}
};
template<class Handle> class TransientMemory {
 struct Entry {MemoryKey key{};Handle memory{};uint64_t returned{};};
 std::array<Entry,32> entries{};size_t used=0;
public:
 static constexpr size_t budget=64u*1024*1024;
 static constexpr uint64_t idleNanos=5ull*1000*1000*1000;
 size_t bytes() const{return used;}
 Handle take(const MemoryKey &key){
  for(auto &e:entries)if(e.memory && e.key==key){auto h=e.memory;e.memory={};used-=key.bytes;texturePoolBytes-=key.bytes;++texturePoolHits;return h;}
  ++texturePoolMisses;return {};
 }
 bool put(const MemoryKey &key,Handle memory,uint64_t now){
  if(!memory || !key.bytes || key.bytes>budget-used)return false;
  for(auto &e:entries)if(!e.memory){e={key,memory,now};used+=key.bytes;texturePoolBytes+=key.bytes;return true;}
  return false;
 }
 template<class Free> void trim(uint64_t now,bool all,Free free){
  for(auto &e:entries)if(e.memory && (all || (now>=e.returned && now-e.returned>=idleNanos))){
   free(e.memory,e.key.bytes);used-=e.key.bytes;texturePoolBytes-=e.key.bytes;e={};++texturePoolTrims;
  }
 }
};
}
