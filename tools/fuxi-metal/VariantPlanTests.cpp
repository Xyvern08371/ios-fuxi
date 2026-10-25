/* Authored adapter forces runtime variants and a coarse-key collision. */
#include "Backend.hpp"
#include <cassert>
int main(){
 using namespace fuxi;Backend b;
 auto *src=b.allocateTexture(80,5,16,16),*dst=b.allocateTexture(80,5,16,16);auto *bytes=b.allocate(1024);
 memset(bytes->contents,37,1024);Operation upload{};upload.src=bytes;upload.texture=src;upload.upload=true;b.submit({upload});
 auto *p=b.compilePipeline(nullptr,"control",nullptr,"control");auto *sam=b.allocateSampler(false,false,1);
 void *bridge=dlopen(FUXI_MTL_TRANSLATOR,RTLD_NOW|RTLD_LOCAL);assert(bridge);
 auto counter=[&](const char *name){auto fn=reinterpret_cast<uint64_t (*)()>(dlsym(bridge,name));assert(fn);return fn();};
 Operation draw{};draw.encoderID=1;draw.texture=dst;draw.pipeline=p;draw.samples[0]=src;draw.sampleCount[0]=1;draw.samplers[0]=sam;draw.vertices=6;draw.viewport={0,16,16,-16,0,1};draw.scissor={{0,0},{16,16}};
 std::vector<Operation> ops(120,draw);for(unsigned i=0;i<120;++i){ops[i].vertices=i%2?6:3;ops[i].load=i?1:2;}
 b.submit(ops);assert(counter("fixture_base_calls")==1 && counter("fixture_variant_calls")==120 && counter("fixture_variant_creates")==1);
 b.submit(ops);assert(counter("fixture_base_calls")==1 && counter("fixture_variant_calls")==240 && p->variants.size()==1);
 // A candidate rejection must retry the baseline, rather than refuse a valid
 // draw or reuse a specialization selected for a different complete input.
 auto fallback=draw;fallback.first=1;fallback.vertices=3;fallback.load=1;
 b.submit({fallback,draw});assert(counter("fixture_base_calls")==2 && counter("fixture_variant_calls")==242 && counter("fixture_variant_creates")==1);
 Operation read{};read.texture=dst;read.dst=bytes;b.submit({read});for(unsigned i=0;i<1024;++i)assert(((uint8_t *)bytes->contents)[i]==37);
 b.destroy(p);b.destroy(sam);b.destroy(src);b.destroy(dst);b.destroy(bytes);b.close();dlclose(bridge);
 puts("PASS: 120 changing draw inputs bypass baseline rejections; coarse-key collision falls back; exact pixels");
}
