#include "iface.hpp"
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
        if ((self.*Entry)(key, cont{&resume, this}, &result))
            done = true;
    }
};
// entry as a plain function that makes a qualified (non-virtual) call
template <auto Thunk, class Obj>
struct op_thunk {
    Obj&        self;
    int         key;
    int         result = 0;
    bool        done   = false;
    static void resume(void* f, int v) noexcept {
        auto* s   = static_cast<op_thunk*>(f);
        s->result = v;
        s->done   = true;
    }
    void start() noexcept {
        if (Thunk(self, key, cont{&resume, this}, &result))
            done = true;
    }
};
inline bool remote_entry(remote& r, int key, cont k, int* slot) noexcept {
    return r.remote::get_entry(key, k, slot);
}
#define NI [[gnu::noinline]]
// 2b. final class, entry named through the final class itself
NI int async_final_own(remote& r, int key) {
    op<&remote::get_entry, remote> o{r, key};
    o.start();
    return o.result;
}
// 2c. final class, entry is a function making a qualified call
NI int async_final_thunk(remote& r, int key) {
    op_thunk<&remote_entry, remote> o{r, key};
    o.start();
    return o.result;
}
