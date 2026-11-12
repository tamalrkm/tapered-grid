// common.hpp -- measurement helpers shared by the benchmark binaries.
//
// Every binary prints exactly one line per measurement of the form
//   JSON {"bench":..., ...}
// which scripts/run.py collects. Memory is reported in MiB (2^20 bytes):
// ru_maxrss is in KiB on Linux, and earlier drafts mixed MiB with 10^6.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/resource.h>

using u64 = uint64_t;
using clk = std::chrono::steady_clock;

inline double secs_since(clk::time_point t0) {
    return std::chrono::duration<double>(clk::now() - t0).count();
}

inline double peak_rss_mib() {
    struct rusage ru; getrusage(RUSAGE_SELF, &ru); return ru.ru_maxrss / 1024.0;
}

// Current resident set (not the peak), from /proc/self/statm.
inline double cur_rss_mib() {
    long pages = 0, res = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f) { if (std::fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0; std::fclose(f); }
    return res * 4096.0 / 1048576.0;
}

// Anonymous memory currently backed by huge pages, from smaps_rollup. Logged so
// that every structure's page policy can be read from the results.
inline double anon_huge_mib() {
    long kb = 0; char line[256];
    FILE* f = std::fopen("/proc/self/smaps_rollup", "r");
    if (f) { while (std::fgets(line, sizeof line, f)) if (std::sscanf(line, "AnonHugePages: %ld", &kb) == 1) break; std::fclose(f); }
    return kb / 1024.0;
}

// Minimal JSON line builder: J j("bench"); j.s("mode",m).i("N",N).f("ns",x); j.emit();
struct J {
    std::string b;
    explicit J(const char* bench) { b = "{\"bench\":\""; b += bench; b += "\""; }
    J& s(const char* k, const std::string& v) { b += ",\""; b += k; b += "\":\""; b += v; b += "\""; return *this; }
    J& i(const char* k, long long v) { char t[64]; std::snprintf(t, sizeof t, ",\"%s\":%lld", k, v); b += t; return *this; }
    J& u(const char* k, unsigned long long v) { char t[64]; std::snprintf(t, sizeof t, ",\"%s\":%llu", k, v); b += t; return *this; }
    J& f(const char* k, double v) { char t[96]; std::snprintf(t, sizeof t, ",\"%s\":%.9g", k, v); b += t; return *this; }
    J& d(const char* k, double v) { char t[96]; std::snprintf(t, sizeof t, ",\"%s\":%.17g", k, v); b += t; return *this; }
    void emit() { std::printf("JSON %s}\n", b.c_str()); std::fflush(stdout); }
};

// xorshift64 query stream.
inline u64 xs(u64& s) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
inline void gen_query(u64& s, int d, int B, int* out) {
    for (int i = 0; i < d; i++) out[i] = int(xs(s) % B) + 1;
    std::sort(out, out + d);
}

// splitmix64 finalizer: the hash of the hand-rolled open-addressing tables and
// the "hashed" address in the locality experiments.
inline u64 mix64(u64 x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Packed key: ceil(log2 B) bits per column; bits*d <= 63 so the all-ones word
// stays free as an empty-slot sentinel.
struct Packer {
    int bits = 1;
    Packer(int B, int d) {
        while ((1 << bits) < B) bits++;
        if (bits * d > 63) { std::fprintf(stderr, "pack: %d bits x %d cols exceeds 63\n", bits, d); std::exit(1); }
    }
    u64 operator()(const int* m, int d) const {
        u64 k = 0; for (int i = 0; i < d; i++) k |= u64(m[i] - 1) << (bits * i); return k;
    }
};

// Cheap deterministic payloads: of the index, and of the key itself (needs the
// argument, as a real f does).
inline float value_of(u64 r) {
    u64 h = r * 2654435761ull; return float(h & 0xFFFFFF) * (1.0f / 16777216.0f);
}
inline float payload_cols(const int* m, int d) {
    u64 h = 1469598103934665603ull;
    for (int i = 0; i < d; i++) h = (h ^ u64(m[i])) * 1099511628211ull;
    h ^= h >> 29;
    return float(h & 0xFFFFFF) * (1.0f / 16777216.0f);
}

inline std::string thp_mode() {
    FILE* f = std::fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    char buf[128] = {0};
    if (f) { if (!std::fgets(buf, sizeof buf, f)) buf[0] = 0; std::fclose(f); }
    const char* a = std::strchr(buf, '['); const char* b = a ? std::strchr(a, ']') : nullptr;
    return (a && b) ? std::string(a + 1, b) : std::string("unknown");
}
