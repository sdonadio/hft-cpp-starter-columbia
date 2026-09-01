#pragma once
#include <cstdint>
// HW11 — a high-performance FIX parser (SOH-delimited tag=value; tags 11/55/54/38/44).
struct NewOrder {
    const char* clordid; int clordid_len;   // tag 11
    char symbol[16]; char side;             // tags 55, 54
    uint32_t qty; double price;             // tags 38, 44
};
// Single pass, no per-message allocation. Return false on a malformed message.
inline bool parse_new_order(const char* buf, int len, NewOrder& out) {
    (void)buf; (void)len; (void)out;
    return false;   // TODO(student)
}
