tg::Shape   sh{16, 11};              // B = 16, 11 stored coordinates
tg::Spacing sp = tg::Spacing::power(16, 2.0);       // s_j = ((j-1)/15)^2
tg::Meta    meta;                    // recorded in the file header
meta.pin_first = 1;                  // x[0] = 0 is not stored
meta.spacing_kind = 1;  meta.spacing_param = 2.0;

// Build once: 4 threads, each owning a range of indices; no locks.
auto t = tg::Table<float>::create("root.tg", sh, meta);
const double* s = sp.s.data();
t.fill([&](const int* m, int d) { return vector_root(m, d, s); }, 4);
t.sync();                            // flush, record the payload checksum

// Serve: map read-only; header validated; MADV_RANDOM.
auto memo = tg::Table<float>::open("root.tg");
int m[11];
tg::snap_nearest(x + 1, 11, sp, m);  // x sorted; x[0] = 0 is pinned
float  p1 = memo[m];                 // one access
double p2 = tg::interp_simplex(memo.data(), x + 1, 11, sp); // d+1 loads
tg::unrank(r, 11, 16, m);            // the argument stored in slot r
