// alpha-entmax threshold memoization: accuracy/size/latency on REAL GPT-2 rows.
//
// f(u) = theta solving sum_i (theta - u_i)_+^2 = 1, where u_i = 0.5(z_1 - z_i)
// are the scaled sorted logit gaps (u_1 = 0, non-decreasing). Entries with
// u_i >= theta contribute EXACTLY zero -> the tapered tail is exact, not
// approximate. Domain is [0,1] (theta <= 1 always), first coordinate pinned.
//
// Compares, on the same 199,669 real rows, in the same language:
//   memo    : O(d) interpolate + O(d) rank + one array read
//   exact   : sorted-support closed form (the fast exact algorithm)
//   bisect  : 60-iteration bisection (what a naive implementation does)
//
// build: g++ -O3 -march=native -o entmax_bench entmax_bench.cpp
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>

using u64 = uint64_t;
using clk = std::chrono::steady_clock;
static const int K = 32;                 // stored gaps per row

static u64 C[80][80];
static void pascal(){ for(int n=0;n<80;n++){C[n][0]=1;
  for(int k=1;k<=n;k++)C[n][k]=C[n-1][k-1]+(k<=n-1?C[n-1][k]:0);} }

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

// ---- table-free rank, first coordinate pinned (cols c_2..c_d over 1..B) ----
static inline u64 rank_pin(const int* c,int d,int B){
  int mp=1; u64 r=0; int m=d-1;                 // m free coordinates
  for(int i=1;i<=m;i++){ int mi=c[i-1],k=m-i;
    r+=C[B-mp+k+1][k+1]-C[B-mi+k+1][k+1]; mp=mi; }
  return r;
}
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
  if(argc<4){fprintf(stderr,"usage: %s B d gamma\n",argv[0]);return 1;}
  int B=atoi(argv[1]), d=atoi(argv[2]); double gamma=atof(argv[3]);
  pascal();
  std::vector<double> s(B+1);                    // s[1..B], tapered in [0,1]
  for(int j=1;j<=B;j++) s[j]=std::pow((double)(j-1)/(B-1), gamma);

  // load real rows
  FILE* f=fopen("entmax_u.bin","rb"); if(!f){perror("entmax_u.bin");return 1;}
  fseek(f,0,SEEK_END); long bytes=ftell(f); fseek(f,0,SEEK_SET);
  long N=bytes/(K*sizeof(float));
  std::vector<float> U((size_t)N*K);
  if(fread(U.data(),1,bytes,f)!=(size_t)bytes){perror("read");return 1;}
  fclose(f);

  u64 M=C[B+d-2][d-1];                           // table size (coord-1 pinned)
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
  { std::vector<int> c(d-1);
    for(long q=0;q<N;q++){
      const float* u=&U[(size_t)q*K];
      double tt=theta_exact(u,K);
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

  // ---- latency: single-row, same language, same rows ----------------------
  long Msub=std::min(N,100000L);
  std::vector<int> c(d-1); volatile double sink=0;
  t0=clk::now();
  for(long q=0;q<Msub;q++){ const float* u=&U[(size_t)q*K];
    snap(u,d,B,s.data(),c.data()); sink+=tab[rank_pin(c.data(),d,B)]; }
  double lat_memo=std::chrono::duration<double,std::nano>(clk::now()-t0).count()/Msub;
  t0=clk::now();
  for(long q=0;q<Msub;q++) sink+=theta_exact(&U[(size_t)q*K],K);
  double lat_exact=std::chrono::duration<double,std::nano>(clk::now()-t0).count()/Msub;
  t0=clk::now();
  for(long q=0;q<Msub;q++) sink+=theta_bisect(&U[(size_t)q*K],K);
  double lat_bis=std::chrono::duration<double,std::nano>(clk::now()-t0).count()/Msub;

  printf("  build=%.1fs\n",build_s);
  printf("  theta err: mean=%.2e p99=%.2e max=%.2e | bias=%+.2e (%.3fx mean|err|)\n",
         emean,se[(size_t)(0.99*(N-1))],emax,bias,std::fabs(bias)/emean);
  printf("  downstream max|dp|: mean=%.2e max=%.2e\n",dpmax_mean,dpmax_max);
  printf("  latency ns/row: memo=%.0f  exact=%.0f  bisect=%.0f  "
         "(memo speedup: %.1fx exact, %.1fx bisect)\n",
         lat_memo,lat_exact,lat_bis,lat_exact/lat_memo,lat_bis/lat_memo);
  (void)sink;
  return 0;
}
