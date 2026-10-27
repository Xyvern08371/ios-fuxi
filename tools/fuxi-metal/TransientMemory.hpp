/* Quiescent, non-imported, non-dedicated allocations only. Caller serializes and owns retirement. */
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace fuxi {
inline std::atomic<uint64_t> surfaceImportHits{0},surfaceImportMisses{0},texturePoolHits{0},texturePoolMisses{0},texturePoolTrims{0},texturePoolBytes{0},texturePoolWasted{0},texturePoolIdleTrims{0},texturePoolPressureTrims{0},texturePoolRejectFull{0},texturePoolRejectBudget{0};
inline std::array<std::atomic<uint64_t>,40> texturePoolClassHits{},texturePoolClassMisses{};
// Memory is allocated at a size class (1/8 steps above 32 KiB, 4 KiB below) and bound at offset 0 to any image whose requirement fits.
inline uint64_t memorySizeClass(uint64_t n){
 if(!n)return 0;unsigned top=63-__builtin_clzll(n);unsigned shift=top>15?top-3:12;uint64_t step=uint64_t(1)<<shift;return (n+step-1)&~(step-1);
}
inline unsigned memoryClassBucket(uint64_t n){unsigned b=n?64-__builtin_clzll(n):0;return b<40?b:39;}
struct MemoryKey {
 uint64_t bytes{};uint32_t type{};
 bool operator==(const MemoryKey &b) const{return bytes==b.bytes && type==b.type;}
};
template<class Handle> class TransientMemory {
 struct Entry {MemoryKey key{};Handle memory{};uint64_t returned{};};
 std::array<Entry,64> entries{};size_t used=0;
 template<class Free> void drop(Entry &e,Free &free,std::atomic<uint64_t> &reason){free(e.memory,e.key.bytes);used-=e.key.bytes;texturePoolBytes-=e.key.bytes;e={};++texturePoolTrims;++reason;}
public:
 static constexpr size_t budget=96u*1024*1024;
 static constexpr uint64_t idleNanos=5ull*1000*1000*1000;
 size_t bytes() const{return used;}
 // Smallest pooled block with bytes>=need, a permitted memory type, and at most need/4+4 KiB slack.
 bool take(uint64_t need,uint32_t typeBits,Handle &memory,MemoryKey &got){
  Entry *best=nullptr;
  for(auto &e:entries)if(e.memory && e.key.bytes>=need && e.key.bytes-need<=need/4+4096 && e.key.type<32 && (typeBits&(1u<<e.key.type)) && (!best || e.key.bytes<best->key.bytes))best=&e;
  if(!best){++texturePoolMisses;++texturePoolClassMisses[memoryClassBucket(need)];return false;}
  memory=best->memory;got=best->key;used-=got.bytes;texturePoolBytes-=got.bytes;best->memory={};++texturePoolHits;++texturePoolClassHits[memoryClassBucket(got.bytes)];texturePoolWasted+=got.bytes-need;return true;
 }
 bool put(const MemoryKey &key,Handle memory,uint64_t now){
  if(!memory || !key.bytes)return false;
  if(key.bytes>budget-used){++texturePoolRejectBudget;return false;}
  for(auto &e:entries)if(!e.memory){e={key,memory,now};used+=key.bytes;texturePoolBytes+=key.bytes;return true;}
  ++texturePoolRejectFull;return false;
 }
 template<class Free> void trim(uint64_t now,bool all,Free free){
  for(auto &e:entries)if(e.memory && (all || (now>=e.returned && now-e.returned>=idleNanos)))drop(e,free,all?texturePoolPressureTrims:texturePoolIdleTrims);
 }
 // Free the longest-idle blocks until at least `need` bytes are released.
 template<class Free> void reclaim(uint64_t need,Free free){
  while(need){Entry *oldest=nullptr;for(auto &e:entries)if(e.memory && (!oldest || e.returned<oldest->returned))oldest=&e;
   if(!oldest)return;auto n=oldest->key.bytes;drop(*oldest,free,texturePoolPressureTrims);need=n>=need?0:need-n;}
 }
};
}
