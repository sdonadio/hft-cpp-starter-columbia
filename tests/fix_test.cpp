// fix_test.cpp — HW11. Contract: fix_parser.hpp defines
//   struct NewOrder { const char* clordid; int clordid_len; char symbol[16];
//                     char side; uint32_t qty; double price; };
//   bool parse_new_order(const char* buf, int len, NewOrder& out);
// SOH ('\x01') delimited tag=value. Fields: 11 ClOrdID, 55 Symbol, 54 Side,
// 38 OrderQty, 44 Price.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include "fix_parser.hpp"
#include "bench.hpp"

static void R(const char* k, bool ok, const std::string& m) {
    std::cout << "RESULT|" << k << "|" << (ok ? "pass" : "fail") << "|" << m << "\n";
}
// a well-formed NewOrder-single (35=D)
static const char MSG[] =
    "8=FIX.4.2\x01" "9=76\x01" "35=D\x01" "11=ORD123\x01" "55=AAPL\x01"
    "54=1\x01" "38=100\x01" "44=185.50\x01" "10=072\x01";

int main() {
    {
        NewOrder o; std::memset(&o, 0, sizeof(o));
        bool ok = parse_new_order(MSG, (int)sizeof(MSG) - 1, o);
        ok = ok && std::strncmp(o.symbol, "AAPL", 4) == 0;
        ok = ok && o.side == '1';
        ok = ok && o.qty == 100;
        ok = ok && (o.price > 185.49 && o.price < 185.51);
        ok = ok && o.clordid_len == 6 && std::strncmp(o.clordid, "ORD123", 6) == 0;
        R("fix_parse", ok, ok ? "symbol/side/qty/price/clordid parsed" : "one or more fields wrong");
    }
    // metric: parse the same message many times (batch)
    {
        NewOrder o;
        double ns = ns_per_op([&] {
            bool r = parse_new_order(MSG, (int)sizeof(MSG) - 1, o);
            doNotOptimize(r); doNotOptimize(o.qty);
        }, 2000000);
        std::cout << "METRIC|fix_parse_ns_per_op|" << ns << "\n";
    }
    return 0;
}
