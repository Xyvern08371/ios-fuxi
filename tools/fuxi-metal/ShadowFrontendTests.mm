/* Native selector coverage with paused submit; CPU model, no shader translation. */
#define FUXI_MTL_HOST_TEST
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include <cassert>
@interface FuxiShadowBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiShadowBase
@end
int main(){@autoreleasepool{
 assert(fuxi::systemTranslationPolicy().overlap);fuxi::Backend::modelTextures=true;
 Class cls=makeDeviceClass([FuxiShadowBase class]);id device=[cls alloc];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 FuxiQueue *queue=(FuxiQueue *)[device newCommandQueue];uint8_t cpu[4096];memset(cpu,19,sizeof(cpu));
 FuxiBuffer *buffer=(FuxiBuffer *)[device newBufferWithBytesNoCopy:cpu length:sizeof(cpu) options:0 deallocator:nil];assert(buffer->_shadow);
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:16 height:16 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModePrivate;
 id target=[device newTextureWithDescriptor:td];MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
 FuxiPipeline *pipeline=[FuxiPipeline new];pipeline->_device=[device retain];pipeline->_pipeline=new fuxi::Pipeline{};
 auto *p=pipeline->_pipeline;for(auto &stage:p->shadowWindows)for(auto &window:stage)window.kind=2;
 p->options.strides[0]=32;p->options.attributes[0]={31,0,0};p->options.attributes[1]={31,16,0};p->shadowWindows[1][1]={1,0,0,64};
 unsigned seen=0;fuxi::Backend::modelDraw=[&](const fuxi::Operation &op){auto v=op.buffers[0][0],f=op.buffers[1][1];assert(v.buffer==buffer->_buffer && f.buffer==buffer->_buffer);
  for(unsigned i=0;i<192;++i)assert(((uint8_t *)v.buffer->contents)[v.offset+i]==19);for(unsigned i=0;i<64;++i)assert(((uint8_t *)f.buffer->contents)[f.offset+i]==19);++seen;};
 for(unsigned mode=0;mode<7;++mode){
  memset(cpu,19,sizeof(cpu));memset(buffer->_buffer->contents,3,sizeof(cpu));FuxiCommandBuffer *c=[(FuxiCommandBuffer *)[queue commandBuffer] retain];c->_frameTrace=YES;c->_traceCreated=fuxi::FrameClock::now();c->_traceThread=std::this_thread::get_id();
  FuxiRender *e=(FuxiRender *)[c renderCommandEncoderWithDescriptor:pass];[e setRenderPipelineState:(id)pipeline];
  id<MTLBuffer> buffers[]={(id)buffer};NSUInteger offsets[]={0};[e setVertexBuffers:buffers offsets:offsets withRange:NSMakeRange(0,1)];[e setVertexBufferOffset:256 atIndex:0];
  [e setFragmentBuffers:buffers offsets:offsets withRange:NSMakeRange(1,1)];[e setFragmentBufferOffset:448 atIndex:1];uint8_t inlineBytes[16]={};[e setVertexBytes:inlineBytes length:16 atIndex:3];
  if(mode==1)p->shadowWindows[1][1]={}; // Unknown shader/argument access is sticky for this command.
  if(mode!=6)[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
  if(mode==2){[e setVertexBufferOffset:1024 atIndex:0];[e setFragmentBufferOffset:1216 atIndex:1];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];}
  if(mode==3)[e useResource:(id)buffer usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
  if(mode==4){uint16_t indices[]={0,1,2};id index=[device newBufferWithBytes:indices length:sizeof(indices) options:0];[e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt16 indexBuffer:index indexBufferOffset:0];[index release];}
  [e endEncoding];if(mode==5){id output=[device newBufferWithLength:16 options:0];id<MTLBlitCommandEncoder> blit=[c blitCommandEncoder];[blit copyFromBuffer:(id)buffer sourceOffset:3000 toBuffer:output destinationOffset:0 size:16];[blit endEncoding];[output release];}
  assert(!c->_rejected);size_t expected=mode==6?0:(mode==1 || mode>=3)?4096:mode==2?512:256;
  dispatch_suspend(queue->_submitQueue);[c commit];assert(c->_shadows.bytes()==expected && c->_flightTrace.snapshotBytes==expected);memset(cpu,77,sizeof(cpu));dispatch_resume(queue->_submitQueue);[c waitUntilCompleted];
  assert(c.status==MTLCommandBufferStatusCompleted && c->_flightTrace.shadowCopyBytes==expected);assert(((uint8_t *)buffer->_buffer->contents)[3000]==(expected==4096?19:3));[c release];p->shadowWindows[1][1]={1,0,0,64};
 }
 assert(seen==8);fuxi::Backend::modelDraw={};[pipeline release];[target release];[buffer release];[queue release];[device release];
 puts("PASS: shadow vertex/fragment bulk bindings and offset changes, inline bytes, merged draw windows, post-commit writes, unbounded/useResource/indexed/blit full fallback, trace bytes");
}}
