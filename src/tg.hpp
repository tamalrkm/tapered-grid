// tg.hpp -- key-free memo table for functions of a sorted argument.
//
// A key is a non-decreasing column vector m = (m_1 <= ... <= m_d), m_i in
// 1..B (a combination with repetition). Its address is the lexicographic rank
// in the combinatorial number system, computed in O(d) from a shared Pascal
// table; unrank inverts it. The table stores values only: N = C(B+d-1, d)
// payloads in N slots, in RAM or in a memory-mapped file.
//
// Header-only, C++17, Linux (mmap/madvise). No dependencies.
//
//   tg::Shape   sh{B, d};                       // N = sh.size()
//   tg::Table<float> t = tg::Table<float>::create("f.tg", sh, meta);
//   t.fill([](const int* m, int d) { return f(m, d); }, threads);
//   t.sync();
//   auto r = tg::Table<float>::open("f.tg");    // validates the header
//   float v = r.at(tg::rank(cols, sh));         // one memory access
//   r.get_many(keys, n, out);                   // n lookups, overlapped
//
// Limits (checked by Shape::valid): B >= 1, d >= 1, B + d <= 68, so that every
// binomial used, and N itself, fits in 64 bits. The caller guarantees that a
// key is non-decreasing and inside 1..B; rank() does not check (tg::check_key
// does, for debug builds and file-boundary validation).
#pragma once
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tg {

using u64 = uint64_t;

// The rank is the inner loop of every lookup; a call or a reloaded closure around
// it costs window entries (see the comment at rank()).
#define TG_ALWAYS_INLINE inline __attribute__((always_inline))

// ---- binomials -------------------------------------------------------------
// Rows 0..67. C(67, k) < 2^64 for every k, C(68, 34) is not; hence B+d <= 68.
constexpr int PASCAL_ROWS = 68;
struct Pascal {
    u64 v[PASCAL_ROWS * PASCAL_ROWS];            // v[n * PASCAL_ROWS + k] = C(n, k)
    u64 operator()(int n, int k) const { return v[n * PASCAL_ROWS + k]; }
    Pascal() {
        std::memset(v, 0, sizeof v);
        for (int n = 0; n < PASCAL_ROWS; n++) {
            v[n * PASCAL_ROWS] = 1;
            for (int k = 1; k <= n; k++) v[n * PASCAL_ROWS + k] = (*this)(n-1, k-1) + (k <= n-1 ? (*this)(n-1, k) : 0);
        }
    }
};
inline const Pascal& pascal() { static const Pascal P; return P; }

struct Shape {
    int B = 0, d = 0;
    bool valid() const { return B >= 1 && d >= 1 && B + d <= PASCAL_ROWS; }
    u64 size() const { return pascal()(B + d - 1, d); }
};

// ---- addressing ------------------------------------------------------------
// Lexicographic rank (0-based). A key comes after m exactly when, at the first
// position i where the two differ, it has the larger value, so
//   rank(m) = N - 1 - sum_{i=1}^{d} C(B - m_i + d - i, d - i + 1).
// One table read per coordinate. Row and column both fall by one per
// coordinate, so the flat index falls by PASCAL_ROWS + 1. The tightness of this
// loop matters more than its O(d) suggests: the next lookup's memory access can
// overlap this one only if both fit in the core's out-of-order window. bench.cpp
// keeps the two-read form of the same sum as mode +two, to measure the difference.
TG_ALWAYS_INLINE u64 rank(const int* m, int d, int B, const Pascal& P = pascal()) {
    const long R = PASCAL_ROWS;
    long x = (B + d - 1) * R + d;                // C(B+d-1, d) = N
    const u64 last = P.v[x] - 1;
    u64 after = 0;
    for (int i = 0; i < d; i++, x -= R + 1) after += P.v[x - R * m[i]];
    return last - after;
}
inline u64 rank(const int* m, const Shape& s) { return rank(m, s.d, s.B); }

// Address change when coordinate i (1-based) moves from v to v+1, all other
// coordinates fixed and the result still non-decreasing:
//   rank(m + e_i) - rank(m) = C(B - v + d - i - 1, d - i).
// It depends on (i, v) only. For i = d it is 1; for i = d-1 it is B - v.
inline u64 step(int i, int v, int d, int B, const Pascal& P = pascal()) {
    return P(B - v + d - i - 1, d - i);
}

// Inverse of rank: greedy combinadic decode, O(B + d) in total because the
// candidate value never decreases. Requires r < N.
inline void unrank(u64 r, int d, int B, int* m, const Pascal& P = pascal()) {
    int v = 1;
    for (int i = 1; i <= d; i++) {
        int L = d - i;
        while (v < B) {
            u64 blk = P(B - v + L, L);        // completions with m_i = v
            if (r < blk) break;
            r -= blk; v++;
        }
        m[i-1] = v;
    }
}

// Lexicographic successor (odometer). Returns false after the last key.
inline bool next(int* m, int d, int B) {
    int i = d - 1;
    while (i >= 0 && m[i] == B) i--;
    if (i < 0) return false;
    m[i]++;
    for (int j = i + 1; j < d; j++) m[j] = m[i];
    return true;
}

inline bool check_key(const int* m, int d, int B) {
    for (int i = 0; i < d; i++) {
        if (m[i] < 1 || m[i] > B) return false;
        if (i && m[i] < m[i-1]) return false;
    }
    return true;
}

// Keys extending a fixed prefix (m_1..m_j) occupy one contiguous rank
// interval of length C(B - m_j + d - j, d - j) (prefix contiguity).
inline u64 prefix_block(int j, int mj, int d, int B, const Pascal& P = pascal()) {
    return P(B - mj + d - j, d - j);
}

// ---- spacing and lookup paths ---------------------------------------------
// Admissible values s[1..B] in [0,1], s[1] = 0, s[B] = 1, strictly increasing.
// power(B, gamma) puts them at ((j-1)/(B-1))^gamma: fine near 0 for gamma > 1.
struct Spacing {
    std::vector<double> s;            // s[0] unused
    int B = 0;
    static Spacing power(int B, double gamma) {
        Spacing sp; sp.B = B; sp.s.assign(B + 1, 0.0);
        for (int j = 1; j <= B; j++) sp.s[j] = std::pow(double(j - 1) / double(B - 1), gamma);
        return sp;
    }
};

// Nearest admissible value per coordinate, scanning forward from the previous
// coordinate's column so the result is non-decreasing. x has d entries.
inline void snap_nearest(const double* x, int d, const Spacing& sp, int* m) {
    const double* s = sp.s.data(); int B = sp.B, lo = 1;
    for (int i = 0; i < d; i++) {
        double xi = std::min(std::max(x[i], 0.0), 1.0);
        int j = lo;
        while (j + 1 <= B && std::fabs(s[j+1] - xi) < std::fabs(s[j] - xi)) j++;
        m[i] = j; lo = j;
    }
}

// Lower cell corner and in-cell fraction per coordinate: s[m] <= x < s[m+1],
// t = (x - s[m]) / (s[m+1] - s[m]) in [0,1]. Non-decreasing x gives a
// non-decreasing corner, and equal corners give non-decreasing fractions.
inline void locate(const double* x, int d, const Spacing& sp, int* m, double* t) {
    const double* s = sp.s.data(); int B = sp.B, lo = 1;
    for (int i = 0; i < d; i++) {
        double xi = std::min(std::max(x[i], 0.0), 1.0);
        int j = lo;
        while (j + 1 < B && s[j+1] <= xi) j++;
        m[i] = j; lo = j;
        t[i] = std::min(1.0, (xi - s[j]) / (s[j+1] - s[j]));
    }
}

constexpr int MAX_D = 64;

// Piecewise-linear interpolation on the Kuhn (Freudenthal) triangulation of the
// cell: visit coordinates in decreasing order of t, ties broken towards the
// larger index, adding one unit at a time. That order keeps every visited
// vertex non-decreasing, so each is a stored key, and each vertex's address is
// the previous one plus step(). d+1 lookups (fewer when some t are equal),
// O(d log d) arithmetic, exact for f affine in x. Requires B >= 2.
template <class T>
inline double interp_simplex(const T* arr, const double* x, int d, const Spacing& sp,
                             const Pascal& P = pascal()) {
    if (d < 1) return double(arr[0]);
    int m[MAX_D], ord[MAX_D]; double t[MAX_D];
    const int B = sp.B;
    locate(x, d, sp, m, t);
    for (int i = 0; i < d; i++) ord[i] = i;
    // insertion sort: t descending, ties -> larger index first (d is small)
    for (int a = 1; a < d; a++) {
        int key = ord[a], b = a - 1;
        while (b >= 0 && (t[ord[b]] < t[key] || (t[ord[b]] == t[key] && ord[b] < key))) {
            ord[b+1] = ord[b]; b--;
        }
        ord[b+1] = key;
    }
    u64 r = rank(m, d, B, P);
    double acc = (1.0 - t[ord[0]]) * double(arr[r]);
    for (int k = 0; k < d; k++) {
        int i = ord[k];
        double w = t[i] - (k + 1 < d ? t[ord[k+1]] : 0.0);
        if (t[i] <= 0.0) break;                  // remaining weights are zero
        r += step(i + 1, m[i], d, B, P);         // m[i] -> m[i] + 1
        if (w > 0.0) acc += w * double(arr[r]);
    }
    return acc;
}

// ---- file format -----------------------------------------------------------
// One 4096-byte header page, then the payload, so the payload is page-aligned
// in the file and in every mapping of it. Little-endian only (checked).
enum class DType : uint32_t { f32 = 1, f64 = 2, u32 = 3 };
template <class T> constexpr DType dtype_of();
template <> constexpr DType dtype_of<float>() { return DType::f32; }
template <> constexpr DType dtype_of<double>() { return DType::f64; }
template <> constexpr DType dtype_of<uint32_t>() { return DType::u32; }

struct Meta {
    uint32_t pin_first = 0;          // 1: stored keys are coordinates 2..d+1, x_1 fixed
    uint32_t spacing_kind = 0;       // 0 none, 1 power(B, gamma)
    double spacing_param = 0.0;      // gamma
    char note[256] = {0};            // generator, compiler, libm: free text
};

struct alignas(4096) Header {
    char magic[8];                   // "TGRID\0\0\1"
    uint32_t version, header_bytes, endian_tag, dtype, elem_size;
    int32_t B, d;
    uint64_t N, payload_offset, payload_hash;
    Meta meta;
    uint64_t header_hash;            // over every byte before this field
};
static_assert(sizeof(Header) == 4096, "header must be one page");
constexpr char MAGIC[8] = {'T','G','R','I','D',0,0,1};
constexpr uint32_t ENDIAN_TAG = 0x01020304u;

// 64-bit FNV-style fold over 8-byte words (plus tail bytes); fast enough for
// multi-GB payloads and deterministic across machines.
inline uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    size_t w = n / 8;
    for (size_t i = 0; i < w; i++) { uint64_t v; std::memcpy(&v, b + 8*i, 8); h = (h ^ v) * 1099511628211ull; }
    for (size_t i = 8*w; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

inline uint64_t header_hash_of(const Header& h) {
    return hash_bytes(&h, offsetof(Header, header_hash));
}

// Header for a table of shape sh and element type T. Writers that stream the
// payload themselves (files larger than RAM) write this page first and rewrite
// it with the final payload hash at the end.
template <class T>
inline Header make_header(Shape sh, const Meta& meta, uint64_t payload_hash) {
    Header h;
    std::memset(static_cast<void*>(&h), 0, sizeof(Header));
    std::memcpy(h.magic, MAGIC, 8);
    h.version = 1; h.header_bytes = sizeof(Header); h.endian_tag = ENDIAN_TAG;
    h.dtype = (uint32_t)dtype_of<T>(); h.elem_size = sizeof(T);
    h.B = sh.B; h.d = sh.d; h.N = sh.size();
    h.payload_offset = sizeof(Header); h.payload_hash = payload_hash;
    h.meta = meta;
    h.header_hash = header_hash_of(h);
    return h;
}

// ---- the table -------------------------------------------------------------
enum class Access { random, sequential, normal };

template <class T>
class Table {
public:
    Table() = default;
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    Table(Table&& o) noexcept { *this = std::move(o); }
    Table& operator=(Table&& o) noexcept {
        if (this != &o) { release(); base_ = o.base_; bytes_ = o.bytes_; data_ = o.data_;
            sh_ = o.sh_; fd_ = o.fd_; meta_ = o.meta_; writable_ = o.writable_;
            o.base_ = nullptr; o.fd_ = -1; o.data_ = nullptr; }
        return *this;
    }
    ~Table() { release(); }

    // Values in anonymous memory (no file). huge: request transparent huge pages.
    // populate: fault every page in now (keeps page-fault cost out of fill()).
    static Table anonymous(Shape sh, bool huge = true, bool populate = false) {
        need(sh.valid(), "anonymous: invalid shape");
        Table t; t.sh_ = sh; t.bytes_ = sh.size() * sizeof(T);
        void* p = mmap(nullptr, t.bytes_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | (populate && !huge ? MAP_POPULATE : 0), -1, 0);
        need_sys(p != MAP_FAILED, "anonymous: mmap failed");
        madvise(p, t.bytes_, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
        t.base_ = p; t.data_ = static_cast<T*>(p); t.writable_ = true;
        if (populate && huge) { for (size_t o = 0; o < t.bytes_; o += 4096) static_cast<volatile char*>(p)[o] = 0; }
        return t;
    }

    // New file: header page + N payload slots, mapped read-write and shared.
    static Table create(const std::string& path, Shape sh, const Meta& meta = Meta()) {
        need(sh.valid(), "create: invalid shape");
        Table t; t.sh_ = sh; t.meta_ = meta;
        t.fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        need_sys(t.fd_ >= 0, "create: open failed");
        t.bytes_ = sizeof(Header) + sh.size() * sizeof(T);
        need_sys(ftruncate(t.fd_, (off_t)t.bytes_) == 0, "create: ftruncate failed");
        void* p = mmap(nullptr, t.bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, t.fd_, 0);
        need_sys(p != MAP_FAILED, "create: mmap failed");
        t.base_ = p; t.data_ = reinterpret_cast<T*>(static_cast<char*>(p) + sizeof(Header));
        t.writable_ = true;
        t.write_header(0);
        return t;
    }

    // Existing file, mapped read-only. Throws on any header mismatch.
    static Table open(const std::string& path, Access a = Access::random, bool verify_payload = false) {
        Table t;
        t.fd_ = ::open(path.c_str(), O_RDONLY);
        need_sys(t.fd_ >= 0, "open: cannot open file");
        struct stat st; need_sys(fstat(t.fd_, &st) == 0, "open: fstat failed");
        need((size_t)st.st_size >= sizeof(Header), "open: file shorter than header");
        Header h; need_sys(pread(t.fd_, &h, sizeof h, 0) == (ssize_t)sizeof h, "open: header read failed");
        need(std::memcmp(h.magic, MAGIC, 8) == 0, "open: bad magic");
        need(h.endian_tag == ENDIAN_TAG, "open: endianness mismatch");
        need(h.header_hash == header_hash_of(h), "open: header checksum mismatch");
        need(h.dtype == (uint32_t)dtype_of<T>() && h.elem_size == sizeof(T), "open: payload type mismatch");
        t.sh_ = Shape{h.B, h.d};
        need(t.sh_.valid() && h.N == t.sh_.size(), "open: shape/size mismatch");
        need(h.payload_offset == sizeof(Header), "open: unexpected payload offset");
        t.bytes_ = sizeof(Header) + h.N * sizeof(T);
        need((size_t)st.st_size == t.bytes_, "open: file size does not match header");
        void* p = mmap(nullptr, t.bytes_, PROT_READ, MAP_SHARED, t.fd_, 0);
        need_sys(p != MAP_FAILED, "open: mmap failed");
        t.base_ = p; t.data_ = reinterpret_cast<T*>(static_cast<char*>(p) + sizeof(Header));
        t.meta_ = h.meta;
        madvise(p, t.bytes_, a == Access::random ? MADV_RANDOM :
                             a == Access::sequential ? MADV_SEQUENTIAL : MADV_NORMAL);
        if (verify_payload && h.payload_hash)
            need(hash_bytes(t.data_, h.N * sizeof(T)) == h.payload_hash, "open: payload checksum mismatch");
        return t;
    }

    // Fill every slot with f(m, d), m the key of that slot. Worker j owns the
    // index range [jN/T, (j+1)N/T): it unranks the first index once and walks
    // the range with next(). No locks, no shared state, any T.
    template <class F>
    void fill(F f, int threads = 1) {
        need(writable_, "fill: table is read-only");
        const u64 N = sh_.size(); const int d = sh_.d, B = sh_.B;
        auto work = [&](u64 lo, u64 hi) {
            if (lo >= hi) return;
            int m[MAX_D]; unrank(lo, d, B, m);
            for (u64 r = lo; r < hi; r++) { data_[r] = f(static_cast<const int*>(m), d); next(m, d, B); }
        };
        run_ranges(N, threads, work);
    }
    // Fill slot r with g(r) (payload that needs only the index).
    template <class G>
    void fill_index(G g, int threads = 1) {
        need(writable_, "fill_index: table is read-only");
        auto work = [&](u64 lo, u64 hi) { for (u64 r = lo; r < hi; r++) data_[r] = g(r); };
        run_ranges(sh_.size(), threads, work);
    }

    // Flush a file-backed table, recording the payload checksum in the header.
    void sync(bool with_hash = true) {
        if (fd_ < 0) return;
        write_header(with_hash ? hash_bytes(data_, sh_.size() * sizeof(T)) : 0);
        msync(base_, bytes_, MS_SYNC);
    }

    T at(u64 r) const { return data_[r]; }
    T operator[](const int* m) const { return data_[rank(m, sh_.d, sh_.B)]; }

    // Independent lookups in a batch: keys holds n rows of d columns, out
    // receives n values. Addresses are computed and prefetched BATCH at a time
    // and read afterwards, so the reads overlap however long the rank loop is.
    static constexpr size_t BATCH = 32;
    void get_many(const int* keys, size_t n, T* out) const {
        const int d = sh_.d, B = sh_.B; const Pascal& P = pascal(); u64 r[BATCH];
        for (size_t q = 0; q < n; q += BATCH) {
            const size_t w = std::min(BATCH, n - q);
            for (size_t i = 0; i < w; i++) { r[i] = rank(keys + (q + i) * d, d, B, P); __builtin_prefetch(data_ + r[i]); }
            for (size_t i = 0; i < w; i++) out[q + i] = data_[r[i]];
        }
    }

    T* data() { return data_; }
    const T* data() const { return data_; }
    Shape shape() const { return sh_; }
    u64 size() const { return sh_.size(); }
    const Meta& meta() const { return meta_; }
    size_t mapped_bytes() const { return bytes_; }
    int fd() const { return fd_; }                // for posix_fadvise / mincore users

private:
    // need(): a logical check. need_sys(): a failed system call, reports errno.
    static void need(bool ok, const char* what) {
        if (!ok) throw std::runtime_error(std::string("tg::Table ") + what);
    }
    static void need_sys(bool ok, const char* what) {
        if (!ok) throw std::runtime_error(std::string("tg::Table ") + what + ": " + std::strerror(errno));
    }
    template <class W>
    static void run_ranges(u64 N, int threads, W& work) {
        if (threads <= 1) { work(0, N); return; }
        std::vector<std::thread> ws;
        for (int j = 0; j < threads; j++) {
            u64 lo = (unsigned __int128)N * j / threads, hi = (unsigned __int128)N * (j + 1) / threads;
            ws.emplace_back([&work, lo, hi] { work(lo, hi); });
        }
        for (auto& w : ws) w.join();
    }
    void write_header(uint64_t payload_hash) {
        Header h = make_header<T>(sh_, meta_, payload_hash);
        std::memcpy(base_, &h, sizeof(Header));
    }
    void release() {
        if (base_) munmap(base_, bytes_);
        if (fd_ >= 0) ::close(fd_);
        base_ = nullptr; fd_ = -1; data_ = nullptr;
    }

    void* base_ = nullptr;
    size_t bytes_ = 0;
    T* data_ = nullptr;
    Shape sh_;
    int fd_ = -1;
    Meta meta_;
    bool writable_ = false;
};

} // namespace tg
