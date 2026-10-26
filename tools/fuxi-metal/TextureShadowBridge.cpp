/* Authored R8/RG8 sampling fixture; no Apple shader inputs. */
#define fuxi_m2v_draw texture_shadow_fixture_draw
#include "MipFeedbackBridge.cpp"
#undef fuxi_m2v_draw
extern "C" int fuxi_m2v_draw(void *p,const fuxi::DrawInput *in,fuxi::DrawOutput *out,char *why,size_t cap){
 int ok=texture_shadow_fixture_draw(p,in,out,why,cap);out->descriptors[0].format=in->formats[0]==1?9:16;return ok;
}
