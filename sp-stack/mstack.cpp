#include "mstack.hpp"

#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

// Both switches save the callee-saved registers on the stack they were called
// on, keep that stack's pointer in rbp, and restore from it. rbp survives
// because a function that leaves its frame still restores its caller's rbp.
asm(R"(
        .text
        .globl  ms_enter
        .type   ms_enter, @function
ms_enter:                       # rdi = arg, rsi = sp, rdx = fn
        .cfi_startproc
        endbr64
        push    %rbp
        .cfi_def_cfa_offset 16
        .cfi_offset %rbp, -16
        mov     %rsp, %rbp
        .cfi_def_cfa_register %rbp
        push    %rbx
        push    %r12
        push    %r13
        push    %r14
        push    %r15
        .cfi_offset %rbx, -24
        .cfi_offset %r12, -32
        .cfi_offset %r13, -40
        .cfi_offset %r14, -48
        .cfi_offset %r15, -56
        mov     %rsi, %rsp      # onto the strand's stack
        call    *%rdx           # fn(arg) -> rax:rdx
        lea     -40(%rbp), %rsp # back, wherever fn left the stack pointer
        pop     %r15
        pop     %r14
        pop     %r13
        pop     %r12
        pop     %rbx
        pop     %rbp
        .cfi_def_cfa %rsp, 8
        ret
        .cfi_endproc
        .size   ms_enter, .-ms_enter

        .globl  ms_run
        .type   ms_run, @function
ms_run:                         # rdi = cont, esi = how
        .cfi_startproc
        endbr64
        push    %rbp
        .cfi_def_cfa_offset 16
        .cfi_offset %rbp, -16
        mov     %rsp, %rbp
        .cfi_def_cfa_register %rbp
        push    %rbx
        push    %r12
        push    %r13
        push    %r14
        push    %r15
        .cfi_offset %rbx, -24
        .cfi_offset %r12, -32
        .cfi_offset %r13, -40
        .cfi_offset %r14, -48
        .cfi_offset %r15, -56
1:      test    %rdi, %rdi
        jz      2f
        mov     8(%rdi), %rsp   # cont::sp: pops whatever was below this frame
        call    *(%rdi)         # cont::resume(cont, how) -> rax:rdx
        mov     %rax, %rdi
        mov     %edx, %esi
        jmp     1b
2:      lea     -40(%rbp), %rsp
        pop     %r15
        pop     %r14
        pop     %r13
        pop     %r12
        pop     %rbx
        pop     %rbp
        .cfi_def_cfa %rsp, 8
        ret
        .cfi_endproc
        .size   ms_run, .-ms_run
        .section .note.GNU-stack,"",@progbits
        .text
)");

namespace ms {

static_assert(offsetof(cont, resume) == 0 && offsetof(cont, sp) == 8);

void* lowest_deferred_sp = nullptr;
void (*after_pending)() noexcept = nullptr;

static cont* head;
static cont** tail = &head;

void defer(cont* k) noexcept {
  if (!lowest_deferred_sp || k->sp < lowest_deferred_sp) lowest_deferred_sp = k->sp;
  k->link = nullptr;
  *tail = k;
  tail = &k->link;
}

int run_pending() noexcept {
  int n = 0;
  while (head) {
    cont* k = head;
    head = k->link;
    if (!head) tail = &head;
    ms_run(k, ASYNC_VALUE);
    ++n;
  }
  return n;
}

// ---- providers -------------------------------------------------------------
static std::size_t page() noexcept {
  static std::size_t p = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  return p;
}

stack mmap_provider::map(std::size_t size) noexcept {
  std::size_t g = page();
  void* p = mmap(nullptr, size + g, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) return {};
  if (mprotect(static_cast<char*>(p) + g, size, PROT_READ | PROT_WRITE) != 0) {
    munmap(p, size + g);
    return {};
  }
  return {static_cast<char*>(p) + g, size};
}

void mmap_provider::unmap(stack s) noexcept { munmap(s.base - page(), s.size + page()); }

stack mmap_provider::acquire() noexcept {
  if (pooled && !free_.empty()) {
    stack s = free_.back();
    free_.pop_back();
    return s;
  }
  stack s = map(size);
  if (!s.base) std::abort();
  ++mapped;
  return s;
}

void mmap_provider::release(stack s) noexcept {
  if (pooled) free_.push_back(s);
  else unmap(s);
}

mmap_provider::~mmap_provider() {
  for (stack s : free_) unmap(s);
}

stack arena_provider::acquire() noexcept {
  if (!free_.empty()) {
    stack s = free_.back();
    free_.pop_back();
    return s;
  }
  if (next_ == count) std::abort();
  return {buf + chunk * next_++, chunk};
}

void fill(stack s) noexcept { std::memset(s.base, 0xA5, s.size); }

std::size_t high_water(stack s) noexcept {
  std::size_t i = 0;
  while (i < s.size && static_cast<unsigned char>(s.base[i]) == 0xA5) ++i;
  return s.size - i;
}

}  // namespace ms
