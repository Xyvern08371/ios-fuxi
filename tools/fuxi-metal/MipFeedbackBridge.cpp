/* Test-only authored SPIR-V adapter. Never built into the production bridge. */
#include "Translator.hpp"
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
struct Product {bool multi;std::vector<uint32_t> vertex,fragment;};
static std::vector<uint32_t> shader(const char *key){
 std::ifstream in(getenv(key),std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(in),{}};
 if(bytes.size()<20 || bytes.size()%4)throw std::runtime_error("control SPIR-V");
 std::vector<uint32_t> words(bytes.size()/4);memcpy(words.data(),bytes.data(),bytes.size());return words;
}
static void descriptors(fuxi::Descriptor *out,bool multi){for(unsigned i=0;i<(multi?2u:1u);++i)out[i]={i,1,16,i,i,44};}
extern "C" uint32_t fuxi_m2v_abi(){return 8;}
extern "C" void *fuxi_m2v_open(const uint8_t *,size_t,char *,size_t){return nullptr;}
extern "C" void fuxi_m2v_close(void *){}
extern "C" int fuxi_m2v_function(void *,const char *,char *,size_t){return 0;}
extern "C" void fuxi_m2v_free(void *p){delete (Product *)p;}
extern "C" int fuxi_m2v_sampler(void *,uint32_t,fuxi::BakedSampler *out,char *,size_t){*out={};return 1;}
extern "C" int fuxi_m2v_identity(void *,const char *,void *,const char *name,const fuxi::CompileOptions *,uint8_t *out,char *,size_t){memset(out,!strcmp(name,"multi")?3:1,32);return 1;}
extern "C" int fuxi_m2v_product_hash(void *p,uint8_t *out){memset(out,((Product *)p)->multi?4:2,32);return 1;}
extern "C" int fuxi_m2v_compile(void *,const char *,void *,const char *name,void *,uint64_t,uint64_t,const fuxi::CompileOptions *options,fuxi::Translation *out,char *,size_t){
 bool multi=!strcmp(name,"multi");auto *p=new Product{multi,shader("CONTROL_VERTEX"),shader(multi?"CONTROL_FRAGMENT_MULTI":"CONTROL_FRAGMENT")};
 *out={};out->handle=p;out->orderedColorAccess=options->orderedColorAccess;
 out->vertex=p->vertex.data();out->vertexCount=p->vertex.size();out->vertexEntry="main";
 out->fragment=p->fragment.data();out->fragmentCount=p->fragment.size();out->fragmentEntry="main";
 out->mask=out->colors[0].mask=15;out->rangeCount=1;out->ranges[0]={0,4,16};out->pushSize=4;out->pushStages=16;
 out->descriptorCount=multi?2:1;descriptors(out->descriptors,multi);return 1;
}
extern "C" int fuxi_m2v_draw(void *product,const fuxi::DrawInput *in,fuxi::DrawOutput *out,char *,size_t){
 auto *p=(Product *)product;if(!in->formats[0] || !in->filters[0])return 0;
 *out={};out->count=in->count;out->first=in->start;out->descriptorCount=p->multi?2:1;descriptors(out->descriptors,p->multi);
 out->pushCount=1;out->pushes[0].size=4;out->pushes[0].stages=16;
 float lod=std::strtof(getenv("CONTROL_LOD"),nullptr);memcpy(out->pushes[0].bytes,&lod,4);return 1;
}
extern "C" int fuxi_m2v_variant(void *,const fuxi::DrawInput *,fuxi::Translation *,char *,size_t){return 0;}
