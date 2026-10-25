/* Authored host adapter extends the mip fixture with blend/storage controls. */
#include <vulkan/vulkan.h>
#define fuxi_m2v_compile mip_fixture_compile
#define fuxi_m2v_draw mip_fixture_draw
#define fuxi_m2v_identity mip_fixture_identity
#define fuxi_m2v_product_hash mip_fixture_product_hash
#include "MipFeedbackBridge.cpp"
#undef fuxi_m2v_compile
#undef fuxi_m2v_draw
#undef fuxi_m2v_identity
#undef fuxi_m2v_product_hash
static fuxi::Descriptor storageDescriptor(){fuxi::Descriptor d{};d.binding=1;d.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;d.stages=16;d.slot=1;d.range=16;return d;}
// Distinct shader products keep distinct cache identities.
extern "C" int fuxi_m2v_identity(void *,const char *,void *,const char *name,const fuxi::CompileOptions *,uint8_t *out,char *,size_t){memset(out,!strcmp(name,"storage")?7:1,32);return 1;}
extern "C" int fuxi_m2v_product_hash(void *p,uint8_t *out){auto *product=(Product *)p;memset(out,product->multi?8:2,32);return 1;}
extern "C" int fuxi_m2v_compile(void *v,const char *vn,void *f,const char *fn,void *c,uint64_t a,uint64_t b,const fuxi::CompileOptions *options,fuxi::Translation *out,char *why,size_t n){
 int result=mip_fixture_compile(v,vn,f,"control",c,a,b,options,out,why,n);
 // Small alpha keeps ordering and the initial clear visible after many draws.
 out->colors[0].blend=1;auto *factors=out->colors[0].factors;
 factors[0]=factors[3]=VK_BLEND_FACTOR_SRC_ALPHA;factors[1]=factors[4]=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
 factors[2]=factors[5]=VK_BLEND_OP_ADD;
 if(!strcmp(fn,"storage")){auto *p=(Product *)out->handle;p->multi=true;p->fragment=shader("CONTROL_FRAGMENT_STORAGE");out->fragment=p->fragment.data();out->fragmentCount=p->fragment.size();out->descriptorCount=2;out->descriptors[1]=storageDescriptor();}
 return result;
}
extern "C" int fuxi_m2v_draw(void *product,const fuxi::DrawInput *in,fuxi::DrawOutput *out,char *why,size_t n){
 auto *p=(Product *)product;int result=mip_fixture_draw(product,in,out,why,n);if(p->multi)out->descriptors[1]=storageDescriptor();return result;
}
