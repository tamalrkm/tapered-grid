"""Per-coordinate page-cache hit rate: lexicographic vs hashed addressing."""
import re, sys
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
src = sys.argv[1] if len(sys.argv) > 1 else '../results_walk.txt'
data = {}
for r in re.split(r'\[walk\]', open(src).read())[1:]:
    addr = re.search(r'addressing=(\w+)', r).group(1)
    rows = re.findall(r'^\s+(\d+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s*$', r, re.M)
    key = addr if addr not in data else addr + '2'
    data[key] = [(int(a), float(c), float(d)) for a, b, c, d in rows]
plt.rcParams.update({'font.size': 9, 'font.family': 'serif',
                     'font.serif': ['Nimbus Roman', 'Liberation Serif'],
                     'mathtext.fontset': 'stix', 'axes.unicode_minus': False})
fig, ax = plt.subplots(figsize=(4.6, 2.8))
for key, lab, st in [('lex', 'lexicographic address (this paper)', '-o'),
                     ('lex2', 'lexicographic, second seed', '--'),
                     ('hashed', 'hashed address', '-s')]:
    if key not in data:
        continue
    ax.plot([a for a, h, sp in data[key]], [h for a, h, sp in data[key]],
            st, ms=3.5, lw=1.2, label=lab)
ax.set_xlabel('coordinate changed by the step (1 = leading, 15 = last)')
ax.set_ylabel('page-cache hit rate (%)')
ax.set_xticks(range(1, 16)); ax.set_ylim(0, 105); ax.grid(alpha=.3)
ax.legend(frameon=False, loc='upper left')
fig.tight_layout(); fig.savefig('figs/locality.pdf')
print('locality.pdf written')
