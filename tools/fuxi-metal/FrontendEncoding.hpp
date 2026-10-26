/* Command-local encoding shortcuts; ownership stays in the command's resource set. */
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include "ShadowRanges.hpp"
namespace fuxi {
class CommandResourceCache {
 struct Entry {const void *resource=nullptr;uint32_t reasons=0;};
 std::array<Entry,64> entries{};
public:
 bool seen(const void *resource,bool bounded,ShadowFullReason reason=ShadowFullReason::other){
  if(!resource)return true;
  auto &e=entries[(uintptr_t(resource)>>4)%entries.size()];uint32_t bit=bounded?1u:2u<<size_t(reason);
  if(e.resource==resource){if(bounded || (e.reasons&bit))return true;e.reasons|=bit;}
  else e={resource,bit};return false;
 }
 void clear(){entries={};}
};
// Draws share immutable diagnostic text; failures still report the full snapshot.
class OperationDiagnostic {
 std::shared_ptr<const std::string> text;
public:
 OperationDiagnostic()=default;
 OperationDiagnostic &operator=(const char *value){text=value && *value?std::make_shared<const std::string>(value):nullptr;return *this;}
 bool empty() const{return !text;}
 void clear(){text.reset();}
 const char *c_str() const{return text?text->c_str():"";}
 friend bool operator==(const OperationDiagnostic &a,const OperationDiagnostic &b){return a.text==b.text || (a.text && b.text && *a.text==*b.text);}
 friend std::string operator+(std::string a,const OperationDiagnostic &b){return a+=b.c_str();}
};
}
