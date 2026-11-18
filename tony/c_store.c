#include "c_store.h"

#include <errno.h>
#include <string.h>

struct c_store {
  char const* keys[2];
  char const* values[2];
};
static c_store const store = {{"a", "b"}, {"apple", "banana"}};
c_store const* c_store_default(void) { return &store; }

static char const* find(c_store const* s, char const* key) {
  for (size_t i = 0; i < sizeof s->keys / sizeof s->keys[0]; ++i) {
    if (strcmp(s->keys[i], key) == 0) return s->values[i];
  }
  return NULL;
}

/* A queue of deferred work, standing in for an event loop. */
typedef struct task {
  async_next (*run)(struct task* self);
  struct task* next;
} task;
static task* head;
static task** tail = &head;
static void defer(task* t) {
  t->next = NULL;
  *tail = t;
  tail = &t->next;
}
int c_store_run_pending(void) {
  int n = 0;
  while (head) {
    task* t = head;
    head = t->next;
    if (!head) tail = &head;
    async_run(t->run(t));
    ++n;
  }
  return n;
}

/* [[tony:c_body]] */
/* What lives across the suspension: our frame on the async stack. */
typedef struct get_frame {
  task task; /* first, so the loop can run it */
  c_store const* store;
  char const* key;
  async_cont* k; /* our caller's record */
} get_frame;

/* Runs from the event loop, after the entry returned "pending". */
static async_next get_later(task* t) {
  get_frame* f = (get_frame*)t;
  async_cont* k = f->k;
  char const* found = find(f->store, f->key);

  c_value* v = async_destination_of(k, C_VALUE)(k); /* ask: where? */
  v->data = found;
  v->len = found ? strlen(found) : 0;
  /* the frame is popped: our caller's top never moved */
  return async_complete(k, C_VALUE); /* returned, not called */
}

async_next c_store_get_entry(c_store const* s, char const* key,
                             async_cont* k, async_sp sp) {
  if (key[0] == '\0') { /* an empty key is an error, reported inline */
    int* e = async_destination_of(k, C_ERROR)(k);
    *e = EINVAL;
    return async_complete(k, C_ERROR);
  }
  get_frame* f = async_push(&sp, sizeof *f);
  f->task.run = get_later;
  f->store = s;
  f->key = key;
  f->k = k;
  defer(&f->task);
  async_next pending = {NULL, ASYNC_PENDING};
  return pending;
}
/* [[/tony]] */
