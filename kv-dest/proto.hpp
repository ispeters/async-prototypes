// The ten variants of the protocol: five ways of telling a callee where to
// construct each completion's payload (rows), by two ways of telling the
// caller which completion happened (columns).
//
// Rows
//   1  single slot: `out` points at one union in the caller (kv-handle as is)
//   2  pointer table at `out`: `out` points at one pointer per payload
//   3  pointer table in the record: the destinations live in the caller's
//      continuation record; there is no `out`
//   4  static function table: the record points at a static table of
//      functions; the destination of a payload is what its function returns
//   5  split: in statically composed code the destination is a member template
//      of a type the sender knows; at an erased call it is row 2's table
//
// Columns
//   A  one `resume` plus `how`: an entry returns {record, how} (kv-handle)
//   B  one resume function per completion: the record holds one slot per
//      completion, and an entry returns the address of a slot (W-149)
//
// Every variant provides:
//   rec                 the record a frame derives from
//   handle              what an entry and a resume return
//   dst<T>              what an entry is passed for the destinations
//   none()              the handle meaning "nothing is ready"
//   complete<I>(k)      the handle meaning "k's completion I"
//   dest<I, T>(d, k)    where to construct completion I's payload
//   wire(f)             set up frame f's record; f has `after<I>()` and,
//                       for rows 3 and 4, `my_dest<I>()`
//   fall_through(h, f)  if h is f's own continuation, run it in place
//   run(h)              the loop at the bottom of the machine stack
#pragma once

#include "common.hpp"

#include <cstdint>
#include <type_traits>

enum : int { VAL = ASYNC_VALUE, ERR = ASYNC_ERROR, STP = ASYNC_STOPPED, PENDING = ASYNC_PENDING };
inline constexpr int NC = 3;  // declared completions: value, error, stopped
inline constexpr int NP = 2;  // those that carry a payload

struct ptab {  // rows 2 and 5: one destination per payload
  void* p[NP];
};
struct nodst {};  // rows 3 and 4: the destinations travel in the record

// Runs completion `how` of frame f. Column A needs it wherever a `how` arrives.
template <class H, class F>
[[gnu::always_inline]] inline H dispatch(F* f, int how) noexcept {
  if (how == VAL) return f->template after<VAL>();
  if (how == ERR) return f->template after<ERR>();
  return f->template after<STP>();
}

// ============================================================================
// Column A: one resume plus `how`
// ============================================================================
struct recA;
struct hA {
  recA* next;
  int how;
};
struct recA {
  hA (*resume)(recA*, int) noexcept;
};
struct recA3 : recA {  // row 3: the destinations beside the resume function
  void* p[NP];
};

template <class F>
hA resumeA(recA* r, int how) noexcept {
  return dispatch<hA>(static_cast<F*>(r), how);
}

template <class Rec>
struct colA {
  using rec = Rec;
  using handle = hA;
  static constexpr char col = 'A';
  static hA none() noexcept { return {nullptr, PENDING}; }
  template <int I>
  static hA complete(Rec* k) noexcept {
    return {k, I};
  }
  template <class F>
  [[gnu::always_inline]] static hA fall_through(hA h, F* f) noexcept {
    if (h.next == static_cast<recA*>(f)) return dispatch<hA>(f, h.how);
    return h;
  }
  static void run(hA h) noexcept {
    while (h.next) h = h.next->resume(h.next, h.how);
  }
};

struct V1A : colA<recA> {
  static constexpr int row = 1;
  static constexpr char const* name = "1A single slot";
  template <class T>
  using dst = slot<T>*;
  template <int I, class T>
  static void* dest(slot<T>* d, recA*) noexcept {
    if constexpr (I == VAL) return &d->value;
    else return &d->error;
  }
  template <class F>
  static void wire(F* f) noexcept {
    f->resume = &resumeA<F>;
  }
};

struct V2A : colA<recA> {
  static constexpr int row = 2;
  static constexpr char const* name = "2A table at out";
  template <class T>
  using dst = ptab*;
  template <int I, class T>
  static void* dest(ptab* d, recA*) noexcept {
    return d->p[I];
  }
  template <class F>
  static void wire(F* f) noexcept {
    f->resume = &resumeA<F>;
  }
};

struct V3A : colA<recA3> {
  static constexpr int row = 3;
  static constexpr char const* name = "3A table in record";
  template <class T>
  using dst = nodst;
  template <int I, class T>
  static void* dest(nodst, recA3* k) noexcept {
    return k->p[I];
  }
  template <class F>
  static void wire(F* f) noexcept {
    f->resume = &resumeA<F>;
    f->p[VAL] = f->template my_dest<VAL>();
    f->p[ERR] = f->template my_dest<ERR>();
  }
};

// Row 4, column A: the record is one pointer to a static table holding the
// resume function and one destination function per payload.
struct recA4;
struct hA4 {
  recA4* next;
  int how;
};
struct tblA4 {
  hA4 (*resume)(recA4*, int) noexcept;
  void* (*dest[NP])(recA4*) noexcept;
};
struct recA4 {
  tblA4 const* t;
};
template <class F>
hA4 resumeA4(recA4* r, int how) noexcept {
  return dispatch<hA4>(static_cast<F*>(r), how);
}
template <class F, int I>
void* destA4(recA4* r) noexcept {
  return static_cast<F*>(r)->template my_dest<I>();
}
template <class F>
inline constexpr tblA4 tblA4_for{&resumeA4<F>, {&destA4<F, VAL>, &destA4<F, ERR>}};

struct V4A {
  static constexpr int row = 4;
  static constexpr char col = 'A';
  static constexpr char const* name = "4A static fn table";
  using rec = recA4;
  using handle = hA4;
  template <class T>
  using dst = nodst;
  static hA4 none() noexcept { return {nullptr, PENDING}; }
  template <int I>
  static hA4 complete(recA4* k) noexcept {
    return {k, I};
  }
  template <int I, class T>
  static void* dest(nodst, recA4* k) noexcept {
    return k->t->dest[I](k);
  }
  template <class F>
  static void wire(F* f) noexcept {
    f->t = &tblA4_for<F>;
  }
  template <class F>
  [[gnu::always_inline]] static hA4 fall_through(hA4 h, F* f) noexcept {
    if (h.next == static_cast<recA4*>(f)) return dispatch<hA4>(f, h.how);
    return h;
  }
  static void run(hA4 h) noexcept {
    while (h.next) h = h.next->t->resume(h.next, h.how);
  }
};

struct V5A : V2A {
  static constexpr int row = 5;
  static constexpr char const* name = "5A split";
};

// ============================================================================
// Column B: one resume function per completion
// ============================================================================
// The record is an array with one unit per completion; a handle is the
// address of a unit, so the function it holds knows which completion it is.
template <class U, class Rec>
struct colB {
  using rec = Rec;
  using handle = U*;
  static constexpr char col = 'B';
  static U* none() noexcept { return nullptr; }
  template <int I>
  static U* complete(Rec* k) noexcept {
    return &k->s[I];
  }
  template <class F>
  [[gnu::always_inline]] static U* fall_through(U* h, F* f) noexcept {
    Rec* r = static_cast<Rec*>(f);
    if (h == &r->s[VAL]) return f->template after<VAL>();
    if (h == &r->s[ERR]) return f->template after<ERR>();
    if (h == &r->s[STP]) return f->template after<STP>();
    return h;
  }
};

// Rows 1, 2, 5: a unit is a resume function.
struct rsB {
  rsB* (*run)(rsB*) noexcept;
};
struct recB {
  rsB s[NC];
};
template <class F, int I>
rsB* runB(rsB* h) noexcept {
  return static_cast<F*>(reinterpret_cast<recB*>(h - I))->template after<I>();
}
template <class F>
inline void wireB(F* f) noexcept {
  f->s[VAL].run = &runB<F, VAL>;
  f->s[ERR].run = &runB<F, ERR>;
  f->s[STP].run = &runB<F, STP>;
}
struct colB1 : colB<rsB, recB> {
  static void run(rsB* h) noexcept {
    while (h) h = h->run(h);
  }
};

struct V1B : colB1 {
  static constexpr int row = 1;
  static constexpr char const* name = "1B single slot";
  template <class T>
  using dst = slot<T>*;
  template <int I, class T>
  static void* dest(slot<T>* d, recB*) noexcept {
    if constexpr (I == VAL) return &d->value;
    else return &d->error;
  }
  template <class F>
  static void wire(F* f) noexcept {
    wireB(f);
  }
};

struct V2B : colB1 {
  static constexpr int row = 2;
  static constexpr char const* name = "2B table at out";
  template <class T>
  using dst = ptab*;
  template <int I, class T>
  static void* dest(ptab* d, recB*) noexcept {
    return d->p[I];
  }
  template <class F>
  static void wire(F* f) noexcept {
    wireB(f);
  }
};

struct V5B : V2B {
  static constexpr int row = 5;
  static constexpr char const* name = "5B split";
};

// Row 3: a unit is a resume function and its completion's destination.
struct rpB {
  rpB* (*run)(rpB*) noexcept;
  void* dest;
};
struct recB3 {
  rpB s[NC];
};
template <class F, int I>
rpB* runB3(rpB* h) noexcept {
  return static_cast<F*>(reinterpret_cast<recB3*>(h - I))->template after<I>();
}
struct V3B : colB<rpB, recB3> {
  static constexpr int row = 3;
  static constexpr char const* name = "3B table in record";
  template <class T>
  using dst = nodst;
  static void run(rpB* h) noexcept {
    while (h) h = h->run(h);
  }
  template <int I, class T>
  static void* dest(nodst, recB3* k) noexcept {
    return k->s[I].dest;
  }
  template <class F>
  static void wire(F* f) noexcept {
    f->s[VAL] = {&runB3<F, VAL>, f->template my_dest<VAL>()};
    f->s[ERR] = {&runB3<F, ERR>, f->template my_dest<ERR>()};
    f->s[STP] = {&runB3<F, STP>, nullptr};
  }
};

// Row 4: a unit points at a static entry holding the resume function and the
// destination function of its completion.
struct eB;
using sB = eB const*;
struct eB {
  sB* (*run)(sB*) noexcept;
  void* (*dest)(sB*) noexcept;
};
struct recB4 {
  sB s[NC];
};
template <class F, int I>
sB* runB4(sB* h) noexcept {
  return static_cast<F*>(reinterpret_cast<recB4*>(h - I))->template after<I>();
}
template <class F, int I>
void* destB4(sB* h) noexcept {
  return static_cast<F*>(reinterpret_cast<recB4*>(h - I))->template my_dest<I>();
}
template <class F>
inline constexpr eB tblB4_for[NC] = {
    {&runB4<F, VAL>, &destB4<F, VAL>}, {&runB4<F, ERR>, &destB4<F, ERR>}, {&runB4<F, STP>, nullptr}};

struct V4B : colB<sB, recB4> {
  static constexpr int row = 4;
  static constexpr char const* name = "4B static fn table";
  template <class T>
  using dst = nodst;
  static void run(sB* h) noexcept {
    while (h) h = (*h)->run(h);
  }
  template <int I, class T>
  static void* dest(nodst, recB4* k) noexcept {
    return k->s[I]->dest(&k->s[I]);
  }
  template <class F>
  static void wire(F* f) noexcept {
    f->s[VAL] = &tblB4_for<F>[VAL];
    f->s[ERR] = &tblB4_for<F>[ERR];
    f->s[STP] = &tblB4_for<F>[STP];
  }
};

template <class V>
inline constexpr bool by_record = V::row == 3 || V::row == 4;
template <class V>
inline constexpr bool by_table = V::row == 2 || V::row == 5;

template <class... Vs>
struct tlist {};
using all_variants = tlist<V1A, V2A, V3A, V4A, V5A, V1B, V2B, V3B, V4B, V5B>;
inline constexpr int n_variants = 10;
