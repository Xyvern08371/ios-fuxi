#pragma once
// GPU-SCANOUT ownership for imported IOSurfaces. Original Fuxi implementation.
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <functional>
namespace fuxi {
inline bool gpuScanoutEnabled(const char *args) {
 if(!args)return false;
 unsigned keys=0;bool enabled=false;
 for(const char *p=args;*p;) {
  while(*p==' ' || *p=='\t' || *p=='\r' || *p=='\n')++p;
  const char *begin=p;while(*p && *p!=' ' && *p!='\t' && *p!='\r' && *p!='\n')++p;
  size_t n=size_t(p-begin);
  if(n>=15 && !strncmp(begin,"fuxigpuscanout=",15)){++keys;enabled=n==16 && begin[15]=='1';}
 }
 return keys==1 && enabled;
}
class SurfaceLease {
 using Lock=int (*)(uintptr_t);
 using Unlock=void (*)(uintptr_t);
 bool enabled_,exclusive_;Lock lock_;Unlock unlock_;std::function<bool()> poisoned_;
 inline static std::mutex mutex_;inline static std::condition_variable wake_;inline static std::vector<uintptr_t> writers_;
 std::vector<uintptr_t> held_;
public:
 template<class Poison> SurfaceLease(bool enabled,Lock lock,Unlock unlock,const Poison *poisoned,bool exclusive=false)
  :enabled_(enabled),exclusive_(exclusive),lock_(lock),unlock_(unlock),poisoned_([poisoned]{return poisoned && bool(*poisoned);}) {}
 SurfaceLease(const SurfaceLease &)=delete;
 SurfaceLease &operator=(const SurfaceLease &)=delete;
 void add(uintptr_t surface) {
  if(!enabled_ || !surface || std::find(held_.begin(),held_.end(),surface)!=held_.end())return;
  if(held_.size()==256)throw std::runtime_error("scanout IOSurface lease limit");
  held_.push_back(surface);
  std::unique_lock<std::mutex> guard(mutex_,std::defer_lock);
  if(exclusive_){guard.lock();wake_.wait(guard,[&]{return poisoned_() || std::find(writers_.begin(),writers_.end(),surface)==writers_.end();});
   if(poisoned_()){held_.pop_back();throw std::runtime_error("scanout device poisoned");}try{writers_.push_back(surface);}catch(...){held_.pop_back();throw;}}
  if(lock_(surface)){if(exclusive_){writers_.erase(std::find(writers_.begin(),writers_.end(),surface));wake_.notify_all();}held_.pop_back();throw std::runtime_error("scanout IOSurface lock failed");}
 }
 ~SurfaceLease() {
  // An uncertain submission keeps both the producer lock and the allocation
  // alive. The kernel quarantines DMA; no framebuffer reader may reuse it.
  if(!exclusive_){if(!poisoned_())for(auto it=held_.rbegin();it!=held_.rend();++it)unlock_(*it);return;}
  std::lock_guard<std::mutex> guard(mutex_);
  if(poisoned_()){wake_.notify_all();return;}
  for(auto it=held_.rbegin();it!=held_.rend();++it){unlock_(*it);if(exclusive_)writers_.erase(std::find(writers_.begin(),writers_.end(),*it));}
  wake_.notify_all();
 }
};
}
