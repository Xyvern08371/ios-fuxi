/* Pool policy and CPU storage ownership; no Vulkan or platform dependencies. */
#include "TransientMemory.hpp"
#include "CommandStorage.hpp"
#include <cassert>
#include <memory>
#include <thread>
int main(){
 using namespace fuxi;TransientMemory<unsigned> pool;MemoryKey key{4096,256,80,5,1};
 assert(!pool.take(key));assert(pool.put(key,1,0));auto different=key;
 for(unsigned field=0;field<5;++field){different=key;if(field==0)++different.bytes;if(field==1)++different.alignment;if(field==2)++different.format;if(field==3)++different.usage;if(field==4)++different.type;assert(!pool.take(different));}
 assert(pool.take(key)==1 && !pool.bytes());
 unsigned freed=0;auto free=[&](unsigned,size_t){++freed;};
 for(unsigned i=1;i<=32;++i)assert(pool.put(key,i,0));assert(!pool.put(key,33,0));
 pool.trim(pool.idleNanos-1,false,free);assert(!freed);pool.trim(pool.idleNanos,false,free);assert(freed==32 && !pool.bytes());
 auto large=key;large.bytes=pool.budget;assert(pool.put(large,40,100));assert(!pool.put(key,41,100));
 pool.trim(99,false,free);assert(pool.bytes()==pool.budget);pool.trim(100,true,free);assert(!pool.bytes() && freed==33);
 assert(texturePoolHits==1 && texturePoolMisses==6 && texturePoolTrims==33 && !texturePoolBytes);
 CommandStorage<std::shared_ptr<int>> storage;auto a=storage.acquire();auto *identity=a.data();auto owned=std::make_shared<int>(12);a.push_back(owned);
 auto b=storage.acquire();assert(b.data()!=identity);storage.release(a);assert(owned.use_count()==1);
 auto c=storage.acquire();assert(c.empty() && c.data()==identity && c.capacity()>=256);
 c.reserve(1024);identity=c.data();storage.release(c);auto d=storage.acquire();assert(d.data()==identity && d.capacity()==1024);
 // Concurrent return/acquire owns disjoint vectors; live command data is never borrowed.
 std::thread other([&]{for(unsigned i=0;i<100;++i){auto v=storage.acquire();v.push_back(owned);storage.release(v);}});
 for(unsigned i=0;i<100;++i){auto v=storage.acquire();assert(v.data()!=d.data());storage.release(v);}other.join();
 struct Block {uint8_t bytes[64*1024];};UploadStorage<Block *> uploads;UploadRing<Block *> ring,flight;
 unsigned allocations=0,releases=0;auto allocate=[&](size_t n){assert(n==sizeof(Block));++allocations;return new Block{};};auto contents=[](Block *block){return block->bytes;};auto drop=[&](Block *block){++releases;delete block;};
 uint8_t bytes[4096];memset(bytes,0x34,sizeof(bytes));auto first=ring.copy(bytes,37,uploads,allocate,contents);memset(bytes,0x56,sizeof(bytes));
 auto second=ring.copy(bytes,128,uploads,allocate,contents);assert(first.handle==second.handle && second.offset==256 && first.length==37 && first.handle->bytes[0]==0x34 && !first.handle->bytes[37]);
 auto pending=flight.copy(bytes,128,uploads,allocate,contents);assert(pending.handle!=first.handle);
 for(unsigned i=0;i<20;++i)ring.copy(bytes,4096,uploads,allocate,contents);assert(allocations==3);
 ring.release(uploads,true,drop);auto again=ring.copy(bytes,128,uploads,allocate,contents);assert(again.handle==first.handle && allocations==3);
 flight.release(uploads,false,drop);assert(releases==1);ring.release(uploads,true,drop);uploads.clear(drop);assert(allocations==releases);
 bool refused=false;try{ring.copy(bytes,4097,uploads,allocate,contents);}catch(const std::runtime_error&){refused=true;}assert(refused);
 // Overflow storage is discarded rather than retained by the bounded cache.
 CommandStorage<uint8_t> bounded;auto huge=bounded.acquire();huge.reserve(bounded.budget+1);auto *largeIdentity=huge.data();bounded.release(huge);auto small=bounded.acquire();assert(small.data()!=largeIdentity);
 puts("PASS: exact memory key, entry/byte bounds, idle/pressure trim, disjoint storage reuse, inline slices/copy/padding, flight isolation and poison discard");
}
