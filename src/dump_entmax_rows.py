"""
Regenerate the alpha-entmax evaluation dataset on WikiText-2 (standard corpus).

Dumps real GPT-2 pre-softmax attention rows: for a sample of (layer, head,
position), the top-32 sorted scores and the true causal row length.

Method note: we do NOT use output_attentions (that returns POST-softmax
weights). We hook each layer's c_attn (the fused QKV projection), recompute
q @ k^T / sqrt(head_dim) ourselves, and apply the causal restriction by
slicing -- so the scores are the true pre-softmax logits regardless of which
attention kernel the model uses internally.

Outputs (in this directory):
  entmax_rows.npz  top32 (N,32) float32 | row_len,layer,head (N,) | meta
  entmax_u.bin     derived scaled gaps u_i = 0.5*(z_1 - z_i), float32, 32/row
"""
import numpy as np, torch, math, sys
from transformers import GPT2LMHeadModel, GPT2TokenizerFast

SEED, BLOCK, TARGET_TOKENS, TARGET_ROWS, K = 0, 512, 50_176, 200_000, 32
torch.manual_seed(SEED); rng = np.random.default_rng(SEED)
dev = "cuda" if torch.cuda.is_available() else "cpu"
print(f"device={dev}")

tok = GPT2TokenizerFast.from_pretrained("gpt2")
model = GPT2LMHeadModel.from_pretrained("gpt2", attn_implementation="eager").to(dev).eval()
cfg = model.config
NH, HD, NL = cfg.n_head, cfg.n_embd // cfg.n_head, cfg.n_layer
print(f"GPT-2: {NL} layers x {NH} heads, head_dim={HD}")

# ---- corpus: WikiText-2 (raw) ------------------------------------------------
# Primary: the datasets library. Fallback: direct parquet fetch, because some
# datasets versions (e.g. 5.0.0) reject the plain "wikitext" repo id.
def wikitext_lines():
    try:
                ds = load_dataset("wikitext", "wikitext-2-raw-v1", split="train")
        print("corpus via datasets.load_dataset")
        return (r["text"] for r in ds)
    except Exception as e:
        print(f"load_dataset failed ({type(e).__name__}); falling back to parquet")
        import pandas as pd
        url = ("https://huggingface.co/datasets/Salesforce/wikitext/resolve/main/"
               "wikitext-2-raw-v1/train-00000-of-00001.parquet")
        return iter(pd.read_parquet(url)["text"].tolist())

text = []
for t in wikitext_lines():
    t = t.strip()
    if t: text.append(t)
    if sum(len(x) for x in text) > 400_000: break
ids = tok("\n\n".join(text))["input_ids"][:TARGET_TOKENS]
nseq = len(ids) // BLOCK
seqs = torch.tensor(ids[:nseq*BLOCK]).view(nseq, BLOCK)
print(f"corpus: wikitext-2-raw-v1 -> {nseq} sequences x {BLOCK} tokens "
      f"= {nseq*BLOCK:,} tokens")

# ---- capture QKV per layer via hooks ----------------------------------------
cap = {}
def mk_hook(li):
    def h(mod, inp, out): cap[li] = out.detach()
    return h
for li, blk in enumerate(model.transformer.h):
    blk.attn.c_attn.register_forward_hook(mk_hook(li))

# rows available per sequence = NL*NH*(BLOCK-1); sample to hit TARGET_ROWS
per_seq = NL * NH * (BLOCK - 1)
keep_p = min(1.0, TARGET_ROWS / (nseq * per_seq))
print(f"sampling {100*keep_p:.2f}% of (layer,head,position) rows")

top32, row_len, layer_id, head_id = [], [], [], []
with torch.no_grad():
    for si in range(nseq):
        x = seqs[si:si+1].to(dev)
        model(x)
        for li in range(NL):
            qkv = cap[li]                                  # (1,T,3*n_embd)
            q, k, _ = qkv.split(cfg.n_embd, dim=2)
            q = q.view(1, BLOCK, NH, HD).permute(0, 2, 1, 3)   # (1,NH,T,HD)
            k = k.view(1, BLOCK, NH, HD).permute(0, 2, 1, 3)
            sc = (q @ k.transpose(-1, -2)) / math.sqrt(HD)     # (1,NH,T,T)
            sc = sc[0].float().cpu().numpy()
            for h in range(NH):
                pos = np.nonzero(rng.random(BLOCK - 1) < keep_p)[0] + 1  # skip pos 0
                for p in pos:
                    row = np.sort(sc[h, p, :p+1])[::-1][:K]    # causal + sorted desc
                    if row.size < K:
                        row = np.concatenate([row, np.full(K - row.size, -np.inf, np.float32)])
                    top32.append(row.astype(np.float32))
                    row_len.append(p + 1); layer_id.append(li); head_id.append(h)
        if (si + 1) % 10 == 0:
            print(f"  seq {si+1}/{nseq}  rows={len(top32):,}", flush=True)

top32 = np.stack(top32); row_len = np.array(row_len, np.int32)
layer_id = np.array(layer_id, np.int16); head_id = np.array(head_id, np.int16)
print(f"\ncollected {top32.shape[0]:,} rows")

np.savez_compressed("entmax_rows.npz", top32=top32, row_len=row_len,
                    layer=layer_id, head=head_id,
                    meta_block=np.array([BLOCK]),
                    meta_total_tokens=np.array([nseq*BLOCK]),
                    meta_seed=np.array([SEED]),
                    meta_corpus=np.array(["wikitext-2-raw-v1"]))

# ---- derived gaps for the C++ benchmark -------------------------------------
u = 0.5 * (top32[:, :1] - top32)
u[~np.isfinite(u)] = 1e9
np.minimum(u, 1e9, out=u)
u.astype(np.float32).tofile("entmax_u.bin")

# ---- integrity report --------------------------------------------------------
fin = np.isfinite(top32)
ok_sorted = all(np.all(np.diff(top32[i][fin[i]]) <= 1e-6) for i in range(0, len(top32), 500))
ok_len = all(fin[i].sum() == min(row_len[i], K) for i in range(0, len(top32), 500))
print(f"integrity: no NaN={not np.isnan(top32[fin]).any()}  "
      f"desc-sorted={ok_sorted}  row_len-consistent={ok_len}")
print(f"row_len: min={row_len.min()} max={row_len.max()} mean={row_len.mean():.1f}")
print(f"wrote entmax_rows.npz and entmax_u.bin")
