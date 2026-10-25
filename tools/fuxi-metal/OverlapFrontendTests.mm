/* Native selectors/ownership against a paused CPU fence model; requires Mac. */
#define FUXI_MTL_HOST_TEST
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include <cassert>
#include <future>
@interface FuxiOverlapBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiOverlapBase
@end
@interface FuxiOverlapDrawable : NSObject { @public unsigned presents; }
@end
@implementation FuxiOverlapDrawable
- (void)present {++presents;}
@end
int main(){@autoreleasepool{
 assert(fuxi::systemTranslationPolicy().overlap);
 Class cls=makeDeviceClass([FuxiOverlapBase class]);id device=[cls alloc];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 FuxiQueue *single=(FuxiQueue *)[device newCommandQueueWithMaxCommandBufferCount:1];
 assert(!dispatch_semaphore_wait(single->_inflight,DISPATCH_TIME_NOW));assert(dispatch_semaphore_wait(single->_inflight,DISPATCH_TIME_NOW));dispatch_semaphore_signal(single->_inflight);[single release];
 FuxiQueue *queue=(FuxiQueue *)[device newCommandQueue];
 static int deliveryKey;dispatch_queue_t delivery=dispatch_queue_create("fuxi.test.completion",DISPATCH_QUEUE_SERIAL);
 dispatch_queue_set_specific(delivery,&deliveryKey,&deliveryKey,nullptr);[queue setCompletionQueue:delivery];dispatch_suspend(queue->_retireQueue);
 __block unsigned callbacks=0,scheduled=0;
 FuxiCommandBuffer *a=[(FuxiCommandBuffer *)[queue commandBuffer] retain],*b=[(FuxiCommandBuffer *)[queue commandBuffer] retain];
 // Enable attribution directly without requiring a backboardd process gate.
 for(auto *command:{a,b}){command->_frameTrace=YES;command->_traceCreated=fuxi::FrameClock::now();command->_traceThread=std::this_thread::get_id();}
 FuxiOverlapDrawable *drawable=[FuxiOverlapDrawable new];
 FuxiBuffer *buffer=(FuxiBuffer *)[device newBufferWithLength:4096 options:0];
 id<MTLBlitCommandEncoder> encoder=[a blitCommandEncoder];[encoder fillBuffer:(id)buffer range:NSMakeRange(0,4096) value:42];[encoder endEncoding];
 [a addScheduledHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusScheduled);[command waitUntilScheduled];++scheduled;}];
 [a addCompletedHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusCompleted && callbacks==0 && dispatch_get_specific(&deliveryKey)==&deliveryKey);[command waitUntilCompleted];++callbacks;}];
 [a presentDrawable:(id)drawable];
 [b addCompletedHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusCompleted && callbacks==1);++callbacks;}];
 // Explicit enqueue order wins over reversed commit order.
 [a enqueue];[b commit];[a commit];[buffer release];
 [a waitUntilScheduled];[b waitUntilScheduled];assert(scheduled==1 && callbacks==0 && !drawable->presents && fuxi::Backend::live==1);
 // Late registration while a fence is pending must join that completion.
 [b addCompletedHandler:^(id<MTLCommandBuffer> command){(void)command;assert(callbacks==2);++callbacks;}];
 FuxiCommandBuffer *third=[(FuxiCommandBuffer *)[queue commandBuffer] retain];third->_frameTrace=YES;third->_traceCreated=fuxi::FrameClock::now();third->_traceThread=std::this_thread::get_id();
 [third addCompletedHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusCompleted && callbacks==3);++callbacks;}];
 std::promise<void> started;auto blocked=std::async(std::launch::async,[&]{@autoreleasepool{started.set_value();[third commit];}});
 started.get_future().wait();assert(blocked.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 dispatch_resume(queue->_retireQueue);blocked.get();[third waitUntilCompleted];[b waitUntilCompleted];[a waitUntilCompleted];
 assert(callbacks==4 && drawable->presents==1 && fuxi::Backend::live==0);
 assert(a->_flightTrace.commitCall && b->_flightTrace.submitQueue && third->_flightTrace.admissionWait);
 assert(a->_flightTrace.frontendCPU && a->_flightTrace.frontendWall && third->_flightTrace.frontendWall>=third->_flightTrace.admissionWait);
 assert(queue->_traceReturn && queue->_traceDelivery && queue->_traceHandlers && queue->_traceWaitCompleted);
 [a addCompletedHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusCompleted);++callbacks;}];[a waitUntilCompleted];assert(callbacks==5);
 [a release];[b release];[third release];[drawable release];
 // Snapshot no-copy CPU bytes at commit, even while the submit worker is paused.
 uint8_t bytes[4096];memset(bytes,19,sizeof(bytes));buffer=(FuxiBuffer *)[device newBufferWithBytesNoCopy:bytes length:sizeof(bytes) options:0 deallocator:nil];
 assert(buffer && buffer->_shadow);FuxiBuffer *output=(FuxiBuffer *)[device newBufferWithLength:4096 options:0];
 a=[(FuxiCommandBuffer *)[queue commandBuffer] retain];encoder=[a blitCommandEncoder];[encoder copyFromBuffer:(id)buffer sourceOffset:0 toBuffer:(id)output destinationOffset:0 size:4096];[encoder endEncoding];
 dispatch_suspend(queue->_submitQueue);[a commit];memset(bytes,77,sizeof(bytes));dispatch_resume(queue->_submitQueue);[a waitUntilCompleted];
 for(unsigned i=0;i<4096;++i)assert(((uint8_t *)[output contents])[i]==19);[a release];[buffer release];[output release];
 // Device loss at retirement reports Error once, never presents, still unblocks waiters.
 dispatch_suspend(queue->_retireQueue);a=[(FuxiCommandBuffer *)[queue commandBuffer] retain];drawable=[FuxiOverlapDrawable new];
 [a presentDrawable:(id)drawable];__block unsigned errors=0;
 [a addCompletedHandler:^(id<MTLCommandBuffer> command){assert(command.status==MTLCommandBufferStatusError && command.error);++errors;}];
 [a commit];[a waitUntilScheduled];owner(device)->backend->poisoned=true;dispatch_resume(queue->_retireQueue);[a waitUntilCompleted];assert(errors==1 && !drawable->presents);
 [a release];[drawable release];[queue release];dispatch_release(delivery);[device release];
 puts("PASS: overlap commit return, explicit enqueue/reversed commits, scheduled/completed/late/self-wait handlers, retained resources, presentation, no-copy snapshot, in-flight poison");
}}
