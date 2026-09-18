import re,sys,statistics,collections
txt=open(sys.argv[1]).read()
recs=collections.defaultdict(list)
for m in re.finditer(r'mode=(\S+)\s+B=(\d+) d=(\d+)\s+N=(\d+)\s+M=\d+\n(?:.*\n)?\s+build=([\d.]+)s\s+latency=([\d.]+) ns/query\s+peakRSS=(\d+) MB',txt):
    mode,B,d,N,b,lat,rss=m.groups()
    recs[(mode,int(B),int(d))].append((float(lat),int(rss),float(b)))
print(f"{'mode':14s} {'B':>2s} {'d':>2s} {'n':>1s} {'lat med':>8s} {'min':>6s} {'max':>6s} {'rss':>5s} {'build':>6s}")
for k in sorted(recs,key=lambda k:(k[2],k[1],k[0])):
    v=recs[k]; lats=[x[0] for x in v]
    print(f"{k[0]:14s} {k[1]:>2d} {k[2]:>2d} {len(v)} {statistics.median(lats):8.1f} {min(lats):6.1f} {max(lats):6.1f} {int(statistics.median(x[1] for x in v)):5d} {statistics.median(x[2] for x in v):6.2f}")
