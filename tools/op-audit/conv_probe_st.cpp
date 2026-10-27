// Probe: is Vulkan CONV_2D accurate vs a host double reference?
#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"
#include <cmath>
#include <cstdio>
#include <vector>

static ggml_tensor * mk(ggml_context * c, int64_t a,int64_t b,int64_t d,int64_t e,const char*n){
    auto*t=ggml_new_tensor_4d(c,GGML_TYPE_F32,a,b,d,e); ggml_set_name(t,n); return t;
}

int main(){
    ggml_backend_load_all();
    auto cd=ggml_backend_dev_by_name("CPU"); auto vd=ggml_backend_dev_by_name("Vulkan0");
    auto cpu=ggml_backend_dev_init(cd,nullptr); auto vk=ggml_backend_dev_init(vd,nullptr);
    if (getenv("NTH1")) ggml_backend_cpu_set_n_threads(cpu,1);

    const int W=16,H=16,Cin=64,Cout=8,K=3,P=1;
    for (int trial=0; trial<2; trial++) {
        ggml_backend_t be = trial? vk : cpu;
        ggml_init_params ip={256u*1024*1024,nullptr,true};
        ggml_context*ctx=ggml_init(ip);
        ggml_tensor* in=mk(ctx,W,H,Cin,1,"in");
        ggml_tensor* kn=mk(ctx,K,K,Cin,Cout,"k");
        ggml_tensor* out=ggml_conv_2d(ctx,kn,in,1,1,P,P,1,1);
        ggml_cgraph*gf=ggml_new_graph(ctx); ggml_build_forward_expand(gf,out);
        ggml_backend_buffer_t buf=ggml_backend_alloc_ctx_tensors(ctx,be);
        // deterministic values
        std::vector<float> hi(W*H*Cin), hk(K*K*Cin*Cout);
        for(size_t i=0;i<hi.size();i++) hi[i]=std::sin(0.01f*i)*0.9f;
        for(size_t i=0;i<hk.size();i++) hk[i]=std::cos(0.013f*i)*0.5f;
        ggml_backend_tensor_set(in,hi.data(),0,hi.size()*4);
        ggml_backend_tensor_set(kn,hk.data(),0,hk.size()*4);
        if (ggml_backend_graph_compute(be,gf)!=GGML_STATUS_SUCCESS){printf("compute fail\n");return 1;}
        std::vector<float> o(W*H*Cout); ggml_backend_tensor_get(out,o.data(),0,o.size()*4);
        // host double reference, ggml layout: in[w,h,ci,n], k[kw,kh,ci,co], out[ow,oh,co,n]
        double maxe=0; int bad=0; double rms=0,ss=0; int cnt=0;
        for(int oh=0;oh<H;oh++)for(int ow=0;ow<W;ow++)for(int co=0;co<Cout;co++){
            double acc=0;
            for(int kh=0;kh<K;kh++)for(int kw=0;kw<K;kw++)for(int ci=0;ci<Cin;ci++){
                int y=oh+kh-P, x=ow+kw-P;
                if(y<0||y>=H||x<0||x>=W) continue;
                acc += (double)hi[(size_t)(x + y*W + ci*W*H)] * (double)hk[(size_t)(kw + kh*K + ci*K*K + co*K*K*Cin)];
            }
            double v=o[(size_t)(ow + oh*W + co*W*H)];
            double d=fabs(v-acc); maxe=std::max(maxe,d); rms+=d*d; ss+=acc*acc; cnt++;
            if(d>1e-3*std::max(1.0,fabs(acc))) bad++;
        }
        printf("%s: max_abs_err_vs_double=%.3g nmse=%.3g bad=%d/%d first=%.6f ref=%.6f\n",
               trial?"vulkan":"cpu   ",maxe,rms/ss,bad,cnt,(double)o[0],
               [&]{double acc=0;for(int kh=0;kh<K;kh++)for(int kw=0;kw<K;kw++)for(int ci=0;ci<Cin;ci++){int y=0+kh-P,x=0+kw-P;if(y<0||y>=H||x<0||x>=W)continue;acc+=(double)hi[x+y*W+ci*W*H]*hk[kw+kh*K+ci*K*K];}return acc;}());
        ggml_backend_buffer_free(buf); ggml_free(ctx);
    }
    return 0;
}
