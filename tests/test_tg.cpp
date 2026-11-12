// Unit tests for tg.hpp. Runs in well under a second; `make verify` runs it
// together with the Python reference checks.
#include "../src/tg.hpp"
#include <cassert>
#include <cstdio>
#include <random>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// The 2015 node-count index (its Eq. 2 and Eq. 3), as in bench.cpp.
static tg::u64 V[70][70], PV[70][70];
static void build_tables(int B, int d) {
    for (int b = 0; b <= B; b++) V[1][b] = 0;
    for (int b = 1; b <= B; b++) V[2][b] = 1;
    for (int i = 3; i <= d; i++)
        for (int b = 1; b <= B; b++) { tg::u64 s = 0; for (int j = b; j <= B; j++) s += V[i-1][j]; V[i][b] = s; }
    for (int i = 1; i <= d; i++) { PV[i][0] = 0; for (int b = 1; b <= B; b++) PV[i][b] = PV[i][b-1] + V[i][b]; }
}
static tg::u64 rank_tabled(const int* c, int d) {
    tg::u64 r = 0;
    for (int i = 1; i <= d - 1; i++) { int row = d + 1 - i, a = c[i-1], b = c[i]; r += PV[row][b-1] - PV[row][a-1]; }
    return r;
}

int main() {
    const tg::Pascal& P = tg::pascal();
    long keys = 0;

    // 1-3: rank = enumeration index, unrank inverts it, step() is the
    // neighbour delta, prefix blocks are contiguous with the stated length.
    for (int B = 1; B <= 8; B++) for (int d = 1; d <= 6; d++) {
        tg::Shape sh{B, d}; int m[16], u[16];
        for (int i = 0; i < d; i++) m[i] = 1;
        tg::u64 idx = 0;
        do {
            tg::u64 r = tg::rank(m, d, B, P);
            CHECK(r == idx, "rank B=%d d=%d idx=%llu got %llu", B, d, (unsigned long long)idx, (unsigned long long)r);
            tg::unrank(r, d, B, u);
            for (int i = 0; i < d; i++) CHECK(u[i] == m[i], "unrank B=%d d=%d", B, d);
            for (int i = 0; i < d; i++) {                          // every valid +1 move
                if (m[i] == B || (i + 1 < d && m[i] == m[i+1])) continue;
                m[i]++; tg::u64 r2 = tg::rank(m, d, B, P); m[i]--;
                CHECK(r2 - r == tg::step(i + 1, m[i], d, B, P), "step B=%d d=%d i=%d", B, d, i + 1);
            }
            for (int j = 0; j <= d; j++) {                         // prefix contiguity
                int lo[16], hi[16];
                for (int i = 0; i < d; i++) { lo[i] = i < j ? m[i] : (j ? m[j-1] : 1); hi[i] = i < j ? m[i] : B; }
                tg::u64 a = tg::rank(lo, d, B, P), b = tg::rank(hi, d, B, P);
                CHECK(b - a + 1 == tg::prefix_block(j, j ? m[j-1] : 1, d, B, P), "prefix len B=%d d=%d j=%d", B, d, j);
                CHECK(a <= r && r <= b, "prefix interval B=%d d=%d j=%d", B, d, j);
            }
            idx++; keys++;
        } while (tg::next(m, d, B));
        CHECK(idx == sh.size(), "size B=%d d=%d", B, d);
    }
    std::printf("rank/unrank/step/prefix: %ld keys\n", keys);

    // 4: the 2015 index agrees on its own domain (m_1 = 1) and collides off it.
    long pinned = 0;
    for (int B = 2; B <= 8; B++) for (int d = 2; d <= 6; d++) {
        build_tables(B, d); int m[16];
        for (int i = 0; i < d; i++) m[i] = 1;
        do { if (m[0] == 1) { CHECK(rank_tabled(m, d) == tg::rank(m, d, B, P), "tabled pinned B=%d d=%d", B, d); pinned++; } }
        while (tg::next(m, d, B));
    }
    build_tables(2, 2);
    { int a[2] = {1, 1}, b[2] = {2, 2};
      CHECK(rank_tabled(a, 2) == 0 && rank_tabled(b, 2) == 0, "Eq.(3) counterexample");
      CHECK(tg::rank(b, 2, 2, P) == 2, "lex rank of (2,2)");
      std::printf("Eq.(3) of the 2015 paper: agrees on %ld pinned keys; B=2,d=2: (1,1)->%llu, (2,2)->%llu (lex rank %llu)\n",
                  pinned, (unsigned long long)rank_tabled(a, 2), (unsigned long long)rank_tabled(b, 2),
                  (unsigned long long)tg::rank(b, 2, 2, P)); }

    // 5: simplex interpolation is exact for affine f, and every vertex it
    // visits is a stored (non-decreasing) key.
    std::mt19937_64 rng(7);
    for (int B : {2, 3, 7, 12}) for (int d : {1, 3, 5, 9}) {
        tg::Shape sh{B, d}; if (!sh.valid()) continue;
        tg::Spacing sp = tg::Spacing::power(B, 2.0);
        std::vector<double> coef(d + 1); for (auto& c : coef) c = std::uniform_real_distribution<double>(-1, 1)(rng);
        auto t = tg::Table<double>::anonymous(sh, false);
        t.fill([&](const int* m, int dd) { double v = coef[0]; for (int i = 0; i < dd; i++) v += coef[i+1] * sp.s[m[i]]; return v; });
        for (int q = 0; q < 2000; q++) {
            double x[16]; for (int i = 0; i < d; i++) x[i] = std::uniform_real_distribution<double>(0, 1)(rng);
            if (q % 5 == 0) for (int i = 0; i < d; i++) x[i] = sp.s[1 + rng() % B];         // on gridpoints
            if (q % 7 == 0 && d > 1) x[1] = x[0];                                            // ties
            std::sort(x, x + d);
            double want = coef[0]; for (int i = 0; i < d; i++) want += coef[i+1] * x[i];
            double got = tg::interp_simplex(t.data(), x, d, sp);
            CHECK(std::fabs(got - want) < 1e-12, "simplex exact B=%d d=%d q=%d err=%g", B, d, q, got - want);
            // replay the vertex walk and check monotonicity
            int m[16], ord[16]; double tt[16]; tg::locate(x, d, sp, m, tt);
            for (int i = 0; i < d; i++) ord[i] = i;
            std::sort(ord, ord + d, [&](int a, int b) { return tt[a] > tt[b] || (tt[a] == tt[b] && a > b); });
            for (int k = 0; k < d && tt[ord[k]] > 0; k++) { m[ord[k]]++; CHECK(tg::check_key(m, d, B), "vertex not a key B=%d d=%d", B, d); }
        }
    }
    std::printf("simplex interpolation: exact on affine f, all vertices stored keys\n");

    // 6: nearest snapping yields stored keys
    { tg::Spacing sp = tg::Spacing::power(16, 2.0); int m[16];
      for (int q = 0; q < 10000; q++) { double x[12]; for (auto& v : x) v = std::uniform_real_distribution<double>(0, 1)(rng);
        std::sort(x, x + 12); tg::snap_nearest(x, 12, sp, m); CHECK(tg::check_key(m, 12, 16), "snap not a key"); } }

    // 7: file round trip, payload hash, header corruption detected
    { tg::Shape sh{9, 5}; const char* path = "test_tg.tmp";
      { auto t = tg::Table<float>::create(path, sh); t.fill([](const int* m, int d) { float v = 0; for (int i = 0; i < d; i++) v = v * 10 + m[i]; return v; }, 3); t.sync(); }
      { auto t = tg::Table<float>::open(path, tg::Access::random, true); int m[5] = {1, 2, 2, 5, 9};
        CHECK(t[m] == 12259.0f, "round trip value %g", (double)t[m]); CHECK(t.size() == sh.size(), "round trip size"); }
      { FILE* f = std::fopen(path, "r+b"); std::fseek(f, 20, SEEK_SET); std::fputc(0x55, f); std::fclose(f);
        bool threw = false; try { auto t = tg::Table<float>::open(path); } catch (const std::exception&) { threw = true; }
        CHECK(threw, "corrupted header not detected"); }
      { bool threw = false; try { auto t = tg::Table<double>::open(path); } catch (const std::exception&) { threw = true; }
        CHECK(threw, "type mismatch not detected"); }
      std::remove(path); }

    // 8: overflow bound
    CHECK(P(67, 33) == 14226520737620288370ull, "C(67,33)");
    CHECK((tg::Shape{40, 28}.valid()) && !(tg::Shape{41, 28}.valid()), "shape bound");

    // 9: parallel fill equals sequential fill
    { tg::Shape sh{11, 7}; auto a = tg::Table<float>::anonymous(sh, false), b = tg::Table<float>::anonymous(sh, false);
      auto f = [](const int* m, int d) { unsigned h = 2166136261u; for (int i = 0; i < d; i++) h = (h ^ m[i]) * 16777619u; return float(h % 1000003); };
      a.fill(f, 1); b.fill(f, 7);
      CHECK(std::memcmp(a.data(), b.data(), sh.size() * 4) == 0, "parallel fill differs"); }

    // 10: the one-read rank equals the two-read telescoped form it replaced, on
    // every key above and on random keys of large shapes
    { auto two = [&](const int* m, int d, int B) { int prev = 1; tg::u64 r = 0;
          for (int i = 1; i <= d; i++) { int mi = m[i-1], k = d - i; r += P(B - prev + k + 1, k + 1) - P(B - mi + k + 1, k + 1); prev = mi; }
          return r; };
      for (int B = 1; B <= 8; B++) for (int d = 1; d <= 6; d++) { int m[16]; for (int i = 0; i < d; i++) m[i] = 1;
          do CHECK(tg::rank(m, d, B, P) == two(m, d, B), "two-read B=%d d=%d", B, d); while (tg::next(m, d, B)); }
      for (auto [B, d] : {std::pair<int,int>{16, 13}, {34, 10}, {40, 28}, {22, 15}, {60, 8}}) { int m[64];
          for (int q = 0; q < 20000; q++) { for (int i = 0; i < d; i++) m[i] = 1 + int(rng() % B); std::sort(m, m + d);
              CHECK(tg::rank(m, d, B, P) == two(m, d, B), "two-read large B=%d d=%d", B, d); } } }

    // 11: get_many returns what single lookups return, in order, for every batch boundary
    { tg::Shape sh{9, 5}; auto t = tg::Table<float>::anonymous(sh, false);
      t.fill_index([](tg::u64 r) { return float(r % 65521); });
      for (size_t n : {size_t(0), size_t(1), size_t(31), size_t(32), size_t(33), size_t(64), size_t(1000)}) {
          std::vector<int> keys(n * 5); std::vector<float> out(n), want(n);
          for (size_t q = 0; q < n; q++) { int* m = &keys[q * 5]; for (int i = 0; i < 5; i++) m[i] = 1 + int(rng() % 9); std::sort(m, m + 5); want[q] = t[m]; }
          t.get_many(keys.data(), n, out.data());
          CHECK(std::memcmp(out.data(), want.data(), n * 4) == 0, "get_many n=%zu", n); } }

    std::printf(fails ? "test_tg: %d FAILURES\n" : "test_tg: all checks passed\n", fails);
    return fails ? 1 : 0;
}
