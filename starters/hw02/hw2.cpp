// hw2.cpp — HW 2 "Pointers, references & the cost of a copy"  (10 pts)
//
// Build (release — a debug build measures nothing real):
//     g++ -std=c++17 -O2 -Wall hw2.cpp -o hw2 && ./hw2
// or just:  make
//
// WHAT YOU DO: fill in the four TODO blocks below. Everything else — the
// timing harness, the data setup, the four tables — is already written and
// already runs. Build it right now, before you write a line: it compiles,
// it prints all four tables, and the numbers are wrong/zero. That is your
// red signal. Turn each one green.
//
// The signatures are FIXED. Do not change them; the write-up asks about
// exactly these.
//
// WHAT YOU HAND IN: this file + a short README/PDF with your four tables,
// your machine + compiler + flags, and the two explanations. See README.md.

#include "../../tests/bench.hpp"   // doNotOptimize / clobber — the grader's own helpers

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <numeric>
#include <vector>

// ===========================================================================
//  THE TIMING HARNESS — provided. Read it, do not change it.
// ===========================================================================
//
// One sample = the time to run the operation `batch` times, divided by batch.
// We percentile over SAMPLES, not over single operations, because
// steady_clock's own resolution (tens of ns on a laptop) is the same order as
// the operations we are timing — timing one op at a time would measure the
// clock. Warm-up runs `warmup` whole batches first so caches, the branch
// predictor and the CPU frequency are in steady state before the first
// recorded sample.
//
// Every benchmarked lambda MUST feed its result to doNotOptimize(), or -O2
// deletes the work and you "measure" 0 ns. That is the single most common way
// this assignment goes wrong.

// A by-value parameter only costs anything if the call actually happens. At -O2
// the compiler will happily inline sum_by_value() and then delete the copy
// outright — you would measure 0 ns of difference and conclude, wrongly, that
// passing 648 bytes by value is free. HW2_NOINLINE forces a real call through
// the ABI for BOTH functions, so the comparison is apples to apples: the same
// arithmetic, one call passing 648 bytes, one call passing an 8-byte address.
// (Try deleting it and re-running — the two rows collapse onto each other.
// That collapse is worth one sentence in your write-up.)
#if defined(__GNUC__) || defined(__clang__)
#  define HW2_NOINLINE __attribute__((noinline))
#else
#  define HW2_NOINLINE
#endif

struct Stats {
    double p50 = 0, p99 = 0, p999 = 0, mean = 0, min = 0;
    int    samples = 0;
    long   batch = 0;
};

template <class F>
Stats bench(F&& f, long batch, int samples = 1000, int warmup = 50) {
    for (int w = 0; w < warmup; ++w)
        for (long i = 0; i < batch; ++i) f();

    std::vector<double> ns;
    ns.reserve(static_cast<std::size_t>(samples));
    for (int s = 0; s < samples; ++s) {
        auto t0 = std::chrono::steady_clock::now();
        for (long i = 0; i < batch; ++i) f();
        auto t1 = std::chrono::steady_clock::now();
        clobber();
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count()
                     / static_cast<double>(batch));
    }
    std::sort(ns.begin(), ns.end());

    auto pct = [&](double p) {
        std::size_t i = static_cast<std::size_t>(p * static_cast<double>(ns.size() - 1) + 0.5);
        return ns[i];
    };
    Stats st;
    st.p50  = pct(0.50);
    st.p99  = pct(0.99);
    st.p999 = pct(0.999);      // 1000 samples => p99.9 is the worst sample
    st.min  = ns.front();
    st.mean = std::accumulate(ns.begin(), ns.end(), 0.0) / static_cast<double>(ns.size());
    st.samples = samples;
    st.batch   = batch;
    return st;
}

// Re-express a per-call statistic as a per-element one.
static Stats per_element(Stats s, long n) {
    const double d = static_cast<double>(n);
    s.p50 /= d; s.p99 /= d; s.p999 /= d; s.mean /= d; s.min /= d;
    return s;
}

static void table_head(const char* title, const char* unit) {
    std::printf("\n%s\n", title);
    std::printf("  %-26s %12s %12s %12s %12s\n", "variant", "p50", "p99", "p99.9", "mean");
    std::printf("  %-26s %12s %12s %12s %12s\n", "", unit, unit, unit, unit);
    std::printf("  %s\n", "--------------------------------------------------------------"
                          "-------------------");
}

static void row(const char* name, const Stats& s) {
    std::printf("  %-26s %12.3f %12.3f %12.3f %12.3f\n", name, s.p50, s.p99, s.p999, s.mean);
}

// ===========================================================================
//  PART 1 (3 pts) — swap by reference vs swap by pointer
// ===========================================================================

/// Swap the two ints THROUGH REFERENCES. `a` and `b` are aliases for the
/// caller's variables; there is nothing to dereference and nothing can be null.
void swap_ref(int& a, int& b) {
    // TODO(1a): swap the two values.
    (void)a; (void)b;          // delete this line when you implement it
}

/// Swap the two ints THROUGH POINTERS. `a` and `b` are addresses. You must
/// dereference to touch the caller's variables — and "absent" is expressible
/// here in a way it is not for a reference.
///
/// Careful: swapping the POINTERS (`int* t = a; a = b; b = t;`) swaps this
/// function's own two local copies of the addresses and does nothing at all to
/// the caller. That is the classic wrong answer and it scores 0 for this part.
void swap_ptr(int* a, int* b) {
    // TODO(1b): swap the two pointed-to values.
    (void)a; (void)b;          // delete this line when you implement it
}

// ===========================================================================
//  PART 2 (3 pts) — pass-by-value vs pass-by-const-reference
// ===========================================================================

/// A deliberately large struct: 648 bytes, ~11 cache lines. Passing one of
/// these by value copies every byte, on every call.
struct Big {
    double px[64];   // 512 B
    long   qty[16];  // 128 B
    int    id;       //   4 B (+ padding)
};

/// Sum everything in `b`. Takes Big BY VALUE — the caller's Big is copied into
/// this function's parameter before the first line of the body runs.
HW2_NOINLINE double sum_by_value(Big b) {
    // TODO(2a): return the sum of all b.px[] plus all b.qty[] plus b.id.
    (void)b;                   // delete this line when you implement it
    return 0.0;
}

/// Same arithmetic, taking a CONST REFERENCE — 8 bytes of address, no copy,
/// and `const` promises the caller you will not modify it.
HW2_NOINLINE double sum_by_cref(const Big& b) {
    // TODO(2b): same sum as sum_by_value. The BODY must be identical —
    // the only difference between the two functions is how the argument
    // arrives, and that is exactly what you are measuring.
    (void)b;                   // delete this line when you implement it
    return 0.0;
}

// ===========================================================================
//  PART 3 (2 pts) — contiguous traversal vs pointer chasing
// ===========================================================================

struct Node {
    int   v;
    Node* next;
};

/// Sum a contiguous std::vector<int>. Element i+1 is 4 bytes after element i,
/// so the hardware prefetcher can see you coming.
long sum_vector(const std::vector<int>& v) {
    // TODO(3a): return the sum of every element.
    (void)v;                   // delete this line when you implement it
    return 0;
}

/// Sum the same values held in a linked list, by following `next` to the end.
/// Every hop is a load of an address the prefetcher cannot guess.
long sum_list(const Node* head) {
    // TODO(3b): walk the list from `head` and return the sum of every v.
    (void)head;                // delete this line when you implement it
    return 0;
}

// ===========================================================================
//  main — data setup + the four tables. Provided; you should not need to edit.
// ===========================================================================

int main() {
    // ---------------------------------------------------------------- setup
    const long N = 1L << 20;                       // 1,048,576 elements

    std::vector<int> vec(static_cast<std::size_t>(N));
    std::iota(vec.begin(), vec.end(), 0);

    // The list holds the SAME values. We own the nodes in one block so the
    // comparison is about ORDER OF ACCESS, not about allocator behaviour, and
    // we link them in a shuffled order so each hop really is an unpredictable
    // jump. (A list built by `new Node{i, head}` in a loop often comes back
    // nearly contiguous from the allocator and badly understates the effect —
    // worth trying once so you see it.)
    std::vector<Node> nodes(static_cast<std::size_t>(N));
    std::vector<long> order(static_cast<std::size_t>(N));
    std::iota(order.begin(), order.end(), 0L);
    // deterministic shuffle (xorshift64) so everyone measures the same layout
    {
        std::uint64_t x = 88172645463325252ULL;
        for (long i = N - 1; i > 0; --i) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            long j = static_cast<long>(x % static_cast<std::uint64_t>(i + 1));
            std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(j)]);
        }
    }
    for (long k = 0; k < N; ++k) {
        Node& n = nodes[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])];
        n.v     = static_cast<int>(k);
        n.next  = (k + 1 < N)
                      ? &nodes[static_cast<std::size_t>(order[static_cast<std::size_t>(k + 1)])]
                      : nullptr;
    }
    const Node* head = &nodes[static_cast<std::size_t>(order[0])];

    Big big{};
    for (int i = 0; i < 64; ++i) big.px[i]  = 100.0 + i * 0.25;
    for (int i = 0; i < 16; ++i) big.qty[i] = 10 + i;
    big.id = 7;

    const long expect_sum = N * (N - 1) / 2;       // 0 + 1 + ... + (N-1)

    std::printf("HW 2 — pointers, references & the cost of a copy\n");
    std::printf("elements N = %ld   sizeof(Big) = %zu B   sizeof(Node) = %zu B\n",
                N, sizeof(Big), sizeof(Node));

    // ------------------------------------------------- TABLE 1: the swaps
    std::printf("\nTABLE 1 — swap correctness\n");
    std::printf("  %-16s %10s %10s %10s %10s   %s\n",
                "function", "a before", "b before", "a after", "b after", "result");
    std::printf("  ---------------------------------------------------------------------\n");
    {
        int a = 3, b = 9;
        const int a0 = a, b0 = b;
        swap_ref(a, b);
        std::printf("  %-16s %10d %10d %10d %10d   %s\n", "swap_ref",
                    a0, b0, a, b, (a == b0 && b == a0) ? "OK" : "WRONG");
    }
    {
        int a = 3, b = 9;
        const int a0 = a, b0 = b;
        swap_ptr(&a, &b);
        std::printf("  %-16s %10d %10d %10d %10d   %s\n", "swap_ptr",
                    a0, b0, a, b, (a == b0 && b == a0) ? "OK" : "WRONG");
    }
    {   // the trap: does your swap_ptr survive being handed the same address?
        int a = 5;
        swap_ptr(&a, &a);
        std::printf("  %-16s %10d %10d %10d %10d   %s\n", "swap_ptr(&a,&a)",
                    5, 5, a, a, (a == 5) ? "OK" : "WRONG (self-swap zeroed it)");
    }

    // --------------------------------- TABLE 2: by value vs by const& (3 pts)
    table_head("TABLE 2 — pass a 648-byte struct: by value vs by const reference",
         "ns/call");
    {
        double sink = 0.0;
        // doNotOptimize(&big) makes `big`'s address escape and clobbers memory,
        // so the compiler cannot hoist the call out of the loop or reuse last
        // iteration's result. Sink the argument AND the result, every time.
        Stats bv = bench([&] { doNotOptimize(&big); sink += sum_by_value(big); doNotOptimize(sink); },
                         /*batch=*/2000, /*samples=*/1000);
        Stats bc = bench([&] { doNotOptimize(&big); sink += sum_by_cref(big);  doNotOptimize(sink); },
                         /*batch=*/2000, /*samples=*/1000);
        row("sum_by_value(Big)", bv);
        row("sum_by_cref(const Big&)", bc);
        std::printf("  p50 ratio value/cref = %.2fx     (checksum %.1f, %d samples x %ld calls)\n",
                    bc.p50 > 0 ? bv.p50 / bc.p50 : 0.0, sink, bv.samples, bv.batch);
        std::printf("  correctness: sum_by_value=%.1f  sum_by_cref=%.1f  %s\n",
                    sum_by_value(big), sum_by_cref(big),
                    (sum_by_value(big) == sum_by_cref(big) && sum_by_value(big) != 0.0)
                        ? "OK (equal, non-zero)" : "WRONG (must be equal and non-zero)");
    }

    // ------------------------------- TABLE 3: contiguous vs chasing (2 pts)
    table_head("TABLE 3 — traverse 1,048,576 ints: contiguous vector vs linked list",
         "ns/elem");
    {
        long sink = 0;
        Stats sv = bench([&] { sink += sum_vector(vec); doNotOptimize(sink); },
                         /*batch=*/1, /*samples=*/200, /*warmup=*/5);
        Stats sl = bench([&] { sink += sum_list(head);  doNotOptimize(sink); },
                         /*batch=*/1, /*samples=*/200, /*warmup=*/5);
        Stats pv = per_element(sv, N), pl = per_element(sl, N);
        row("sum_vector (contiguous)", pv);
        row("sum_list   (pointer chase)", pl);
        std::printf("  p50 ratio list/vector = %.2fx    (%d samples x 1 full traversal)\n",
                    pv.p50 > 0 ? pl.p50 / pv.p50 : 0.0, sv.samples);
        std::printf("  correctness: sum_vector=%ld  sum_list=%ld  expected=%ld  %s\n",
                    sum_vector(vec), sum_list(head), expect_sum,
                    (sum_vector(vec) == expect_sum && sum_list(head) == expect_sum)
                        ? "OK" : "WRONG (both must equal expected)");
        std::printf("  bytes touched: vector %.1f MB, list %.1f MB\n",
                    static_cast<double>(N * static_cast<long>(sizeof(int))) / 1048576.0,
                    static_cast<double>(N * static_cast<long>(sizeof(Node))) / 1048576.0);
    }

    // ------------------------------------ TABLE 4: build / machine (2 pts)
    std::printf("\nTABLE 4 — build & method (state your machine in the README)\n");
    std::printf("  %-22s %s\n", "compiler",
#if defined(__clang__)
                "clang++ " __clang_version__
#elif defined(__GNUC__)
                "g++ " __VERSION__
#else
                "unknown"
#endif
    );
    std::printf("  %-22s %s\n", "optimisation",
#if defined(__OPTIMIZE__)
                "-O2/-O3 (release)  OK"
#else
                "-O0 (DEBUG BUILD — these numbers are fiction, rebuild with -O2)"
#endif
    );
    std::printf("  %-22s __cplusplus = %ldL\n", "language", static_cast<long>(__cplusplus));
    std::printf("  %-22s %s\n", "arch",
#if defined(__aarch64__)
                "arm64"
#elif defined(__x86_64__)
                "x86-64"
#else
                "other"
#endif
    );
    std::printf("  %-22s %s\n", "clock", "std::chrono::steady_clock (monotonic)");
    std::printf("  %-22s %s\n", "warm-up", "yes — whole batches before the first sample");
    std::printf("  %-22s %s\n", "reported", "p50 / p99 / p99.9 over samples, plus mean");
    std::printf("  %-22s %s\n", "dead-code guard", "doNotOptimize() on every result");
    std::printf("  %-22s %s\n", "machine", "TODO(4): put your CPU / RAM / OS in the README");

    std::printf("\n");
    return 0;
}
