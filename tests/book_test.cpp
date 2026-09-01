// book_test.cpp — HW7. Contract: order_book.hpp defines
//   struct Book { void add(uint64_t id,char side,double px,uint32_t qty);
//                 void cancel(uint64_t id); double best_bid() const; double best_ask() const; };
//   struct SymMap { void put(const char* sym,uint64_t id); uint64_t get(const char* sym) const; };
// side 'B'=buy/bid, 'S'=sell/ask. SymMap.get returns (uint64_t)-1 if absent.
#include <cstdint>
#include <iostream>
#include <string>
#include "order_book.hpp"
#include "bench.hpp"

static void R(const char* k, bool ok, const std::string& m) {
    std::cout << "RESULT|" << k << "|" << (ok ? "pass" : "fail") << "|" << m << "\n";
}
static bool eq(double a, double b) { return a > b ? a - b < 1e-6 : b - a < 1e-6; }

int main() {
    // book correctness
    {
        Book b;
        b.add(1, 'B', 100.00, 5);
        b.add(2, 'B', 100.02, 3);   // best bid
        b.add(3, 'S', 100.06, 4);
        b.add(4, 'S', 100.04, 2);   // best ask
        bool ok = eq(b.best_bid(), 100.02) && eq(b.best_ask(), 100.04);
        b.cancel(2);                 // remove best bid -> 100.00
        ok = ok && eq(b.best_bid(), 100.00);
        b.cancel(4);                 // remove best ask -> 100.06
        ok = ok && eq(b.best_ask(), 100.06);
        R("book_bbo", ok, ok ? "best bid/ask + cancel correct" : "wrong best bid/ask after add/cancel");
    }
    // symmap correctness
    {
        SymMap m;
        m.put("AAPL", 10); m.put("MSFT", 20); m.put("NVDA", 30);
        m.put("SPY", 40); m.put("BRK.B", 99);            // has a dot, <=16 chars
        bool ok = m.get("AAPL") == 10 && m.get("NVDA") == 30 && m.get("BRK.B") == 99;
        ok = ok && m.get("TSLA") == (uint64_t)-1;         // absent
        m.put("AAPL", 11);                                 // overwrite
        ok = ok && m.get("AAPL") == 11;
        R("symmap", ok, ok ? "put/get/overwrite/absent correct" : "map returned wrong id");
    }
    // metrics
    {
        Book b;
        for (uint64_t i = 0; i < 2000; ++i) {
            b.add(i, (i & 1) ? 'B' : 'S', 100.0 + ((i * 7) % 50) * 0.01, 1 + (i % 9));
        }
        double ns = ns_per_op([&] { double x = b.best_bid() + b.best_ask(); doNotOptimize(x); }, 2000000);
        std::cout << "METRIC|book_bbo_ns_per_op|" << ns << "\n";
        SymMap m;
        const char* syms[] = {"AAPL","MSFT","NVDA","TSLA","AMZN","GOOGL","META","SPY"};
        for (uint64_t i = 0; i < 8; ++i) m.put(syms[i], i);
        int k = 0;
        double ns2 = ns_per_op([&] { uint64_t v = m.get(syms[(k++) & 7]); doNotOptimize(v); }, 2000000);
        std::cout << "METRIC|symmap_get_ns_per_op|" << ns2 << "\n";
    }
    return 0;
}
