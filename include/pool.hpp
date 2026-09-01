#pragma once
#include <cstddef>
// HW4 — a high-performance fixed-size object pool (O(1) alloc/free, placement new).
struct Pool {
    Pool(std::size_t obj_size, std::size_t capacity) { (void)obj_size; (void)capacity;
        // TODO(student): back this with ONE pre-allocated buffer + a free-list.
    }
    void* alloc() { return nullptr;               // TODO(student): pop a free slot, O(1)
    }
    void  free(void* p) { (void)p;                // TODO(student): return the slot, O(1)
    }
};
