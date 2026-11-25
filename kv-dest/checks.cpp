// Every variant, every workload, with the backing store completing inline and
// from the event loop: the right result, every frame popped, nothing left
// queued, the observers run once per call. Build with -DASYNC_CHECK.
#include "callers.hpp"

#include <cstdio>

int main() {
    int failures = 0, runs = 0;
    for (int v = -1; v < n_variants; ++v) {
        for (int w = 0; w < n_workloads; ++w) {
            for (bool deferred : {false, true}) {
                if (v < 0 && (deferred || w == w_or_else_value || w == w_or_else_error)) {
                    continue;
                }
                rig_base* r = make_rig(v, w, deferred);
                for (int i = 1; i <= 3; ++i) {
                    ++runs;
                    std::size_t got = r->call();
                    bool        ok  = got == expected(w) && run_pending() == 0 &&
                                      async_live_frames == 0;
                    if (w >= w_pass2_value && r->count() != i) {
                        ok = false;
                    }
                    if (!ok) {
                        ++failures;
                        std::printf("FAIL %-22s %-16s %s call %d: got %zu\n",
                                    variant_name(v),
                                    workload_names[w],
                                    deferred ? "deferred" : "inline",
                                    i,
                                    got);
                    }
                }
                delete r;
            }
        }
    }
    std::printf("%d runs, %d failures\n", runs, failures);
    return failures != 0;
}
