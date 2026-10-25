/* Test-only Vulkan recording assertions, forwarding to the supplied local ICD. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unordered_map>
static PFN_vkGetInstanceProcAddr instanceProc;
static PFN_vkGetDeviceProcAddr deviceProc;
static PFN_vkCmdBeginRenderPass beginPass;
static PFN_vkCmdEndRenderPass endPass;
static PFN_vkCmdPipelineBarrier pipelineBarrier;
static PFN_vkCmdDraw draw;
static PFN_vkBeginCommandBuffer beginCommand;
static PFN_vkEndCommandBuffer endCommand;
static PFN_vkCreateRenderPass createPass;
static bool inside;
static uint64_t passes,draws;
static std::unordered_map<VkRenderPass,std::pair<uint32_t,uint32_t>> actions;
static uint32_t lastLoad,lastStore;
extern "C" uint64_t render_merge_passes(){return passes;}
extern "C" uint64_t render_merge_draws(){return draws;}
extern "C" uint64_t render_merge_load(){return lastLoad;}
extern "C" uint64_t render_merge_store(){return lastStore;}
static VkResult create(VkDevice d,const VkRenderPassCreateInfo *i,const VkAllocationCallbacks *a,VkRenderPass *p){auto r=createPass(d,i,a,p);if(r==VK_SUCCESS && i->attachmentCount)actions[*p]={i->pAttachments[0].loadOp,i->pAttachments[0].storeOp};return r;}
static void begin(VkCommandBuffer c,const VkRenderPassBeginInfo *i,VkSubpassContents s){assert(!inside);inside=true;++passes;auto a=actions.at(i->renderPass);lastLoad=a.first;lastStore=a.second;beginPass(c,i,s);}
static void end(VkCommandBuffer c){assert(inside);inside=false;endPass(c);}
static void barrier(VkCommandBuffer c,VkPipelineStageFlags a,VkPipelineStageFlags b,VkDependencyFlags f,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *bb,uint32_t ni,const VkImageMemoryBarrier *ib){assert(!inside);pipelineBarrier(c,a,b,f,nm,m,nb,bb,ni,ib);}
static void drawn(VkCommandBuffer c,uint32_t n,uint32_t i,uint32_t f,uint32_t fi){assert(inside);++draws;draw(c,n,i,f,fi);}
static VkResult start(VkCommandBuffer c,const VkCommandBufferBeginInfo *i){inside=false;return beginCommand(c,i);}
static VkResult finish(VkCommandBuffer c){assert(!inside);return endCommand(c);}
static PFN_vkVoidFunction getDevice(VkDevice d,const char *name){auto f=deviceProc(d,name);
#define WRAP(n,var,fn) if(!strcmp(name,"vk" #n)){var=reinterpret_cast<PFN_vk##n>(f);return reinterpret_cast<PFN_vkVoidFunction>(fn);}
 WRAP(CmdBeginRenderPass,beginPass,begin);WRAP(CmdEndRenderPass,endPass,end);WRAP(CmdPipelineBarrier,pipelineBarrier,barrier);WRAP(CmdDraw,draw,drawn);WRAP(BeginCommandBuffer,beginCommand,start);WRAP(EndCommandBuffer,endCommand,finish);WRAP(CreateRenderPass,createPass,create);
 return f;
}
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance i,const char *name){
 if(!instanceProc){auto *lib=dlopen(getenv("SOFTWARE_ICD"),RTLD_NOW|RTLD_LOCAL);assert(lib);instanceProc=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib,"vk_icdGetInstanceProcAddr"));assert(instanceProc);}
 auto f=instanceProc(i,name);if(!strcmp(name,"vkGetDeviceProcAddr")){deviceProc=reinterpret_cast<PFN_vkGetDeviceProcAddr>(f);return reinterpret_cast<PFN_vkVoidFunction>(getDevice);}return f;
}
