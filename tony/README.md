# stdexec under C++29, in Tony Tables (2026-11-17)

The code behind the artifact "stdexec under C++29: Tony Tables"
(https://claude.ai/artifact/77wmUPu5ceRp7LaSsJmbCg; `page.html` here is a copy).
Representative stdexec algorithms hand-written to the C++26 protocol beside the
same algorithms under the C++29 strawman: D4481's `operation_handle`, result
destinations as 4A, and D4471's async functions.

Every table on the page is cut verbatim from these files, between
`[[tony:NAME]]` markers. Names in `x29.hpp` are placeholders, not proposals.

| File | What |
|---|---|
| `abi4a.h` | The erased-call ABI under 4A, in C (`kv-dest/async_abi.h` with the record as one table pointer and no `out`) |
| `x29.hpp` | Library stand-in for the C++29 protocol: `operation_handle`, `set_value` with its default, `slot<T>`, `values<A, B>`, environments |
| `algos26.hpp` | C++26 column: `just`, `then`, `write_env`, `let_value`, `when_all`, `run_loop`, `sync_wait`, against stdexec |
| `algos29.hpp` | C++29 column: the same algorithms, plus `just_in_place` for section 4 |
| `erased.hpp` | The erased receiver and record under 4A (section 10) |
| `lowered.hpp` | Section 1's `cache::get` body, hand-lowered to `abi4a.h` (section 11) |
| `c_store.h`, `c_store.c` | A leaf in C behind the same ABI (section 12) |
| `tests26.cpp`, `tests29.cpp`, `fresh_body.inc` | Checks and move counts for both columns |
| `erased_test.cpp`, `lowered_test.cpp`, `c_test.cpp` | Checks for sections 10 to 12 |

Built with GCC 13.3 (under ASan and UBSan) and Clang 18.1; stdexec `main`
30acdb0.

```sh
g++ -std=c++23 -fsanitize=address,undefined -I$STDEXEC/include tests26.cpp -o t26 && ./t26
g++ -std=c++23 -fsanitize=address,undefined tests29.cpp -o t29 && ./t29
g++ -std=c++23 -fsanitize=address,undefined erased_test.cpp -o et && ./et
g++ -std=c++23 -fsanitize=address,undefined lowered_test.cpp -o lt && ./lt
gcc -std=c11 -pedantic -c c_store.c && g++ -std=c++23 c_test.cpp c_store.o -o ct && ./ct
```

Findings recorded on the page (observed unless marked):

- A sender may choose `set_value(rcvr, v)` (value by reference, as in C++26)
  instead of constructing at the destination; a defaulted CPO makes it free for
  receivers. With it, no measured case costs more moves than C++26, and values
  created fresh cost fewer (`then`: 1 against 2; `when_all` of two `then`s:
  0 against 2).
- `when_all` needs four protocol rules (asking for a destination early and
  abandoning it; a piecewise-constructible value aggregate; no first-error claim
  on the parent's storage, because it may alias the value elements; running the
  first child's handle before starting the second). Its in-place values work
  under 3A, 4A and 5A alike, so it isn't a 4A-only case.
- `std::optional` can't hand out storage, so the design needs a `slot<T>`-like
  library type; `sync_wait` still moves once at the boundary to synchronous code.
- The lowered body allocates its frame only on the path that can suspend (the
  hit path has none), which bears on `connect`-or-`start` frame allocation.
