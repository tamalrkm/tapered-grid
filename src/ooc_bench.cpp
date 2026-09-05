// Out-of-core benchmark: tapered flat array BIGGER THAN RAM on an HDD laptop.
//
// The regime: value file (4N bytes) > total RAM, rotational disk. A point query
// = O(d) in-cache rank arithmetic + exactly ONE file access (page fault -> one
// ~8ms seek on HDD, or a page-cache hit). Any key-storing structure at this N
// cannot exist in RAM at all (measured hash footprint 43.5 B/entry -> ~242 GB),
// and on disk would pay >=2 DEPENDENT accesses per query (bucket + node).
//
// Modes (chain them: build && scan && query):
//   build  <B> <d>            create + fill file by rank (sequential write)
//   scan   <B> <d>            full sequential read verifying EVERY payload
//                             (= 100% correctness check + streaming-aggregate rate)
//   query  <B> <d> <M> <seed> M random sorted-key point queries via mmap,
//                             per-query latency distribution (bimodal: cache/disk)
//   walk   <B> <d> <M> <seed> lex|hashed
//                             M queries along a random walk in key space (one
//                             coordinate +-1 per step), addressed either by the
//                             lexicographic rank (this paper) or by a 64-bit
//                             mix of the key (what any hash-addressed layout
//                             does). Page-cache hits are decided by mincore()
//                             BEFORE the access, not by timing.
//
// build: g++ -O3 -march=native -o ooc_bench ooc_bench.cpp
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

using u64 = uint64_t;
using clk = std::chrono::steady_clock;
static const char* PATH = "ooc_memo.bin";

static u64 C[64][64];
static void pascal(){ for(int n=0;n<64;n++){C[n][0]=1;
  for(int k=1;k<=n;k++)C[n][k]=C[n-1][k-1]+(k<=n-1?C[n-1][k]:0);} }

static inline u64 rank_fixed(const int* cols,int d,int B){
  int mp=1; u64 r=0;
  for(int i=1;i<=d;i++){ int mi=cols[i-1],k=d-i;
    r+=C[B-mp+k+1][k+1]-C[B-mi+k+1][k+1]; mp=mi; }
  return r;
}
static inline float value_of(u64 r){
  u64 h=r*2654435761ull; return float(h&0xFFFFFF)*(1.0f/16777216.0f);
}
static inline u64 xs(u64&s){ s^=s<<13; s^=s>>7; s^=s<<17; return s; }
static void gen_query(u64&s,int d,int B,int*out){
  for(int i=0;i<d;i++) out[i]=int(xs(s)%B)+1;
  std::sort(out,out+d);
}

int main(int argc,char**argv){
  if(argc<4){fprintf(stderr,"usage: %s build|scan|query|walk B d [M seed [lex|hashed]]\n",argv[0]);return 1;}
  std::string mode=argv[1]; int B=atoi(argv[2]), d=atoi(argv[3]);
  pascal();
  u64 N=C[B+d-1][d];
  double gb=double(N)*4/1e9;
  printf("[%s] B=%d d=%d  N=%llu  file=%.2f GB\n",mode.c_str(),B,d,(unsigned long long)N,gb);
  fflush(stdout);

  if(mode=="build"){
    auto t0=clk::now();
    int fd=open(PATH,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd<0){perror("open");return 1;}
    const u64 CHUNK=1u<<20;                     // 1M floats = 4MB
    std::vector<float> buf(CHUNK);
    for(u64 base=0;base<N;base+=CHUNK){
      u64 n=std::min(CHUNK,N-base);
      for(u64 j=0;j<n;j++) buf[j]=value_of(base+j);
      if(write(fd,buf.data(),n*4)!=(ssize_t)(n*4)){perror("write");return 1;}
      if(((base/CHUNK)&255)==255)               // every ~1GB: flush + drop cache
        { fdatasync(fd); posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED); }
    }
    fdatasync(fd); posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED); close(fd);
    double s=std::chrono::duration<double>(clk::now()-t0).count();
    printf("  build=%.1fs  (%.0f Mentries/s, %.0f MB/s)\n",s,N/s/1e6,gb*1000/s);
  }
  else if(mode=="scan"){
    auto t0=clk::now();
    int fd=open(PATH,O_RDONLY); if(fd<0){perror("open");return 1;}
    posix_fadvise(fd,0,0,POSIX_FADV_SEQUENTIAL);
    const u64 CHUNK=1u<<20; std::vector<float> buf(CHUNK);
    u64 bad=0; double acc=0;
    for(u64 base=0;base<N;base+=CHUNK){
      u64 n=std::min(CHUNK,N-base);
      if(read(fd,buf.data(),n*4)!=(ssize_t)(n*4)){perror("read");return 1;}
      for(u64 j=0;j<n;j++){ if(buf[j]!=value_of(base+j)) bad++; acc+=buf[j]; }
    }
    close(fd);
    double s=std::chrono::duration<double>(clk::now()-t0).count();
    printf("  scan=%.1fs  (%.1f Mentries/s streamed)  MISMATCHES=%llu  acc=%.3f\n",
           s,N/s/1e6,(unsigned long long)bad,acc);
  }
  else if(mode=="query"){
    long M = argc>4?atol(argv[4]):20000;
    u64 seed = argc>5?atoll(argv[5]):42;
    // pre-generate keys + ranks (excluded from timing)
    std::vector<u64> ranks(M);
    { u64 s=seed|1; std::vector<int> c(d);
      for(long q=0;q<M;q++){ gen_query(s,d,B,c.data()); ranks[q]=rank_fixed(c.data(),d,B); } }
    int fd=open(PATH,O_RDONLY); if(fd<0){perror("open");return 1;}
    float* arr=(float*)mmap(nullptr,N*4,PROT_READ,MAP_SHARED,fd,0);
    if(arr==MAP_FAILED){perror("mmap");return 1;}
    madvise(arr,N*4,MADV_RANDOM);
    std::vector<double> lat(M); double checksum=0;
    auto T0=clk::now();
    for(long q=0;q<M;q++){
      auto t0=clk::now();
      checksum+=arr[ranks[q]];
      lat[q]=std::chrono::duration<double,std::micro>(clk::now()-t0).count();
    }
    double total=std::chrono::duration<double>(clk::now()-T0).count();
    munmap(arr,N*4); close(fd);
    // correctness on the sampled ranks
    u64 bad=0; for(long q=0;q<M;q++){} // values checked via checksum vs recompute:
    double expect=0; for(long q=0;q<M;q++) expect+=value_of(ranks[q]);
    std::vector<double> sorted=lat; std::sort(sorted.begin(),sorted.end());
    auto pct=[&](double p){ return sorted[size_t(p*(M-1))]; };
    long hits=0; for(long q=0;q<M;q++) if(lat[q]<100.0) hits++;   // <100us = page-cache
    printf("  M=%ld  total=%.1fs  mean=%.0fus  p50=%.0fus p90=%.0fus p99=%.0fus\n",
           M,total,1e6*total/M,pct(.50),pct(.90),pct(.99));
    printf("  cache-hit(<100us)=%.1f%%  disk(>=100us)=%.1f%%  disk-mean=%.1fms\n",
           100.0*hits/M,100.0*(M-hits)/M,
           [&]{double s=0;long n=0;for(long q=0;q<M;q++)if(lat[q]>=100){s+=lat[q];n++;}return n? s/n/1000:0;}());
    printf("  checksum=%.6f expect=%.6f  %s\n",checksum,expect,
           fabs(checksum-expect)<1e-3?"CORRECT":"MISMATCH");
    (void)bad;
  }

  else if(mode=="walk"){
    long M = argc>4?atol(argv[4]):20000;
    u64 seed = argc>5?atoll(argv[5]):42;
    std::string addr = argc>6?argv[6]:"lex";
    bool hashed = (addr=="hashed");
    auto mix64=[](u64 x){ x+=0x9e3779b97f4a7c15ull; x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;
      x=(x^(x>>27))*0x94d049bb133111ebull; return x^(x>>31); };
    // ---- generate the walk (excluded from timing) ----
    std::vector<u64> ranks(M), addrs(M); std::vector<int> coord(M);
    { u64 s=seed|1; std::vector<int> c(d); gen_query(s,d,B,c.data());
      long q=0, tries=0;
      while(q<M){
        tries++;
        int i=int(xs(s)%d); int sg=(xs(s)&1)?1:-1; int v=c[i]+sg;
        bool ok = v>=1 && v<=B && (i==0||c[i-1]<=v) && (i==d-1||v<=c[i+1]);
        if(!ok) continue;
        c[i]=v; ranks[q]=rank_fixed(c.data(),d,B); coord[q]=i;
        addrs[q]= hashed ? (mix64(ranks[q])%N) : ranks[q];
        q++;
      }
      printf("  walk: %ld steps (%ld proposals), addressing=%s\n",M,tries,addr.c_str());
    }
    // ---- open, evict, measure residency before the run ----
    int fd=open(PATH,O_RDONLY); if(fd<0){perror("open");return 1;}
    posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);
    float* arr=(float*)mmap(nullptr,N*4,PROT_READ,MAP_SHARED,fd,0);
    if(arr==MAP_FAILED){perror("mmap");return 1;}
    madvise(arr,N*4,MADV_RANDOM);
    const u64 PG=4096, pages=(N*4+PG-1)/PG;
    { std::vector<unsigned char> vec(pages); u64 res=0;
      if(mincore(arr,N*4,vec.data())==0){ for(u64 i=0;i<pages;i++) res+=vec[i]&1; }
      printf("  resident before run: %.2f%% of %llu pages\n",100.0*res/pages,(unsigned long long)pages); }
    // ---- run ----
    std::vector<double> lat(M); std::vector<char> hit(M); double checksum=0;
    std::vector<long> stepsPer(d,0), hitsPer(d,0), samePagePer(d,0);
    u64 prevPage=~0ull; long samePage=0;
    auto T0=clk::now();
    for(long q=0;q<M;q++){
      u64 a=addrs[q]; u64 pg=(a*4)/PG;
      unsigned char v=0; char* pa=(char*)arr+pg*PG;
      mincore(pa,PG,&v); hit[q]=(v&1)?1:0;
      auto t0=clk::now();
      checksum+=arr[a];
      lat[q]=std::chrono::duration<double,std::micro>(clk::now()-t0).count();
      stepsPer[coord[q]]++; if(hit[q]) hitsPer[coord[q]]++;
      if(pg==prevPage){ samePage++; samePagePer[coord[q]]++; }
      prevPage=pg;
    }
    double total=std::chrono::duration<double>(clk::now()-T0).count();
    // distinct pages touched
    { std::vector<u64> pgs(M); for(long q=0;q<M;q++) pgs[q]=(addrs[q]*4)/PG;
      std::sort(pgs.begin(),pgs.end()); long distinct=std::unique(pgs.begin(),pgs.end())-pgs.begin();
      printf("  distinct pages touched: %ld of %ld steps\n",distinct,M); }
    munmap(arr,N*4); close(fd);
    long hits=0; double sHit=0,sMiss=0; long nMiss=0;
    for(long q=0;q<M;q++){ if(hit[q]){hits++;sHit+=lat[q];} else {nMiss++;sMiss+=lat[q];} }
    std::vector<double> sorted=lat; std::sort(sorted.begin(),sorted.end());
    auto pct=[&](double p){ return sorted[size_t(p*(M-1))]; };
    printf("  M=%ld total=%.1fs mean=%.0fus p50=%.0fus p90=%.0fus p99=%.0fus\n",M,total,1e6*total/M,pct(.5),pct(.9),pct(.99));
    printf("  page-cache hit (mincore) = %.1f%%   same-page-as-previous = %.1f%%\n",100.0*hits/M,100.0*samePage/M);
    printf("  hit-mean=%.1fus  miss-mean=%.2fms (n=%ld)\n",hits?sHit/hits:0,nMiss?sMiss/nMiss/1000:0,nMiss);
    printf("  per-coordinate (1=leading): coord steps hit%% samepage%%\n");
    for(int i=0;i<d;i++) printf("    %2d %6ld %6.1f %6.1f\n",i+1,stepsPer[i],stepsPer[i]?100.0*hitsPer[i]/stepsPer[i]:0,stepsPer[i]?100.0*samePagePer[i]/stepsPer[i]:0);
    double expect=0; for(long q=0;q<M;q++) expect+=value_of(ranks[q]*(hashed?0:1)+(hashed?addrs[q]:0));
    printf("  checksum=%.6f expect=%.6f  %s\n",checksum,expect,fabs(checksum-expect)<1e-3?"CORRECT":"MISMATCH");
  }
  else { fprintf(stderr,"unknown mode\n"); return 1; }
  return 0;
}
