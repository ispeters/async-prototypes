#include "async_abi.h"

size_t async_high_water;
size_t async_slabs_allocated;
long   async_live_frames;

static async_task*  head;
static async_task** tail = &head;

void async_defer(async_task* t) {
    t->next = NULL;
    *tail   = t;
    tail    = &t->next;
}

int async_run_pending(void) {
    int n = 0;
    while (head) {
        async_task* t = head;
        head          = t->next;
        if (!head)
            tail = &head;
        async_run(t->run(t));
        ++n;
    }
    return n;
}
