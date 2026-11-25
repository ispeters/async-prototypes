struct cont {
    void (*resume)(void*, int) noexcept;
    void* frame;
};
struct store {
    virtual bool get_entry(int key, cont k, int* slot) noexcept = 0; // async entry
    virtual int  get_sync(int key) noexcept                     = 0; // sync baseline
    virtual ~store()                                            = default;
};
struct remote final : store {
    bool get_entry(int key, cont k, int* slot) noexcept override;
    int  get_sync(int key) noexcept override;
};
