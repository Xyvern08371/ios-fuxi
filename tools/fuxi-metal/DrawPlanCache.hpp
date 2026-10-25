/* Caller serializes planning and pipeline lifetime changes. No resource ownership. */
#pragma once
#include <array>
#include "Translator.hpp"
namespace fuxi {
template<class Pipeline> class DrawPlanCache {
 struct Entry {Pipeline *requested{},*resolved{};DrawInput input{};DrawOutput output{};};
 std::array<Entry,32> entries{};size_t next=0;
public:
 bool find(Pipeline *requested,const DrawInput &input,Pipeline *&resolved,DrawOutput &output) const {
  for(auto &e:entries)if(e.requested && e.requested==requested && sameDrawInput(e.input,input)){resolved=e.resolved;memcpy(&output,&e.output,sizeof(output));return true;}return false;
 }
 void insert(Pipeline *requested,const DrawInput &input,Pipeline *resolved,const DrawOutput &output) {
  auto &e=entries[next];next=(next+1)%entries.size();e.requested=requested;e.resolved=resolved;memcpy(&e.input,&input,sizeof(input));memcpy(&e.output,&output,sizeof(output));
 }
 void invalidate(Pipeline *p){for(auto &e:entries)if(e.requested==p || e.resolved==p)e.requested=e.resolved=nullptr;}
};
}
