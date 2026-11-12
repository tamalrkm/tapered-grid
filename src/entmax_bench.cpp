// alpha-entmax threshold memoization: accuracy/size/latency on REAL GPT-2 rows.
//
// f(u) = theta solving sum_i (theta - u_i)_+^2 = 1, where u_i = 0.5(z_1 - z_i)
// are the scaled sorted logit gaps (u_1 = 0, non-decreasing). Entries with
// u_i >= theta contribute EXACTLY zero -> the tapered tail is exact, not
// approximate. Domain is [0,1] (theta <= 1 always), first coordinate pinned.
//
// Compares, on the same real rows, in the same language:
//   memo    : O(d) interpolate + O(d) rank + one array read
//   exact   : sorted-support closed form (the fast exact algorithm)
//   bisect  : 60-iteration bisection (what a naive implementation does)
//
// usage: entmax_bench B d gamma [reps=1] [rows=entmax_u.bin]
// Output is JSON lines for scripts/run.py; the latency loops can be repeated
// in-process.
#include "common.hpp"
#include "tg.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>

static const int K = 32;                 // stored gaps per row


// ---- exact alpha=1.5 threshold: try support sizes on the sorted gaps -------
static inline double theta_exact(const float* u, int n){
  double S1=0,S2=0;
  for(int k=1;k<=n;k++){
    double uk=u[k-1];
    if(uk>=1e8) break;                    // padding
    S1+=uk; S2+=uk*uk;
    double disc=S1*S1-k*(S2-1.0);
    if(disc<0) continue;
    double th=(S1+std::sqrt(disc))/k;
    double nxt=(k<n && u[k]<1e8)? (double)u[k] : 1e18;
    if(uk<th && th<=nxt+1e-12) return th;
  }
  double S1b=0,S2b=0; int k=0;
  for(int i=0;i<n && u[i]<1e8;i++){ S1b+=u[i]; S2b+=(double)u[i]*u[i]; k++; }
  double disc=S1b*S1b-k*(S2b-1.0); if(disc<0) disc=0;
  return (S1b+std::sqrt(disc))/k;
}
static inline double theta_bisect(const float* u,int n){
  double lo=0,hi=1;
  for(int it=0;it<60;it++){
    double t=0.5*(lo+hi), s=0;
    for(int i=0;i<n;i++){ double dd=t-u[i]; if(dd>0) s+=dd*dd; else break; }
    if(s<1.0) lo=t; else hi=t;
  }
  return 0.5*(lo+hi);
}

// ---- the library's rank; the first coordinate is pinned, so d-1 are stored --
static inline u64 rank_pin(const int* c,int d,int B){ return tg::rank(c,d-1,B,tg::pascal()); }
// ---- monotone nearest-grid interpolation (paper's Interpolate, wt=0) ------
static inline void snap(const float* u,int d,int B,const double* s,int* c){
  int lo=1;
  for(int i=1;i<d;i++){
    double ui = (u[i]<1e8)? std::min((double)u[i],1.0) : 1.0;
    int j=lo;
    while(j+1<=B && std::fabs(s[j+1]-ui) < std::fabs(s[j]-ui)) j++;
    c[i-1]=j; lo=j;
  }
}

int main(int argc,char**argv){
  if(argc<4){fprintf(stderr,"usage: %s B d gamma [reps] [rows]\n",argv[0]);return 1;}
  int B=atoi(argv[1]), d=atoi(argv[2]); double gamma=atof(argv[3]);
  int reps = argc>4 ? atoi(argv[4]) : 1;
  const char* rows_path = argc>5 ? argv[5] : "entmax_u.bin";
  std::vector<double> s(B+1);                    // s[1..B], tapered in [0,1]
  for(int j=1;j<=B;j++) s[j]=std::pow((double)(j-1)/(B-1), gamma);

  // load real rows
  FILE* f=fopen(rows_path,"rb"); if(!f){perror(rows_path);return 1;}
  fseek(f,0,SEEK_END); long bytes=ftell(f); fseek(f,0,SEEK_SET);
  long N=bytes/(K*sizeof(float));
  std::vector<float> U((size_t)N*K);
  if(fread(U.data(),1,bytes,f)!=(size_t)bytes){perror("read");return 1;}
  fclose(f);

  u64 M=tg::Shape{B,d-1}.size();                 // table size (coord-1 pinned)
  printf("B=%d d=%d gamma=%.1f  |U|=%llu (%.1f MB)  rows=%ld\n",
         B,d,gamma,(unsigned long long)M,M*4/1e6,N);
  fflush(stdout);

  // ---- build table: theta at every gridpoint, filled by rank via odometer --
  auto t0=clk::now();
  std::vector<float> tab(M);
  { std::vector<int> c(d-1,1); std::vector<float> g(d);
    for(u64 done=0;done<M;done++){
      g[0]=0.f; for(int i=1;i<d;i++) g[i]=(float)s[c[i-1]];
      tab[rank_pin(c.data(),d,B)]=(float)theta_exact(g.data(),d);
      int i=d-2; while(i>=0&&c[i]==B) i--;
      if(i<0) break; c[i]++; for(int j=i+1;j<d-1;j++) c[j]=c[i];
    } }
  double build_s=std::chrono::duration<double>(clk::now()-t0).count();

  // ---- accuracy over ALL real rows ----------------------------------------
  std::vector<double> err(N); double bias=0,emax=0;
  double dpmax_mean=0, dpmax_max=0;
  std::vector<int> support(N);                    // entries below the threshold, per row
  { std::vector<int> c(d-1);
    for(long q=0;q<N;q++){
      const float* u=&U[(size_t)q*K];
      double tt=theta_exact(u,K);
      { int k=0; while(k<K && u[k]<tt) k++; support[q]=k; }
      snap(u,d,B,s.data(),c.data());
      double th=tab[rank_pin(c.data(),d,B)];
      double e=th-tt; bias+=e; err[q]=std::fabs(e); emax=std::max(emax,std::fabs(e));
      double dm=0;                                  // downstream prob error
      for(int i=0;i<K;i++){
        double a=th-u[i], b=tt-u[i];
        a=(a>0?a*a:0); b=(b>0?b*b:0);
        dm=std::max(dm,std::fabs(a-b));
      }
      dpmax_mean+=dm; dpmax_max=std::max(dpmax_max,dm);
    } }
  bias/=N; dpmax_mean/=N;
  std::vector<double> se=err; std::sort(se.begin(),se.end());
  double emean=0; for(double e:err) emean+=e; emean/=N;

  J("entmax_bench").s("what","accuracy").i("B",B).i("d",d).u("N",M).i("rows",N).f("build_s",build_s)
    .f("err_mean",emean).f("err_p99",se[(size_t)(0.99*(N-1))]).f("err_max",emax)
    .f("bias",bias).f("bias_over_mean",std::fabs(bias)/emean)
    .f("dp_max_mean",dpmax_mean).f("dp_max_max",dpmax_max)
    .f("support_mean",[&]{double t=0;for(int v:support)t+=v;return t/N;}())
    .f("support_median",[&]{std::nth_element(support.begin(),support.begin()+N/2,support.end());return double(support[N/2]);}())
    .emit();

  // ---- latency: single-row, same language, same rows ----------------------
  long Msub=std::min(N,100000L);
  std::vector<int> c(d-1); volatile double sink=0;
  for(int rep=0;rep<reps;rep++){
    auto t0=clk::now();
    for(long q=0;q<Msub;q++){ const float* u=&U[(size_t)q*K];
      snap(u,d,B,s.data(),c.data()); sink=sink+tab[rank_pin(c.data(),d,B)]; }
    double lat_memo=std::chrono::duration<double,std::nano>(clk::now()-t0).count()/Msub;
    t0=clk::now();
    for(long q=0;q<Msub;q++) sink=sink+theta_exact(&U[(size_t)q*K],K);
    double lat_exact=std::chrono::duration<double,std::nano>(clk::now()-t0).count()/Msub;
    t0=clk::now();
    for(long q=0;q<Msub;q++) sink=sink+theta_bisect(&U[(size_t)q*K],K);
    double lat_bis=std::chrono::duration<double,std::nano>(clk::now()-t0).count()/Msub;
    J("entmax_bench").s("what","latency").i("rep",rep).i("B",B).i("d",d)
      .f("memo_ns",lat_memo).f("exact_ns",lat_exact).f("bisect_ns",lat_bis)
      .f("memo_over_exact",lat_memo/lat_exact).emit();
  }
  return 0;
}
