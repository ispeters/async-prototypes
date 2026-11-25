struct cont {
    void (*resume)(void*, int) noexcept;
    void* frame;
};
bool get_entry(int key, cont k, int* slot) noexcept; // declaration only

using entry_t = bool (*)(int, cont, int*) noexcept;

// (A) entry is a constant template argument of the operation state
template <entry_t Entry>
struct op_static {
    int         key;
    int         result;
    bool        done = false;
    static void resume(void* f, int v) noexcept {
        auto* s   = static_cast<op_static*>(f);
        s->result = v;
        s->done   = true;
    }
    void start() noexcept {
        if (Entry(key, cont{&resume, this}, &result)) {
            done = true;
        }
    }
};

// (B) entry is a run-time value, as in a type-erased object
struct op_dynamic {
    entry_t     entry;
    int         key;
    int         result;
    bool        done = false;
    static void resume(void* f, int v) noexcept {
        auto* s   = static_cast<op_dynamic*>(f);
        s->result = v;
        s->done   = true;
    }
    void start() noexcept {
        if (entry(key, cont{&resume, this}, &result)) {
            done = true;
        }
    }
};

[[gnu::noinline]] int call_static(int key) {
    op_static<&get_entry> op{key, 0};
    op.start();
    return op.result;
}
[[gnu::noinline]] int call_dynamic(entry_t e, int key) {
    op_dynamic op{e, key, 0};
    op.start();
    return op.result;
}
int main(int argc, char**) {
    return call_static(argc) + call_dynamic(&get_entry, argc) == 4 ? 0 : 1;
}
