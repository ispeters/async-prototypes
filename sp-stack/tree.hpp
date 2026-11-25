// A call tree with almost nothing in the bodies, to isolate what a call costs:
//
//   async long walk(node const* t) {
//     if (!t) co_return 0;                 // optionally: suspend first
//     long a = co_await walk(t->left);
//     long b = co_await walk(t->right);
//     co_return (a ^ (b << 1)) + t->value;
//   }
#pragma once

#include "mstack.hpp"

struct node {
    const node* left;
    const node* right;
    long        value;
};

extern bool walk_leaves_suspend; // a null child completes from the event loop

long       walk_sync(const node* t);
async_next walk_c(const node* t, long* out, async_cont* k, async_sp sp) noexcept;
ms::next   walk_m(const node* t, long* out, ms::cont* k) noexcept;

extern std::size_t walk_frame_bytes[2]; // cursor, machine: sizeof the hand-written record
