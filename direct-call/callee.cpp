// Separately compiled "async entry": receives args, a continuation, returns
// true if it completed inline (the handshake).
struct cont {
    void (*resume)(void*, int) noexcept;
    void* frame;
};
bool get_entry(int key, cont k, int* slot) noexcept {
    if (key & 1) {
        *slot = key * 2;
        return true;
    } // completed inline
    k.resume(k.frame, key * 2); // "later"
    return false;
}
