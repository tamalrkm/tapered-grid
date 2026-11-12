// Out-of-core serving and locality on a tg table file larger than RAM.
//
// usage:
//   ooc_bench build B d [path]                 stream-write header + payload (value_of)
//   ooc_bench scan  B d [path]                 sequential read, verify every payload
//                                              and the header's payload hash
//   ooc_bench query B d M seed [path] [evict=1] [W]
//                                              M random point queries (mmap, MADV_RANDOM);
//                                              evict=0 leaves the page cache as it is
//   ooc_bench walk  B d M seed lex|hashed [W] [path]
//                   one coordinate +-1 per step, coordinate uniform
//   ooc_bench gauss B d M seed lex|hashed sigma [W] [path]
//                   every coordinate moves by round(sigma*Z) per step, then sorted
//   ooc_bench trace B d tracefile lex|hashed [W] [path]
//                   replay a trace of 16-byte records {u64 rank; i64 lead}, where
//                   lead is the first coordinate (0-based) that differs from the
//                   same item's previous query, -1 if none (pl_fit --trace-out)
//
// W = 0 is a cold start. W < 0 is a warm start: the stream's first -W steps are
// generated but not replayed. Their most recent distinct pages, as many as fit in
// 90% of the available memory, are read in address order, which leaves the cache as
// a long run would have left it without hours of seeks; then M steps are measured.
//
// "hashed" addresses the same key by mix64(rank) mod N, as any hash-addressed
// layout would. A page-cache hit is decided by mincore() on the target page
// before the access, not by timing. The file is evicted with
// posix_fadvise(DONTNEED) before every run except where noted.
#include "common.hpp"
#include "tg.hpp"
#include <cmath>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

static const char* DEFAULT_PATH = "ooc_memo.tg";

static void evict(int fd) { fdatasync(fd); posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); }

static double resident_pct(const void* p, size_t bytes) {
    size_t pages = (bytes + 4095) / 4096; std::vector<unsigned char> v(pages);
    if (mincore(const_cast<void*>(p), bytes, v.data()) != 0) return -1;
    size_t r = 0; for (auto c : v) r += c & 1; return 100.0 * r / pages;
}

static std::string csv(const std::vector<double>& v) {
    std::string s; char t[32];
    for (size_t i = 0; i < v.size(); i++) { std::snprintf(t, sizeof t, "%s%.1f", i ? "," : "", v[i]); s += t; }
    return s;
}

static double mem_available_bytes() {
    FILE* f = std::fopen("/proc/meminfo", "r"); char line[256]; long kb = 0;
    if (f) { while (std::fgets(line, sizeof line, f)) if (std::sscanf(line, "MemAvailable: %ld", &kb) == 1) break; std::fclose(f); }
    return kb * 1024.0;
}

// Replays a sequence of (rank, leading changed coordinate) against the mapped
// file. Steps [0, W) are warm-up, [W, W+M) are measured.
struct Step { u64 rank; int coord; };

// Brings in the most recent distinct pages of steps [0, W), newest first until the
// cache is full, then reads them in address order. Returns the number of pages.
static size_t prewarm(int fd, const float* arr, const std::vector<Step>& steps, long W, bool hashed, u64 N) {
    const size_t cap = size_t(0.9 * mem_available_bytes() / 4096);
    std::vector<bool> seen(N * 4 / 4096 + 1, false);
    std::vector<u64> pages;
    for (long q = W - 1; q >= 0 && pages.size() < cap; q--) {
        u64 addr = hashed ? mix64(steps[q].rank) % N : steps[q].rank, pg = addr * 4 / 4096;
        if (!seen[pg]) { seen[pg] = true; pages.push_back(pg); }
    }
    std::sort(pages.begin(), pages.end());
    for (u64 pg : pages) posix_fadvise(fd, off_t(4096 + pg * 4096), 4096, POSIX_FADV_WILLNEED);
    volatile float sink = 0;
    for (u64 pg : pages) sink = arr[pg * 1024];
    (void)sink;
    return pages.size();
}
static void replay(const char* kind, const std::string& path, int B, int d, const std::vector<Step>& steps,
                   long W, bool hashed, u64 seed, double sigma, bool do_evict) {
    auto t = tg::Table<float>::open(path, tg::Access::random);
    const u64 N = t.size(); const float* arr = t.data();
    if (do_evict) evict(t.fd());
    const long start = W < 0 ? -W : 0;            // steps [0, start) are only prewarmed
    const size_t warmed = start ? prewarm(t.fd(), arr, steps, start, hashed, N) : 0;
    const long W_arg = W;
    if (W < 0) W = 0;
    double res0 = resident_pct(arr, N * 4);
    const int NC = d + 1;                         // coord buckets 0..d-1, d = "no change"
    struct Acc { long n = 0, hit = 0, same = 0; double s_hit = 0, s_miss = 0; std::vector<long> n_c, h_c, sp_c;
                 std::vector<double> lat; };
    Acc ph[2]; for (auto& a : ph) { a.n_c.assign(NC, 0); a.h_c.assign(NC, 0); a.sp_c.assign(NC, 0); }
    u64 prev_page = ~0ull; double checksum = 0, expect = 0;
    std::vector<u64> pages_measured;
    auto T0 = clk::now();
    for (size_t q = start; q < steps.size(); q++) {
        Acc& a = ph[(long)q < W ? 0 : 1];
        u64 addr = hashed ? mix64(steps[q].rank) % N : steps[q].rank;
        u64 pg = addr * 4 / 4096;
        unsigned char v = 0; mincore((void*)((const char*)arr + pg * 4096), 4096, &v);
        bool hit = v & 1;
        auto t0 = clk::now();
        checksum += arr[addr];
        double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
        expect += value_of(addr);
        int c = steps[q].coord < 0 ? d : steps[q].coord;
        a.n++; a.n_c[c]++; a.lat.push_back(us);
        if (hit) { a.hit++; a.h_c[c]++; a.s_hit += us; } else a.s_miss += us;
        if (pg == prev_page) { a.same++; a.sp_c[c]++; }
        prev_page = pg;
        if ((long)q >= W) pages_measured.push_back(pg);
    }
    double total = secs_since(T0);
    std::sort(pages_measured.begin(), pages_measured.end());
    long distinct = std::unique(pages_measured.begin(), pages_measured.end()) - pages_measured.begin();
    for (int p = 0; p < 2; p++) {
        Acc& a = ph[p]; if (!a.n) continue;
        std::vector<double> sl = a.lat; std::sort(sl.begin(), sl.end());
        auto pct = [&](double x) { return sl[size_t(x * (sl.size() - 1))]; };
        double mean = 0; for (double x : a.lat) mean += x; mean /= a.n;
        std::vector<double> hc(NC), spc(NC), nc(NC);
        for (int c = 0; c < NC; c++) { nc[c] = a.n_c[c]; hc[c] = a.n_c[c] ? 100.0 * a.h_c[c] / a.n_c[c] : -1;
                                       spc[c] = a.n_c[c] ? 100.0 * a.sp_c[c] / a.n_c[c] : -1; }
        long miss = a.n - a.hit;
        J("ooc_bench").s("mode", kind).s("addressing", hashed ? "hashed" : "lex").s("phase", p ? "measured" : "warmup")
            .i("B", B).i("d", d).u("N", N).i("M", a.n).i("W", W_arg).u("seed", seed).f("sigma", sigma)
            .i("evicted", do_evict).f("resident_before_pct", res0).u("warm_pages", warmed)
            .f("hit_pct", 100.0 * a.hit / a.n).f("same_page_pct", 100.0 * a.same / a.n)
            .i("distinct_pages", p ? distinct : -1)
            .f("mean_us", mean).f("p50_us", pct(.5)).f("p90_us", pct(.9)).f("p99_us", pct(.99))
            .f("hit_mean_us", a.hit ? a.s_hit / a.hit : 0).f("miss_mean_ms", miss ? a.s_miss / miss / 1000 : 0)
            .f("total_s", total).s("steps_by_coord", csv(nc)).s("hit_pct_by_coord", csv(hc))
            .s("same_page_pct_by_coord", csv(spc))
            .s("checksum_ok", std::fabs(checksum - expect) < 1e-3 * (1 + std::fabs(expect)) ? "yes" : "NO").emit();
    }
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s build|scan|query|walk|gauss|trace B d ...\n", argv[0]); return 1; }
    std::string mode = argv[1]; int B = atoi(argv[2]), d = atoi(argv[3]);
    tg::Shape sh{B, d};
    if (!sh.valid()) { std::fprintf(stderr, "invalid shape\n"); return 1; }
    const u64 N = sh.size();
    auto arg = [&](int i, const char* def) { return argc > i ? std::string(argv[i]) : std::string(def); };

    if (mode == "build") {
        std::string path = arg(4, DEFAULT_PATH);
        auto t0 = clk::now();
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); return 1; }
        tg::Header h = tg::make_header<float>(sh, tg::Meta(), 0);
        if (write(fd, &h, sizeof h) != (ssize_t)sizeof h) { perror("write"); return 1; }
        const u64 CH = 1u << 20; std::vector<float> buf(CH); uint64_t hh = 1469598103934665603ull;
        for (u64 base = 0; base < N; base += CH) {
            u64 n = std::min(CH, N - base);
            for (u64 j = 0; j < n; j++) buf[j] = value_of(base + j);
            hh = tg::hash_bytes(buf.data(), n * 4, hh);       // chunks are multiples of 8 bytes except the last
            if (write(fd, buf.data(), n * 4) != (ssize_t)(n * 4)) { perror("write"); return 1; }
            if (((base / CH) & 255) == 255) evict(fd);          // every ~1 GB: flush and drop
        }
        h = tg::make_header<float>(sh, tg::Meta(), hh);
        if (pwrite(fd, &h, sizeof h, 0) != (ssize_t)sizeof h) { perror("pwrite"); return 1; }
        evict(fd); close(fd);
        double s = secs_since(t0);
        J("ooc_bench").s("mode", "build").i("B", B).i("d", d).u("N", N).f("file_gb", (N * 4.0 + 4096) / 1e9)
            .f("build_s", s).f("entries_per_s", N / s).emit();
    } else if (mode == "scan") {
        std::string path = arg(4, DEFAULT_PATH);
        auto t0 = clk::now();
        { auto t = tg::Table<float>::open(path, tg::Access::sequential); (void)t; }   // header checks
        int fd = open(path.c_str(), O_RDONLY); posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        tg::Header h; if (pread(fd, &h, sizeof h, 0) != (ssize_t)sizeof h) { perror("pread"); return 1; }
        if (lseek(fd, 4096, SEEK_SET) < 0) { perror("lseek"); return 1; }
        const u64 CH = 1u << 20; std::vector<float> buf(CH); u64 bad = 0; uint64_t hh = 1469598103934665603ull;
        for (u64 base = 0; base < N; base += CH) {
            u64 n = std::min(CH, N - base);
            if (read(fd, buf.data(), n * 4) != (ssize_t)(n * 4)) { perror("read"); return 1; }
            hh = tg::hash_bytes(buf.data(), n * 4, hh);
            for (u64 j = 0; j < n; j++) if (buf[j] != value_of(base + j)) bad++;
        }
        close(fd);
        double s = secs_since(t0);
        J("ooc_bench").s("mode", "scan").i("B", B).i("d", d).u("N", N).f("scan_s", s).f("entries_per_s", N / s)
            .u("mismatches", bad).s("payload_hash_ok", hh == h.payload_hash ? "yes" : "NO").emit();
        return bad ? 2 : 0;
    } else if (mode == "query") {
        long M = atol(arg(4, "20000").c_str()); u64 seed = strtoull(arg(5, "42").c_str(), 0, 10);
        std::string path = arg(6, DEFAULT_PATH); bool ev = arg(7, "1") != "0"; long W = atol(arg(8, "0").c_str());
        const long T = M + (W < 0 ? -W : 0);
        std::vector<Step> steps(T);
        { u64 s = seed | 1; std::vector<int> c(d); for (long q = 0; q < T; q++) { gen_query(s, d, B, c.data()); steps[q] = {tg::rank(c.data(), d, B), -1}; } }
        replay("query", path, B, d, steps, W, false, seed, 0, ev);
    } else if (mode == "walk" || mode == "gauss") {
        long M = atol(arg(4, "20000").c_str()); u64 seed = strtoull(arg(5, "42").c_str(), 0, 10);
        bool hashed = arg(6, "lex") == "hashed";
        int k = 7; double sigma = 0;
        if (mode == "gauss") sigma = atof(arg(k++, "0.5").c_str());
        long W = atol(arg(k++, "0").c_str());
        std::string path = arg(k, DEFAULT_PATH);
        const long total = M + (W < 0 ? -W : W);
        std::vector<Step> steps; steps.reserve(total);
        u64 s = seed | 1; std::vector<int> c(d), c2(d);
        gen_query(s, d, B, c.data());
        auto unif = [&]() { return (xs(s) >> 11) * (1.0 / 9007199254740992.0); };
        while ((long)steps.size() < total) {
            if (mode == "walk") {
                int i = int(xs(s) % d), sg = (xs(s) & 1) ? 1 : -1, v = c[i] + sg;
                bool ok = v >= 1 && v <= B && (i == 0 || c[i-1] <= v) && (i == d - 1 || v <= c[i+1]);
                if (!ok) continue;
                c[i] = v; steps.push_back({tg::rank(c.data(), d, B), i});
            } else {
                for (int i = 0; i < d; i++) {
                    double u1 = std::max(unif(), 1e-300), u2 = unif();
                    double z = std::sqrt(-2 * std::log(u1)) * std::cos(6.283185307179586 * u2);
                    c2[i] = std::min(B, std::max(1, c[i] + (int)std::lround(sigma * z)));
                }
                std::sort(c2.begin(), c2.end());
                int lead = -1; for (int i = 0; i < d; i++) if (c2[i] != c[i]) { lead = i; break; }
                c = c2; steps.push_back({tg::rank(c.data(), d, B), lead});
            }
        }
        replay(mode.c_str(), path, B, d, steps, W, hashed, seed, sigma, true);
    } else if (mode == "trace") {
        std::string tf = arg(4, "trace.u64"); bool hashed = arg(5, "lex") == "hashed";
        long W = atol(arg(6, "0").c_str()); std::string path = arg(7, DEFAULT_PATH);
        FILE* f = std::fopen(tf.c_str(), "rb"); if (!f) { perror("trace"); return 1; }
        std::vector<Step> steps; struct { u64 r; int64_t lead; } rec;
        while (std::fread(&rec, sizeof rec, 1, f) == 1) {
            if (rec.r >= N || rec.lead >= d) { std::fprintf(stderr, "trace record out of range\n"); return 1; }
            steps.push_back({rec.r, (int)rec.lead});
        }
        std::fclose(f);
        replay("trace", path, B, d, steps, W, hashed, 0, 0, true);
    } else { std::fprintf(stderr, "unknown mode\n"); return 1; }
    return 0;
}
