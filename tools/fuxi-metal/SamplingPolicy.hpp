/* A sampling refusal can omit one draw, only with the explicit boot opt-in. */
#pragma once
#include "TranslationPolicy.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
namespace fuxi {
inline std::atomic<uint64_t> skippedSamplingDraws{0};
inline bool skipSamplingDraw(const char *reason) {
 if(!systemTranslationPolicy().skipSampling)return false;
 auto count=++skippedSamplingDraws;
 // First eight, then powers of two: bounded log volume, cumulative count.
 if(count<=8 || !(count&(count-1)))fprintf(stderr,"FUXI MTL: sampling skip draw reason=%s count=%llu\n",reason,(unsigned long long)count);
 return true;
}
}
