/* Pixel A/B controls on the production backend and authored GLSL. */
#include "Backend.hpp"
#include <cassert>
#include <cstdio>
int main(){
 using namespace fuxi;Backend b;bool merge=systemTranslationPolicy().merge;
 auto *src=b.allocateTexture(80,5,16,16),*other=b.allocateTexture(80,5,16,16),*dst=b.allocateTexture(80,5,16,16,3);
 auto *bytes=b.allocate(1024),*read=b.allocate(1024);auto *p=b.compilePipeline(nullptr,"control",nullptr,"control");auto *sam=b.allocateSampler(false,false,1);
 auto put=[&](Texture *t,uint8_t value,uint32_t level=0){memset(bytes->contents,value,1024);if(t==src || t==other)for(unsigned i=3;i<1024;i+=4)((uint8_t *)bytes->contents)[i]=t==src?16:32;Operation op{};op.src=bytes;op.texture=t;op.upload=true;op.dstLevel=level;b.submit({op});};
 put(src,91);put(other,37);
 Operation draw{};draw.encoderID=1;draw.pipeline=p;draw.texture=dst;draw.samples[0]=src;draw.sampleCount[0]=1;draw.samplers[0]=sam;draw.vertices=6;draw.viewport={0,16,16,-16,0,1};draw.scissor={{0,0},{16,16}};draw.clear[0]=.2f;draw.clear[1]=.4f;draw.clear[2]=.6f;draw.clear[3]=1;
 void *proxy=dlopen(FUXI_MTL_ICD,RTLD_NOW|RTLD_LOCAL);assert(proxy);
 auto counter=[&](const char *name){auto fn=reinterpret_cast<uint64_t (*)()>(dlsym(proxy,name));assert(fn);return fn();};
 auto run=[&](const std::vector<Operation> &ops,unsigned passes,unsigned merged){auto beforePass=counter("render_merge_passes"),beforeDraw=counter("render_merge_draws");FrameSample s;{FrameTraceScope scope(&s);b.submit(ops);}assert(s.passes==(merge?passes:s.draws+std::count_if(ops.begin(),ops.end(),[](const Operation &o){return o.clearOnly && (o.areaWidth || o.areaHeight);})));assert(counter("render_merge_passes")-beforePass==s.passes && counter("render_merge_draws")-beforeDraw==s.draws);assert(s.mergedDraws==(merge?merged:0));return s;};
 auto pixels=[&](const char *label,uint32_t level=0){Operation get{};get.texture=dst;get.dst=read;get.srcLevel=level;b.submit({get});printf("PIXELS %s ",label);for(size_t i=0;i<dst->levelWidth(level)*dst->levelHeight(level)*4;++i)printf("%02x",((uint8_t *)read->contents)[i]);puts("");};
 // Distinct bindings, viewports and scissors remain per draw. Clear is applied
 // once; blending makes ordering, repeated clears and missing draws observable.
 std::vector<Operation> many(23,draw);for(unsigned i=0;i<many.size();++i){auto &op=many[i];op.load=i?1:2;op.samples[0]=i%2?other:src;op.scissor.extent.width=16-i%5;op.viewport.width=16-float(i%3);}
 run(many,1,22);assert(counter("render_merge_load")==uint64_t(merge?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD));pixels("blend-clear");assert(dst->isDefined(0));
 // A load first draw must preserve earlier pixels.
 for(auto &op:many)op.load=1;run(many,1,22);pixels("blend-load");
 // The final discard is observable through defined tracking, never through
 // discarded pixels (whose values Vulkan deliberately leaves unspecified).
 many.back().store=0;run(many,1,22);assert(!dst->isDefined(0));assert(counter("render_merge_store")==VK_ATTACHMENT_STORE_OP_DONT_CARE);
 bool refused=false;try{b.submit({many.back()});}catch(const std::runtime_error &e){refused=std::string(e.what()).find("load undefined")!=std::string::npos;}assert(refused && !b.poisoned);
 run({draw},1,0);pixels("after-discard-clear");
 // Encoder identity is essential even if the next encoder starts with LOAD.
 auto a=draw,c=draw;c.load=1;c.encoderID=2;auto s=run({a,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::encoder)]==1);pixels("encoders");
 c.encoderID=0;s=run({a,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::encoder)]==1);
 c=draw;c.load=1;c.areaWidth=8;c.areaHeight=8;c.scissor.extent={8,8};s=run({a,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::area)]==1);pixels("area-split");
 // Partial render areas must preserve uploaded pixels outside their rectangle.
 put(dst,173);many.assign(9,c);for(unsigned i=0;i<many.size();++i)many[i].load=i?1:2;run(many,1,8);pixels("partial");
 for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)if(x>=8 || y>=8)for(unsigned q=0;q<4;++q)assert(((uint8_t *)read->contents)[(y*16+x)*4+q]==173);
 // Empty partial clears and transfer operations remain outside merged passes.
 Operation empty=draw;empty.pipeline=nullptr;empty.clearOnly=true;empty.areaWidth=8;empty.areaHeight=8;c=draw;c.load=1;s=run({draw,empty,c},3,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::nonDraw)]==2);pixels("empty-partial");
 Operation upload{};upload.texture=other;upload.upload=true;upload.src=bytes;c.samples[0]=other;s=run({draw,upload,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::nonDraw)]==2);pixels("upload");
 Operation copy{};copy.texture=other;copy.sourceTexture=src;s=run({draw,copy,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::nonDraw)]==2);pixels("image-copy");
 Operation mips{};mips.texture=dst;mips.generateMips=true;mips.mipCount=3;s=run({draw,mips,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::nonDraw)]==2);pixels("blit-mips",2);
 // A preceding draw produces a mip sampled later in the same encoder. This
 // must split even with a full GENERAL view and proven disjoint mip accesses.
 put(dst,83,0);put(dst,67,1);put(dst,51,2);a=draw;a.dstLevel=1;a.viewport={0,8,8,-8,0,1};a.scissor.extent={8,8};
 c=draw;c.load=1;c.samples[0]=dst;c.sampleBase[0]=1;c.sampleCount[0]=1;s=run({a,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::attachmentSample)]==1);pixels("mip-producer");
 c.sampleBase[0]=0;c.sampleCount[0]=3;c.dstLevel=1;c.viewport={0,8,8,-8,0,1};c.scissor.extent={8,8};c.load=1;a=draw;
 s=run({a,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::attachmentSample)]==1);pixels("general-view",1);
 if(systemTranslationPolicy().allowFeedback){
  // True same-level feedback retains the existing opt-in's undefined written
  // texels. Only compare an untouched mip, plus split and gate survival.
  c.dstLevel=0;c.viewport=draw.viewport;c.scissor=draw.scissor;s=run({draw,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::attachmentSample)]==1);pixels("feedback-untouched",2);
 }
 // Internal clear/discard actions cannot disappear even for a malformed host
 // producer that reuses an encoder identity.
 c=draw;s=run({draw,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::actions)]==1);pixels("action-split");
 // Storage can write buffers consumed by a later draw. Preserve the old
 // inter-op barrier by isolating both draws; one fragment invocation per draw.
 auto *storage=b.compilePipeline(nullptr,"control",nullptr,"storage");auto *state=b.allocate(16);memset(state->contents,0,16);
 a=draw;a.pipeline=storage;a.buffers[1][1]={state,0,false};a.scissor.extent={1,1};c=a;c.load=1;
 s=run({a,c},2,0);if(merge)assert(s.mergeSplits[size_t(MergeSplit::storage)]==1);assert(*(uint32_t *)state->contents==2);pixels("storage-barrier");b.destroy(storage);b.destroy(state);
 // Hoisted state rolls back on recording failure, including defined bits.
 many.assign(5,draw);for(unsigned i=0;i<many.size();++i)many[i].load=i?1:2;auto layouts=dst->levelLayouts;auto defined=dst->definedLevels;auto layout=dst->layout;
 b.failBeforeQueue=true;refused=false;try{b.submit(many);}catch(...){refused=true;}assert(refused && !b.poisoned && dst->definedLevels==defined && dst->levelLayouts==layouts && dst->layout==layout);run(many,1,4);
 // Use both lanes and retire with the recording thread's trace scope gone.
 std::function<void()> first,second;b.submit(many,{},&first);b.submit(many,{},&second);first();second();pixels("two-flights");
 b.destroy(p);b.dropPipelineCacheForTest();b.destroy(sam);b.destroy(src);b.destroy(other);b.destroy(dst);b.destroy(bytes);b.destroy(read);
 puts("PASS: render merge pixels, actions, boundaries, mip hazards, partial areas, rollback and two flights");
}
