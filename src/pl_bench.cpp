// Plackett-Luce / vectorized-root memoization: the paper's canonical f.
//
// f(x) = p1 solving  sum_i p1^{1/a_i} = 1,  a_i = 1 - x_i,  x_1 = 0,
// x non-decreasing in [0,1) (the 2015 paper's own transform). No closed form
// exists -> every exact evaluation is an iterative root-find with transcendental
// pow() calls. Queries drawn from the 2015 paper's own generator (Sec. 5):
//   p=0; x_1=0; repeat: r ~ U(p,1); x_next = p + (r-p)/b; p = x_next.
//
// build: g++ -O3 -march=native -o pl_bench pl_bench.cpp
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>

using u64 = uint64_t;
using clk = std::chrono::steady_clock;
static const int KMAX = 32;                 // query vector length

static u64 C[80][80];
static void pascal(){ for(int n=0;n<80;n++){C[n][0]=1;
  for(int k=1;k<=n;k++)C[n][k]=C[n-1][k-1]+(k<=n-1?C[n-1][k]:0);} }

// ---- exact f: Newton on sum p^{b_i} = 1, b_i = 1/(1-x_i) ------------------
static inline double p1_newton(const double* x,int n){
  double p=0.5;
  for(int it=0;it<40;it++){
    double s=0,ds=0;
    for(int i=0;i<n;i++){
      double b=1.0/std::max(1e-12,1.0-x[i]);
      double t=std::pow(p,b); s+=t; ds+=b*t/p;
    }
    double step=(s-1.0)/ds; p-=step;
    if(p<=1e-12)p=1e-12; if(p>=1)p=1-1e-12;
    if(std::fabs(step)<1e-13) break;
  }
  return p;
}
static inline double p1_bisect(const double* x,int n){
  double lo=0,hi=1;
  for(int it=0;it<50;it++){
    double m=0.5*(lo+hi),s=0;
    for(int i=0;i<n;i++) s+=std::pow(m,1.0/std::max(1e-12,1.0-x[i]));
    if(s<1.0) lo=m; else hi=m;
  }
  return 0.5*(lo+hi);
}

static inline u64 rank_pin(const int* c,int d,int B){
  int mp=1; u64 r=0; int m=d-1;
  for(int i=1;i<=m;i++){ int mi=c[i-1],k=m-i;
    r+=C[B-mp+k+1][k+1]-C[B-mi+k+1][k+1]; mp=mi; }
  return r;
}
static inline void snap(const double* x,int d,int B,const double* s,int* c){
  int lo=1;
  for(int i=1;i<d;i++){
    double xi=std::min(x[i],1.0);
    int j=lo;
    while(j+1<=B && std::fabs(s[j+1]-xi) < std::fabs(s[j]-xi)) j++;
    c[i-1]=j; lo=j;
  }
}

int main(int argc,char**argv){
  if(argc<4){fprintf(stderr,"usage: %s B d gamma [bparam]\n",argv[0]);return 1;}
  int B=atoi(argv[1]), d=atoi(argv[2]); double gamma=atof(argv[3]);
  double bp = argc>4? atof(argv[4]) : 5.0;
  pascal();
  std::vector<double> s(B+1);
  for(int j=1;j<=B;j++) s[j]=std::pow((double)(j-1)/(B-1), gamma);

  // ---- query set: the 2015 paper's generator ------------------------------
  long N=200000; std::vector<double> X((size_t)N*KMAX);
  { unsigned st=12345;
    for(long q=0;q<N;q++){ double p=0; X[(size_t)q*KMAX]=0;
      for(int i=1;i<KMAX;i++){
        st=st*1103515245u+12345u; double r=p+((st>>16)/65535.0)*(1.0-p);
        p=p+(r-p)/bp; X[(size_t)q*KMAX+i]=std::min(p,0.999999);
      } } }

  u64 M=C[B+d-2][d-1];
  printf("B=%d d=%d gamma=%.1f b=%.1f  |U|=%llu (%.1f MB)  queries=%ld\n",
         B,d,gamma,bp,(unsigned long long)M,M*4/1e6,N);
  fflush(stdout);

  // ---- build ---------------------------------------------------------------
  auto t0=clk::now();
  std::vector<float> tab(M);
  { std::vector<int> c(d-1,1); std::vector<double> g(d);
    for(u64 done=0;done<M;done++){
      g[0]=0.0; for(int i=1;i<d;i++) g[i]=s[c[i-1]];
      tab[rank_pin(c.data(),d,B)]=(float)p1_newton(g.data(),d);
      int i=d-2; while(i>=0&&c[i]==B) i--;
      if(i<0) break; c[i]++; for(int j=i+1;j<d-1;j++) c[j]=c[i];
    } }
  double build_s=std::chrono::duration<double>(clk::now()-t0).count();

  // ---- accuracy ------------------------------------------------------------
  std::vector<double> err(N); double bias=0,emax=0,emean=0;
  { std::vector<int> c(d-1);
    for(long q=0;q<N;q++){
      const double* x=&X[(size_t)q*KMAX];
      double tt=p1_newton(x,KMAX);
      snap(x,d,B,s.data(),c.data());
      double th=tab[rank_pin(c.data(),d,B)];
      double e=th-tt; bias+=e; err[q]=std::fabs(e);
      emax=std::max(emax,std::fabs(e)); emean+=std::fabs(e);
    } }
  bias/=N; emean/=N;
  std::vector<double> se=err; std::sort(se.begin(),se.end());

  // ---- latency (single-query, same language) -------------------------------
  long Msub=std::min(N,100000L); std::vector<int> c(d-1); volatile double sink=0;
  t0=clk::now();
  for(long q=0;q<Msub;q++){ const double* x=&X[(size_t)q*KMAX];
    snap(x,d,B,s.data(),c.data()); sink+=tab[rank_pin(c.data(),d,B)]; }
  double lat_memo=std::chrono::duration<double,std::nano>(clk::now()-t0).count()/Msub;
  t0=clk::now();
  for(long q=0;q<Msub;q++) sink+=p1_newton(&X[(size_t)q*KMAX],KMAX);
  double lat_newton=std::chrono::duration<double,std::nano>(clk::now()-t0).count()/Msub;
  t0=clk::now();
  for(long q=0;q<Msub;q++) sink+=p1_bisect(&X[(size_t)q*KMAX],KMAX);
  double lat_bis=std::chrono::duration<double,std::nano>(clk::now()-t0).count()/Msub;

  printf("  build=%.1fs\n",build_s);
  printf("  p1 err: mean=%.2e p50=%.2e p99=%.2e max=%.2e | BIAS=%+.2e (%.3fx mean|err|)\n",
         emean,se[N/2],se[(size_t)(0.99*(N-1))],emax,bias,std::fabs(bias)/emean);
  printf("  latency ns/query: memo=%.0f  Newton=%.0f  bisect=%.0f  "
         "-> memo speedup %.1fx (Newton), %.1fx (bisect)\n",
         lat_memo,lat_newton,lat_bis,lat_newton/lat_memo,lat_bis/lat_memo);
  (void)sink;
  return 0;
}
