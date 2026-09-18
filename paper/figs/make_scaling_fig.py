"""Latency and peak RSS vs N: desktop sweep (median over repetitions) + server sweep."""
import re,sys,statistics,collections
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
src=sys.argv[1] if len(sys.argv)>1 else '../results_median3.txt'
txt=open(src).read()
recs=collections.defaultdict(list)   # (mode,B,d) -> [(lat,rss)]
for m in re.finditer(r'mode=(\S+)\s+B=(\d+) d=(\d+)\s+N=(\d+)\s+M=\d+\n(?:.*\n)?\s+build=[\d.]+s\s+latency=([\d.]+) ns/query\s+peakRSS=(\d+) MB',txt):
    mode,B,d,N,lat,rss=m.groups()
    recs[(mode,int(B),int(d))].append((float(lat),int(rss),int(N)))
def med(mode,d):
    pts=[]
    for (mo,B,dd),v in recs.items():
        if mo==mode and dd==d and len(v)>0:
            pts.append((v[0][2],statistics.median(x[0] for x in v),statistics.median(x[1] for x in v),len(v)))
    return sorted(pts)
plt.rcParams.update({'font.size':9,'font.family':'serif'})
fig,(a1,a2)=plt.subplots(1,2,figsize=(6.4,2.7))
series=[('tapered-ram','values-only array','-o'),('oahash','open-addressing flat table','-s'),('hash','std::unordered\\_map'.replace('\\_','_'),'-^')]
for mode,lab,st in series:
    pts=med(mode,10)
    if not pts: continue
    a1.plot([p[0] for p in pts],[p[1] for p in pts],st,ms=3.5,lw=1.2,label=lab)
    a2.plot([p[0] for p in pts],[p[2] for p in pts],st,ms=3.5,lw=1.2,label=lab)
# server sweep (Table 4 of the paper): N, tapered ns, hash ns, tapered RSS GB, hash RSS GB
srv=[(92561040,98.9,110.8,0.737,4.189),(472733756,144.3,150.6,2.187,19.540),(1121099408,149.6,183.4,4.660,45.843),(1917334783,153.6,282.8,7.698,78.406)]
a1.plot([r[0] for r in srv],[r[1] for r in srv],':o',ms=3,lw=1,color='C0',alpha=.7,label='array, server')
a1.plot([r[0] for r in srv],[r[2] for r in srv],':^',ms=3,lw=1,color='C2',alpha=.7,label='unordered_map, server')
a2.plot([r[0] for r in srv],[r[3]*1024 for r in srv],':o',ms=3,lw=1,color='C0',alpha=.7)
a2.plot([r[0] for r in srv],[r[4]*1024 for r in srv],':^',ms=3,lw=1,color='C2',alpha=.7)
for a in (a1,a2): a.set_xscale('log'); a.grid(alpha=.3); a.set_xlabel('$N$ (entries)')
a1.set_ylabel('query latency (ns)'); a2.set_ylabel('peak RSS (MB)'); a2.set_yscale('log')
a1.legend(frameon=False,fontsize=7,loc='upper left')
fig.tight_layout(); fig.savefig('figs/scaling.pdf')
n=[p[3] for p in med('tapered-ram',10)]
print('scaling.pdf written; repetitions per desktop point:',n)
