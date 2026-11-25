#include "iface.hpp"
// Operation state: the entry is a constant pointer to a (virtual) member function.
template <auto Entry, class Obj>
struct op {
    Obj&        self;
    int         key;
    int         result = 0;
    bool        done   = false;
    static void resume(void* f, int v) noexcept {
        auto* s   = static_cast<op*>(f);
        s->result = v;
        s->done   = true;
    }
    void start() noexcept {
        if ((self.*Entry)(key, cont{&resume, this}, &result)) {
            done = true;
        }
    }
};
#define NI [[gnu::noinline]]
// 1. dynamic type unknown
NI int async_unknown(store& s, int key) {
    op<&store::get_entry, store> o{s, key};
    o.start();
    return o.result;
}
NI int sync_unknown(store& s, int key) { return s.get_sync(key); }
// 2. static type is a final class; entry still named through the base
NI int async_final(remote& r, int key) {
    op<&store::get_entry, remote> o{r, key};
    o.start();
    return o.result;
}
NI int sync_final(remote& r, int key) { return r.get_sync(key); }
// 3. object constructed in view, used through a base reference
NI int async_local(int key) {
    remote                       r;
    store&                       s = r;
    op<&store::get_entry, store> o{s, key};
    o.start();
    return o.result;
}
NI int sync_local(int key) {
    remote r;
    store& s = r;
    return s.get_sync(key);
}
int main(int argc, char**) {
    remote r;
    return async_unknown(r, argc) + sync_unknown(r, argc) + async_final(r, argc) +
                       sync_final(r, argc) + async_local(argc) + sync_local(argc) ==
                   12
               ? 0
               : 1;
}
