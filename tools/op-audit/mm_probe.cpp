// Probe 2: CPU vs Vulkan vs double for a plain f32 mul_mat (the conv inner loop).
#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"
#include <cmath>
#include <cstdio>
#include <vector>

int main(){
    ggml_backend_load_all();
    auto cd=ggml_backend_dev_by_name("CPU"); auto vd=ggml_backend_dev_by_name("Vulkan0");
    auto cpu=ggml_backend_dev_init(cd,nullptr); auto vk=ggml_backend_dev_init(vd,nullptr);
    const int K=576,M=8,N=256;
    for(int trial=0;trial<2;trial++){
        ggml_backend_t be=trial?vk:cpu;
        ggml_init_params ip={256u*1024*1024,nullptr,true};
        ggml_context*ctx=ggml_init(ip);
        ggml_tensor*w=ggml_new_tensor_2d(ctx,GGML_TYPE_F32,K,M); ggml_set_name(w,"w");
        ggml_tensor*x=ggml_new_tensor_2d(ctx,GGML_TYPE_F32,K,N); ggml_set_name(x,"x");
        ggml_tensor*o=ggml_mul_mat(ctx,w,x);
        ggml_cgraph*gf=ggml_new_graph(ctx); ggml_build_forward_expand(gf,o);
        ggml_backend_buffer_t buf=ggml_backend_alloc_ctx_tensors(ctx,be);
        std::vector<float> hw(K*M),hx(K*N);
        for(size_t i=0;i<hw.size();i++) hw[i]=std::cos(0.013f*i)*0.5f;
        for(size_t i=0;i<hx.size();i++) hx[i]=std::sin(0.01f*i)*0.9f;
        ggml_backend_tensor_set(w,hw.data(),0,hw.size()*4);
        ggml_backend_tensor_set(x,hx.data(),0,hx.size()*4);
        ggml_backend_graph_compute(be,gf);
        std::vector<float> got(M*N); ggml_backend_tensor_get(o,got.data(),0,got.size()*4);
        double maxe=0,se=0,ss=0;
        for(int n=0;n<N;n++)for(int m=0;m<M;m++){
            double acc=0; for(int k=0;k<K;k++) acc+=(double)hw[k+m*K]*hx[k+n*K];
            double d=got[m+n*M]-acc; maxe=std::max(maxe,fabs(d)); se+=d*d; ss+=acc*acc;
        }
        printf("%s mul_mat K=%d: max_abs=%.3g nmse=%.3g\n",trial?"vulkan":"cpu   ",K,maxe,se/ss);
        ggml_backend_buffer_free(buf); ggml_free(ctx);
    }
    return 0;
}
